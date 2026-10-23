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
constexpr size_t k_max_tx_queue = 1024;
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
    Wake tx_wake, timer_wake;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        if (!running_)
        {
            return;
        }
        const bool had_tx = has_tx_locked();
        queue();
        if (!had_tx && has_tx_locked())
        {
            tx_wake = tx_wake_;
        }
        if (timer_wake_pending_)
        {
            timer_wake_pending_ = false;
            timer_wake = timer_wake_;
        }
    }
    if (timer_wake)
    {
        timer_wake();
    }
    if (tx_wake)
    {
        tx_wake();
    }
}

DatagramAir::~DatagramAir()
{
    stop();
}

bool DatagramAir::start(Wake tx_wake, DeliverApp deliver_app,
                        FecTimer* fec_timer, Wake timer_wake,
                        const UpstreamConfig& cfg, const char* log_name)
{
    stop();
    std::lock_guard<std::mutex> lock(tx_mu_);
    fec_tx_.disable();
    fec_rx_ = RsBlockErasure{};
    txq_.clear();
    if (cfg.fec_timeout_ms < 0 ||
        (cfg.fec_type != FecType::none &&
         cfg.fec_type != FecType::RsBlockErasure) ||
        (cfg.fec_type == FecType::RsBlockErasure &&
         !fec_tx_.init(cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms)))
    {
        return false;
    }
    tx_wake_ = std::move(tx_wake);
    deliver_app_ = std::move(deliver_app);
    timer_wake_ = std::move(timer_wake);
    fec_timer_ = fec_timer;
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
    if (rx_timer_armed_ && fec_timer_)
    {
        fec_timer_->cancel(rx_timer_id_);
    }
    rx_timer_armed_ = false;
    fec_timer_ = nullptr;
    tx_wake_ = nullptr;
    deliver_app_ = nullptr;
    timer_wake_ = nullptr;
    txq_.clear();
    fec_tx_.disable();
}

void DatagramAir::push_tx(bfc::sized_buffer pkt, bool is_fec)
{
    if (pkt.empty())
    {
        return;
    }
    if (txq_.size() >= k_max_tx_queue)
    {
        txq_.pop_front();
        tx_queue_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    txq_.push_back(TxItem{std::move(pkt), is_fec});
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
    Wake timer_wake;
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
            timer_wake = timer_wake_;
        }
    }
    if (timer_wake)
    {
        timer_wake();
    }
    for (const auto& payload : payloads)
    {
        deliver(payload.u8(), payload.size());
    }
}

void DatagramAir::cancel_fec_timer_locked()
{
    ++tx_timer_generation_;
    if (fec_timer_armed_ && fec_timer_)
    {
        fec_timer_->cancel(fec_timer_id_);
    }
    fec_timer_armed_ = false;
}

void DatagramAir::sync_fec_timer_after_push()
{
    cancel_fec_timer_locked();
    std::chrono::steady_clock::time_point deadline;
    if (!fec_timer_ || !fec_tx_.next_deadline(&deadline))
    {
        return;
    }
    const auto delay = std::chrono::duration_cast<std::chrono::microseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count();
    const auto generation = tx_timer_generation_;
    auto guard = timer_guard_;
    fec_timer_id_ =
        fec_timer_->wait_us(std::max<int64_t>(0, delay),
                            [guard, generation]()
                            {
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
    if (!fec_timer_ || rx_timer_armed_ || !fec_rx_.has_pending_rx())
    {
        return;
    }
    auto guard = timer_guard_;
    rx_timer_id_ =
        fec_timer_->wait_ms(k_rx_poll_ms,
                            [guard]()
                            {
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
        // Already on the IO reactor; the next timer deadline is observed there.
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
    for (const auto& item : txq_)
    {
        ++packets;
        bytes += item.pkt.size();
    }
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

bool DatagramAir::has_tx()
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    return has_tx_locked();
}

bool DatagramAir::has_tx_locked() const
{
    return !txq_.empty();
}

size_t DatagramAir::get_tx_size()
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    return txq_.empty() ? 0 : txq_.front().pkt.size();
}

bfc::sized_buffer DatagramAir::pull_tx(size_t max, bool* is_fec)
{
    if (is_fec)
    {
        *is_fec = false;
    }
    std::lock_guard<std::mutex> lock(tx_mu_);
    if (txq_.empty() || txq_.front().pkt.size() > max)
    {
        return {};
    }
    TxItem item = std::move(txq_.front());
    txq_.pop_front();
    if (item.is_fec)
    {
        fec_air_tx_bytes_.fetch_add(item.pkt.size(), std::memory_order_relaxed);
        fec_air_tx_packets_.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_fec)
    {
        *is_fec = item.is_fec;
    }
    return std::move(item.pkt);
}

}  // namespace winject
