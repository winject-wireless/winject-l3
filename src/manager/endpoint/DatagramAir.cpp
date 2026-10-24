#include "endpoint/DatagramAir.h"

#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <algorithm>
#include <chrono>
#include <string.h>
#include <utility>
#include <vector>

namespace
{
constexpr int k_rx_poll_ms = 10;

bfc::sized_buffer make_pkt(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return {};
    }
    bfc::sized_buffer pkt(len);
    memcpy(pkt.data(), data, len);
    return pkt;
}
}  // namespace

namespace winject
{

template <typename F>
void DatagramAir::queue_tx(F&& queue)
{
    std::shared_ptr<Reactor> reactor;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        if (!running_)
        {
            return;
        }
        queue();
        if (timer_wake_pending_)
        {
            timer_wake_pending_ = false;
            reactor = reactor_;
        }
    }
    if (reactor)
    {
        reactor->wake_up();
    }
}

DatagramAir::~DatagramAir()
{
    stop();
}

bool DatagramAir::start(std::shared_ptr<BoundedLossyQueue> tx_queue,
                        DeliverApp deliver_app,
                        std::shared_ptr<Reactor> reactor,
                        const UpstreamConfig& cfg, uint8_t route_bus,
                        UpstreamStats* tx_stats, std::mutex* tx_stats_mu,
                        const char* log_name)
{
    stop();
    std::lock_guard<std::mutex> lock(tx_mu_);
    fec_tx_.disable();
    fec_rx_ = RsBlockErasure{};
    if (!reactor || !tx_queue || tx_queue->capacity() == 0 || route_bus == 0 ||
        tx_stats == nullptr || tx_stats_mu == nullptr ||
        cfg.fec_timeout_ms < 0 ||
        (cfg.fec_type != FecType::none &&
         cfg.fec_type != FecType::RsBlockErasure) ||
        (cfg.fec_type == FecType::RsBlockErasure &&
         !fec_tx_.init(cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms)))
    {
        return false;
    }
    tx_queue_ = std::move(tx_queue);
    route_bus_ = route_bus;
    tx_stats_ = tx_stats;
    tx_stats_mu_ = tx_stats_mu;
    deliver_app_ = std::move(deliver_app);
    reactor_ = std::move(reactor);
    log_name_ = log_name && log_name[0] ? log_name : "upstream";
    fec_timeout_ms_ = cfg.fec_timeout_ms;
    timer_guard_ = std::make_shared<TimerGuard>();
    timer_guard_->owner = this;
    running_ = true;
    return true;
}

void DatagramAir::stop()
{
    // start/stop themselves are serialized by the endpoint lifecycle owner.
    auto guard = timer_guard_;
    std::unique_lock<std::mutex> fence;
    if (guard)
    {
        fence = std::unique_lock<std::mutex>(guard->mutex);
        guard->owner = nullptr;
    }
    std::lock_guard<std::mutex> lock(tx_mu_);
    running_ = false;
    cancel_fec_timer_locked();
    if (rx_timer_armed_ && reactor_)
    {
        reactor_->get_timer().cancel(rx_timer_id_);
    }
    rx_timer_armed_ = false;
    reactor_.reset();
    tx_queue_.reset();
    route_bus_ = 0;
    tx_stats_ = nullptr;
    tx_stats_mu_ = nullptr;
    deliver_app_ = nullptr;
    fec_tx_.disable();
}

void DatagramAir::push_tx(bfc::sized_buffer pkt, bool is_fec)
{
    if (pkt.empty() || route_bus_ == 0 || tx_stats_ == nullptr ||
        tx_stats_mu_ == nullptr)
    {
        return;
    }
    const auto capacity = tx_queue_->capacity();
    const size_t size = pkt.size();
    bfc::sized_buffer framed(k_slot_prefix_len + size);
    size_t framed_len = 0;
    {
        std::lock_guard<std::mutex> lock(*tx_stats_mu_);
        if (!stamp_slot_payload(tx_stats_, route_bus_, is_fec,
                                reinterpret_cast<const uint8_t*>(pkt.data()),
                                size, reinterpret_cast<uint8_t*>(framed.data()),
                                framed.size(), &framed_len))
        {
            return;
        }
    }
    auto shared =
        bfc::shared_sized_buffer::copy_from(framed.data(), framed_len);
    const auto before = tx_queue_->push(std::move(shared));
    if (capacity == 0 || before >= capacity)
    {
        tx_queue_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_fec)
    {
        fec_air_tx_bytes_.fetch_add(size, std::memory_order_relaxed);
        fec_air_tx_packets_.fetch_add(1, std::memory_order_relaxed);
    }
}

void DatagramAir::enqueue_fec(const std::vector<std::vector<uint8_t>>& shards)
{
    for (const auto& shard : shards)
    {
        push_tx(make_pkt(shard.data(), shard.size()), true);
    }
}

void DatagramAir::deliver(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    DeliverApp callback;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        if (!running_)
        {
            return;
        }
        callback = deliver_app_;
    }
    if (!callback)
    {
        return;
    }
    app_rx_bytes_.fetch_add(len, std::memory_order_relaxed);
    app_rx_packets_.fetch_add(1, std::memory_order_relaxed);
    callback(data, len);
}

void DatagramAir::accept_app_datagram(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    queue_tx(
        [&]()
        {
            app_tx_bytes_.fetch_add(len, std::memory_order_relaxed);
            app_tx_packets_.fetch_add(1, std::memory_order_relaxed);
            const size_t limit = fec_tx_.enabled()
                                     ? RsBlockErasure::max_original()
                                     : k_stream_payload_max;
            if (len > limit)
            {
                app_rx_oversize_pkt_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (fec_tx_.enabled())
            {
                std::vector<std::vector<uint8_t>> shards;
                fec_tx_.push_app(data, len, &shards);
                enqueue_fec(shards);
                sync_fec_timer_after_push();
            }
            else
            {
                push_tx(make_pkt(data, len), false);
            }
        });
}

void DatagramAir::on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec)
{
    if (pkt.empty() || pkt.size() > k_stream_payload_max)
    {
        return;
    }
    if (!is_fec)
    {
        deliver(pkt.u8(), pkt.size());
        return;
    }
    fec_vendor::fec_rx_payload_list payloads;
    std::shared_ptr<Reactor> reactor;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        if (!running_)
        {
            return;
        }
        fec_air_rx_bytes_.fetch_add(pkt.size(), std::memory_order_relaxed);
        fec_air_rx_packets_.fetch_add(1, std::memory_order_relaxed);
        if (!fec_tx_.enabled())
        {
            fec_air_rx_unexpected_.fetch_add(1, std::memory_order_relaxed);
        }
        fec_rx_.push_air(std::move(pkt), &payloads);
        arm_rx_timer();
        if (timer_wake_pending_)
        {
            timer_wake_pending_ = false;
            reactor = reactor_;
        }
    }
    if (reactor)
    {
        reactor->wake_up();
    }
    for (const auto& payload : payloads)
    {
        deliver(payload.u8(), payload.size());
    }
}

void DatagramAir::cancel_fec_timer_locked()
{
    ++tx_timer_generation_;
    if (fec_timer_armed_ && reactor_)
    {
        reactor_->get_timer().cancel(fec_timer_id_);
    }
    fec_timer_armed_ = false;
}

void DatagramAir::sync_fec_timer_after_push()
{
    cancel_fec_timer_locked();
    std::chrono::steady_clock::time_point deadline;
    if (!reactor_ || !fec_tx_.next_deadline(&deadline))
    {
        return;
    }
    const auto delay = std::chrono::duration_cast<std::chrono::microseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count();
    const auto generation = tx_timer_generation_;
    auto guard = timer_guard_;
    fec_timer_id_ = reactor_->get_timer().wait_us(
        std::max<int64_t>(0, delay),
        [guard, generation]()
        {
            auto lease = guard->lifetime.lock();
            if (guard->managed && !lease)
            {
                return;
            }
            std::lock_guard<std::mutex> fence(guard->mutex);
            if (guard->owner)
            {
                guard->owner->on_fec_timer(generation);
            }
        });
    fec_timer_armed_ = true;
    timer_wake_pending_ = true;
}

void DatagramAir::on_fec_timer(uint64_t generation)
{
    queue_tx(
        [&]()
        {
            if (generation != tx_timer_generation_)
            {
                return;
            }
            fec_timer_armed_ = false;
            std::vector<std::vector<uint8_t>> shards;
            fec_tx_.on_tick(&shards);
            enqueue_fec(shards);
            sync_fec_timer_after_push();
        });
}

void DatagramAir::arm_rx_timer()
{
    if (!reactor_ || rx_timer_armed_ || !fec_rx_.has_pending_rx())
    {
        return;
    }
    auto guard = timer_guard_;
    rx_timer_id_ = reactor_->get_timer().wait_ms(
        k_rx_poll_ms,
        [guard]()
        {
            auto lease = guard->lifetime.lock();
            if (guard->managed && !lease)
            {
                return;
            }
            std::lock_guard<std::mutex> fence(guard->mutex);
            if (guard->owner)
            {
                guard->owner->on_rx_timer();
            }
        });
    rx_timer_armed_ = true;
    timer_wake_pending_ = true;
}

void DatagramAir::on_rx_timer()
{
    fec_vendor::fec_rx_payload_list payloads;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        if (!running_)
        {
            return;
        }
        rx_timer_armed_ = false;
        fec_rx_.poll_rx(&payloads);
        arm_rx_timer();
        // Already on the CV reactor; the next timer deadline is observed there.
        timer_wake_pending_ = false;
    }
    for (const auto& payload : payloads)
    {
        deliver(payload.u8(), payload.size());
    }
}

void DatagramAir::flush_fec_locked()
{
    cancel_fec_timer_locked();
    std::vector<std::vector<uint8_t>> shards;
    fec_tx_.flush(&shards);
    enqueue_fec(shards);
}

void DatagramAir::announce_down()
{
    queue_tx(
        [&]()
        {
            flush_fec_locked();
        });
}

bool DatagramAir::set_fec(FecType type, int k, int n, std::string* error)
{
    bool ok = true;
    queue_tx(
        [&]()
        {
            if ((type != FecType::none && type != FecType::RsBlockErasure) ||
                (type == FecType::RsBlockErasure &&
                 (k < 1 || n <= k || n > 31)))
            {
                if (error)
                {
                    *error = "invalid fec k/n (need 1 <= k < n <= 31)";
                }
                ok = false;
                return;
            }
            flush_fec_locked();
            if (type == FecType::none)
            {
                fec_tx_.disable();
            }
            else
            {
                ok = fec_tx_.init(k, n, fec_timeout_ms_);
            }
        });
    return ok;
}

bool DatagramAir::set_fec_timeout_ms(int timeout_ms, std::string* error)
{
    if (timeout_ms < 0)
    {
        if (error)
        {
            *error = "EINVAL";
        }
        return false;
    }
    queue_tx(
        [&]()
        {
            if (fec_tx_.enabled())
            {
                const int k = fec_tx_.k(), n = fec_tx_.n();
                flush_fec_locked();
                fec_tx_.init(k, n, timeout_ms);
            }
            fec_timeout_ms_ = timeout_ms;
        });
    return true;
}

int DatagramAir::fec_timeout_ms() const
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    return fec_timeout_ms_;
}

uint64_t DatagramAir::fec_recovered() const
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    return fec_rx_.recovered();
}

uint64_t DatagramAir::fec_decode_fail() const
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    // Preserve the upstream aggregate's inclusion of malformed/mismatched
    // shards.
    return fec_rx_.decode_fail() + fec_rx_.hdr_errors() + fec_rx_.kn_mismatch();
}

void DatagramAir::tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    uint64_t packets = 0, bytes = 0;
    fec_tx_.pending_tx_stats(&packets, &bytes);
    if (pkt)
    {
        *pkt = packets;
    }
    if (byt)
    {
        *byt = bytes;
    }
}

void DatagramAir::get_fec(FecType* type, int* k, int* n) const
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    if (type)
    {
        *type = fec_tx_.enabled() ? FecType::RsBlockErasure : FecType::none;
    }
    if (k)
    {
        *k = fec_tx_.enabled() ? fec_tx_.k() : 0;
    }
    if (n)
    {
        *n = fec_tx_.enabled() ? fec_tx_.n() : 0;
    }
}

}  // namespace winject
