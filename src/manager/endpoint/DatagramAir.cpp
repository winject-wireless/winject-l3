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
    std::shared_ptr<Reactor> wake_reactor;
    {
        std::lock_guard<std::mutex> lock(tx_mu);
        if (!running)
        {
            return;
        }
        queue();
        if (timer_wake_pending)
        {
            timer_wake_pending = false;
            wake_reactor = this->reactor;
        }
    }
    if (wake_reactor)
    {
        wake_reactor->wake_up();
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
    std::lock_guard<std::mutex> lock(tx_mu);
    fec_tx.disable();
    fec_rx = RsBlockErasure{};
    if (!reactor || !tx_queue || tx_queue->capacity() == 0 || route_bus == 0 ||
        tx_stats == nullptr || tx_stats_mu == nullptr ||
        cfg.fec_timeout_ms < 0 ||
        (cfg.fec_type != FecType::none &&
         cfg.fec_type != FecType::RsBlockErasure) ||
        (cfg.fec_type == FecType::RsBlockErasure &&
         !fec_tx.init(cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms)))
    {
        return false;
    }
    this->tx_queue = std::move(tx_queue);
    this->route_bus = route_bus;
    this->tx_stats = tx_stats;
    this->tx_stats_mu = tx_stats_mu;
    this->deliver_app = std::move(deliver_app);
    this->reactor = std::move(reactor);
    this->log_name = log_name && log_name[0] ? log_name : "upstream";
    fec_timeout_setting = cfg.fec_timeout_ms;
    timer_guard = std::make_shared<TimerGuard>();
    timer_guard->owner = this;
    running = true;
    return true;
}

void DatagramAir::stop()
{
    // start/stop themselves are serialized by the endpoint lifecycle owner.
    auto guard = timer_guard;
    std::unique_lock<std::mutex> fence;
    if (guard)
    {
        fence = std::unique_lock<std::mutex>(guard->mutex);
        guard->owner = nullptr;
    }
    std::lock_guard<std::mutex> lock(tx_mu);
    running = false;
    cancel_fec_timer_locked();
    if (rx_timer_armed && reactor)
    {
        reactor->get_timer().cancel(rx_timer_id);
    }
    rx_timer_armed = false;
    reactor.reset();
    tx_queue.reset();
    route_bus = 0;
    tx_stats = nullptr;
    tx_stats_mu = nullptr;
    deliver_app = nullptr;
    fec_tx.disable();
}

void DatagramAir::push_tx(bfc::sized_buffer pkt, bool is_fec)
{
    if (pkt.empty() || route_bus == 0 || tx_stats == nullptr ||
        tx_stats_mu == nullptr)
    {
        return;
    }
    const auto capacity = tx_queue->capacity();
    const size_t size = pkt.size();
    bfc::sized_buffer framed(k_slot_prefix_len + size);
    size_t framed_len = 0;
    {
        std::lock_guard<std::mutex> lock(*tx_stats_mu);
        if (!stamp_slot_payload(tx_stats, route_bus, is_fec,
                                reinterpret_cast<const uint8_t*>(pkt.data()),
                                size, reinterpret_cast<uint8_t*>(framed.data()),
                                framed.size(), &framed_len))
        {
            return;
        }
    }
    auto shared =
        bfc::shared_sized_buffer::copy_from(framed.data(), framed_len);
    const auto before = tx_queue->push(std::move(shared));
    if (capacity == 0 || before >= capacity)
    {
        tx_queue_drops_acc.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_fec)
    {
        fec_air_tx_bytes_acc.fetch_add(size, std::memory_order_relaxed);
        fec_air_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
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
        std::lock_guard<std::mutex> lock(tx_mu);
        if (!running)
        {
            return;
        }
        callback = deliver_app;
    }
    if (!callback)
    {
        return;
    }
    app_rx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
    app_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
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
            app_tx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
            app_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
            const size_t limit = fec_tx.enabled()
                                     ? RsBlockErasure::max_original()
                                     : k_stream_payload_max;
            if (len > limit)
            {
                app_rx_oversize_pkt_acc.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (fec_tx.enabled())
            {
                std::vector<std::vector<uint8_t>> shards;
                fec_tx.push_app(data, len, &shards);
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
    std::shared_ptr<Reactor> wake_reactor;
    {
        std::lock_guard<std::mutex> lock(tx_mu);
        if (!running)
        {
            return;
        }
        fec_air_rx_bytes_acc.fetch_add(pkt.size(), std::memory_order_relaxed);
        fec_air_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
        if (!fec_tx.enabled())
        {
            fec_air_rx_unexpected_acc.fetch_add(1, std::memory_order_relaxed);
        }
        fec_rx.push_air(std::move(pkt), &payloads);
        arm_rx_timer();
        if (timer_wake_pending)
        {
            timer_wake_pending = false;
            wake_reactor = this->reactor;
        }
    }
    if (wake_reactor)
    {
        wake_reactor->wake_up();
    }
    for (const auto& payload : payloads)
    {
        deliver(payload.u8(), payload.size());
    }
}

void DatagramAir::cancel_fec_timer_locked()
{
    ++tx_timer_generation;
    if (fec_timer_armed && reactor)
    {
        reactor->get_timer().cancel(fec_timer_id);
    }
    fec_timer_armed = false;
}

void DatagramAir::sync_fec_timer_after_push()
{
    cancel_fec_timer_locked();
    std::chrono::steady_clock::time_point deadline;
    if (!reactor || !fec_tx.next_deadline(&deadline))
    {
        return;
    }
    const auto delay = std::chrono::duration_cast<std::chrono::microseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count();
    const auto generation = tx_timer_generation;
    auto guard = timer_guard;
    fec_timer_id = reactor->get_timer().wait_us(
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
    fec_timer_armed = true;
    timer_wake_pending = true;
}

void DatagramAir::on_fec_timer(uint64_t generation)
{
    queue_tx(
        [&]()
        {
            if (generation != tx_timer_generation)
            {
                return;
            }
            fec_timer_armed = false;
            std::vector<std::vector<uint8_t>> shards;
            fec_tx.on_tick(&shards);
            enqueue_fec(shards);
            sync_fec_timer_after_push();
        });
}

void DatagramAir::arm_rx_timer()
{
    if (!reactor || rx_timer_armed || !fec_rx.has_pending_rx())
    {
        return;
    }
    auto guard = timer_guard;
    rx_timer_id = reactor->get_timer().wait_ms(
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
    rx_timer_armed = true;
    timer_wake_pending = true;
}

void DatagramAir::on_rx_timer()
{
    fec_vendor::fec_rx_payload_list payloads;
    {
        std::lock_guard<std::mutex> lock(tx_mu);
        if (!running)
        {
            return;
        }
        rx_timer_armed = false;
        fec_rx.poll_rx(&payloads);
        arm_rx_timer();
        // Already on the CV reactor; the next timer deadline is observed there.
        timer_wake_pending = false;
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
    fec_tx.flush(&shards);
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
                fec_tx.disable();
            }
            else
            {
                ok = fec_tx.init(k, n, fec_timeout_setting);
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
            if (fec_tx.enabled())
            {
                const int k = fec_tx.k(), n = fec_tx.n();
                flush_fec_locked();
                fec_tx.init(k, n, timeout_ms);
            }
            fec_timeout_setting = timeout_ms;
        });
    return true;
}

int DatagramAir::fec_timeout_ms() const
{
    std::lock_guard<std::mutex> lock(tx_mu);
    return fec_timeout_setting;
}

uint64_t DatagramAir::fec_recovered() const
{
    std::lock_guard<std::mutex> lock(tx_mu);
    return fec_rx.recovered();
}

uint64_t DatagramAir::fec_decode_fail() const
{
    std::lock_guard<std::mutex> lock(tx_mu);
    // Preserve the upstream aggregate's inclusion of malformed/mismatched
    // shards.
    return fec_rx.decode_fail() + fec_rx.hdr_errors() + fec_rx.kn_mismatch();
}

void DatagramAir::tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
{
    std::lock_guard<std::mutex> lock(tx_mu);
    uint64_t packets = 0, bytes = 0;
    fec_tx.pending_tx_stats(&packets, &bytes);
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
    std::lock_guard<std::mutex> lock(tx_mu);
    if (type)
    {
        *type = fec_tx.enabled() ? FecType::RsBlockErasure : FecType::none;
    }
    if (k)
    {
        *k = fec_tx.enabled() ? fec_tx.k() : 0;
    }
    if (n)
    {
        *n = fec_tx.enabled() ? fec_tx.n() : 0;
    }
}

}  // namespace winject
