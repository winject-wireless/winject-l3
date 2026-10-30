#include "endpoint/DatagramAir.h"

#include "utils/NetUtil.h"

#include <utility>

namespace winject
{

DatagramAir::~DatagramAir()
{
    stop();
}

bool DatagramAir::start(std::shared_ptr<BoundedLossyQueue> tx_queue,
                        DeliverApp deliver_app,
                        std::shared_ptr<Reactor> reactor,
                        const UpstreamConfig& cfg, uint8_t route_bus,
                        UpstreamStats* tx_stats, std::mutex* tx_stats_mu,
                        DeliverOwnedApp deliver_owned_app)
{
    std::lock_guard<std::mutex> lock(mu);
    stop_locked();
    if (!reactor || !tx_queue || tx_queue->capacity() == 0 || route_bus == 0 ||
        tx_stats == nullptr || tx_stats_mu == nullptr ||
        cfg.fec_timeout_ms < 0 ||
        (cfg.fec_type != FecType::none &&
         cfg.fec_type != FecType::RsBlockErasure))
    {
        return false;
    }
    auto emit = [this](const uint8_t* shard, size_t len)
    {
        push_tx(shard, len, true);
    };
    if (cfg.fec_type == FecType::RsBlockErasure &&
        !fec_tx.start(reactor, cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms, emit))
    {
        return false;
    }
    fec_rx.start(std::move(reactor),
                 [this](const uint8_t* data, size_t len)
                 {
                     deliver(data, len);
                 });
    this->tx_queue = std::move(tx_queue);
    this->route_bus = route_bus;
    this->tx_stats = tx_stats;
    this->tx_stats_mu = tx_stats_mu;
    this->deliver_app = std::move(deliver_app);
    this->deliver_owned_app = std::move(deliver_owned_app);
    return true;
}

void DatagramAir::stop()
{
    std::lock_guard<std::mutex> lock(mu);
    stop_locked();
}

void DatagramAir::stop_locked()
{
    fec_tx.stop();
    fec_rx.stop();
    tx_queue.reset();
    route_bus = 0;
    tx_stats = nullptr;
    tx_stats_mu = nullptr;
    deliver_app = nullptr;
    deliver_owned_app = nullptr;
}

void DatagramAir::push_tx(const uint8_t* data, size_t len, bool is_fec)
{
    if (len == 0 || !tx_queue)
    {
        return;
    }
    const auto capacity = tx_queue->capacity();
    bfc::sized_buffer framed(k_slot_prefix_len + len);
    size_t framed_len = 0;
    {
        std::lock_guard<std::mutex> lock(*tx_stats_mu);
        if (!stamp_slot_payload(tx_stats, route_bus, is_fec, data, len,
                                reinterpret_cast<uint8_t*>(framed.data()),
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
        fec_air_tx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
        fec_air_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
    }
}

void DatagramAir::deliver(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0 || !deliver_app)
    {
        return;
    }
    app_rx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
    app_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
    deliver_app(data, len);
}

void DatagramAir::accept_app_datagram(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lock(mu);
    if (data == nullptr || len == 0 || !tx_queue)
    {
        return;
    }
    app_tx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
    app_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
    const size_t limit =
        fec_tx.enabled() ? FecTx::max_original() : k_stream_payload_max;
    if (len > limit)
    {
        app_rx_oversize_pkt_acc.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (fec_tx.enabled())
    {
        fec_tx.push(data, len);
    }
    else
    {
        push_tx(data, len, false);
    }
}

void DatagramAir::on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec)
{
    std::lock_guard<std::mutex> lock(mu);
    if (pkt.empty() || pkt.size() > k_stream_payload_max || !tx_queue)
    {
        return;
    }
    if (!is_fec)
    {
        if (!deliver_owned_app)
        {
            deliver(pkt.u8(), pkt.size());
            return;
        }
        // Hand the radio buffer on without a copy.
        if (pkt.size() == 0)
        {
            return;
        }
        app_rx_bytes_acc.fetch_add(pkt.size(), std::memory_order_relaxed);
        app_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
        deliver_owned_app(std::move(pkt));
        return;
    }
    fec_air_rx_bytes_acc.fetch_add(pkt.size(), std::memory_order_relaxed);
    fec_air_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
    if (!fec_tx.enabled())
    {
        fec_air_rx_unexpected_acc.fetch_add(1, std::memory_order_relaxed);
    }
    fec_rx.push(std::move(pkt));
}

}  // namespace winject
