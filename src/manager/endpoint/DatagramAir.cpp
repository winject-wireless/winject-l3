#include "endpoint/DatagramAir.h"

#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <string.h>
#include <utility>
#include <vector>

namespace
{

constexpr size_t k_max_tx_queue = 1024;

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

bfc::sized_buffer make_pkt(std::vector<uint8_t>&& payload)
{
    if (payload.empty())
    {
        return {};
    }
    return make_pkt(payload.data(), payload.size());
}

}  // namespace

namespace winject
{

template <typename F>
void DatagramAir::queue_tx(F&& queue)
{
    Wake tx_wake;
    Wake timer_wake;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
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
    tx_wake_ = std::move(tx_wake);
    deliver_app_ = std::move(deliver_app);
    timer_wake_ = std::move(timer_wake);
    fec_timer_ = fec_timer;
    log_name_ =
        (log_name != nullptr && log_name[0] != '\0') ? log_name : "upstream";
    fec_timeout_ms_ = cfg.fec_timeout_ms;
    if (cfg.fec_type == FecType::RsBlockErasure)
    {
        if (!fec_.init(cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms))
        {
            LOG_ERR("%s fec init k=%d n=%d failed", log_name_, cfg.fec_k,
                    cfg.fec_n);
            return false;
        }
        LOG_INF("%s fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)",
                log_name_, cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms,
                fec_.impl_name());
    }
    return true;
}

void DatagramAir::stop()
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    cancel_fec_timer_locked();
    fec_timer_ = nullptr;
    tx_wake_ = nullptr;
    deliver_app_ = nullptr;
    timer_wake_ = nullptr;
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
    }
    TxItem item;
    item.pkt = std::move(pkt);
    item.is_fec = is_fec;
    txq_.push_back(std::move(item));
}

void DatagramAir::enqueue_air(bfc::sized_buffer pkt)
{
    queue_tx(
        [&]()
        {
            push_tx(std::move(pkt), false);
        });
}

void DatagramAir::deliver(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    DeliverApp deliver;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        deliver = deliver_app_;
    }
    if (!deliver)
    {
        return;
    }
    app_rx_bytes_.fetch_add(static_cast<uint64_t>(len),
                            std::memory_order_relaxed);
    app_rx_packets_.fetch_add(1, std::memory_order_relaxed);
    deliver(data, len);
}

void DatagramAir::accept_app_datagram(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    app_tx_bytes_.fetch_add(static_cast<uint64_t>(len),
                            std::memory_order_relaxed);
    app_tx_packets_.fetch_add(1, std::memory_order_relaxed);
    if (fec_.enabled())
    {
        queue_tx(
            [&]()
            {
                fec_.push_app(data, len, nullptr);
                sync_fec_timer_after_push();
            });
        return;
    }
    if (len > k_stream_payload_max)
    {
        const uint64_t drops =
            app_rx_oversize_pkt_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (drops == 1 || (drops & 1023u) == 0)
        {
            LOG_WRN("drop oversized %s %zu (drops=%llu)", log_name_, len,
                    static_cast<unsigned long long>(drops));
        }
        return;
    }
    enqueue_air(make_pkt(data, len));
}

void DatagramAir::on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec)
{
    if (pkt.empty())
    {
        return;
    }
    const uint8_t* air = reinterpret_cast<const uint8_t*>(pkt.data());
    const size_t air_len = pkt.size();
    if (!is_fec)
    {
        deliver(air, air_len);
        return;
    }
    fec_air_rx_bytes_.fetch_add(static_cast<uint64_t>(air_len),
                                std::memory_order_relaxed);
    fec_air_rx_packets_.fetch_add(1, std::memory_order_relaxed);
    if (!fec_.enabled())
    {
        fec_air_rx_unexpected_.fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<std::vector<uint8_t>> payloads;
    fec_.push_air(air, air_len, &payloads);
    for (const auto& payload : payloads)
    {
        deliver(payload.data(), payload.size());
    }
}

void DatagramAir::arm_fec_timer()
{
    if (fec_timer_ == nullptr || !fec_.enabled() || fec_timer_armed_ ||
        fec_timeout_ms_ <= 0 || !fec_.has_pending())
    {
        return;
    }
    fec_timer_id_ = fec_timer_->wait_ms(fec_timeout_ms_,
                                        [this]()
                                        {
                                            on_fec_timer();
                                        });
    fec_timer_armed_ = true;
    timer_wake_pending_ = static_cast<bool>(timer_wake_);
}

void DatagramAir::cancel_fec_timer_locked()
{
    timer_wake_pending_ = false;
    if (!fec_timer_armed_ || fec_timer_ == nullptr)
    {
        fec_timer_armed_ = false;
        return;
    }
    fec_timer_->cancel(fec_timer_id_);
    fec_timer_armed_ = false;
}

void DatagramAir::on_fec_timer()
{
    queue_tx(
        [&]()
        {
            fec_timer_armed_ = false;
            fec_.flush_if_deadline();
            sync_fec_timer_after_push();
        });
}

void DatagramAir::sync_fec_timer_after_push()
{
    if (!fec_.has_pending())
    {
        cancel_fec_timer_locked();
        return;
    }
    arm_fec_timer();
}

void DatagramAir::stage_fec_shard_for_tx(size_t max)
{
    if (!fec_.enabled() || !txq_.empty())
    {
        return;
    }
    std::vector<uint8_t> shard;
    if (!fec_.pop_tx_shard(max, &shard))
    {
        return;
    }
    push_tx(make_pkt(std::move(shard)), true);
}

void DatagramAir::announce_down()
{
    if (!fec_.enabled())
    {
        return;
    }
    queue_tx(
        [&]()
        {
            std::vector<std::vector<uint8_t>> encoded;
            cancel_fec_timer_locked();
            fec_.flush(&encoded);
            fec_.drain_tx_shards(&encoded);
            for (auto& pkt : encoded)
            {
                push_tx(make_pkt(std::move(pkt)), true);
            }
        });
}

bool DatagramAir::set_fec(FecType type, int k, int n, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };

    announce_down();
    std::lock_guard<std::mutex> lock(tx_mu_);
    if (type == FecType::none)
    {
        fec_.disable();
        LOG_INF("%s fec disabled", log_name_);
        return true;
    }
    if (type != FecType::RsBlockErasure)
    {
        return fail("unsupported fec type");
    }
    const int timeout_ms = fec_timeout_ms_ > 0
                               ? fec_timeout_ms_
                               : RsBlockErasure::k_default_timeout_ms;
    if (!fec_.init(k, n, timeout_ms))
    {
        return fail("invalid fec k/n (need 1 <= k < n <= 31)");
    }
    LOG_INF("%s fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)", log_name_,
            k, n, timeout_ms, fec_.impl_name());
    return true;
}

bool DatagramAir::set_fec_timeout_ms(int timeout_ms, std::string* error)
{
    if (timeout_ms < 0)
    {
        if (error != nullptr)
        {
            *error = "EINVAL";
        }
        return false;
    }
    fec_timeout_ms_ = timeout_ms;
    return true;
}

uint64_t DatagramAir::fec_recovered() const
{
    return fec_.recovered();
}

uint64_t DatagramAir::fec_decode_fail() const
{
    return fec_.decode_fail();
}

void DatagramAir::tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
{
    uint64_t packets = 0;
    uint64_t bytes = 0;
    std::lock_guard<std::mutex> lock(tx_mu_);
    for (const auto& item : txq_)
    {
        packets++;
        bytes += static_cast<uint64_t>(item.pkt.size());
    }
    if (fec_.enabled())
    {
        uint64_t fec_packets = 0;
        uint64_t fec_bytes = 0;
        fec_.tx_pending_stats(&fec_packets, &fec_bytes);
        packets += fec_packets;
        bytes += fec_bytes;
    }
    if (pkt != nullptr)
    {
        *pkt = packets;
    }
    if (byt != nullptr)
    {
        *byt = bytes;
    }
}

void DatagramAir::get_fec(FecType* type, int* k, int* n) const
{
    if (type != nullptr)
    {
        *type = fec_.enabled() ? FecType::RsBlockErasure : FecType::none;
    }
    if (k != nullptr)
    {
        *k = fec_.enabled() ? fec_.k() : 0;
    }
    if (n != nullptr)
    {
        *n = fec_.enabled() ? fec_.n() : 0;
    }
}

bool DatagramAir::has_tx()
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    return has_tx_locked();
}

bool DatagramAir::has_tx_locked() const
{
    if (!txq_.empty())
    {
        return true;
    }
    return fec_.enabled() && fec_.has_tx_shards();
}

size_t DatagramAir::get_tx_size()
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    if (!txq_.empty())
    {
        return txq_.front().pkt.size();
    }
    if (fec_.enabled())
    {
        return fec_.first_tx_shard_size();
    }
    return 0;
}

bfc::sized_buffer DatagramAir::pull_tx(size_t max, bool* is_fec)
{
    if (is_fec != nullptr)
    {
        *is_fec = false;
    }
    std::lock_guard<std::mutex> lock(tx_mu_);
    stage_fec_shard_for_tx(max);
    if (txq_.empty() || max == 0)
    {
        return {};
    }
    TxItem& item = txq_.front();
    if (item.pkt.size() > max)
    {
        return {};
    }
    bfc::sized_buffer out = std::move(item.pkt);
    const bool shard = item.is_fec;
    txq_.pop_front();
    if (shard)
    {
        fec_air_tx_bytes_.fetch_add(static_cast<uint64_t>(out.size()),
                                    std::memory_order_relaxed);
        fec_air_tx_packets_.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_fec != nullptr)
    {
        *is_fec = shard;
    }
    return out;
}

}  // namespace winject
