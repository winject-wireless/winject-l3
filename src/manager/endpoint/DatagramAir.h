#ifndef WINJECT_MANAGER_DATAGRAM_AIR_H_
#define WINJECT_MANAGER_DATAGRAM_AIR_H_

#include "Config.h"
#include "endpoint/UpstreamQueues.h"
#include "endpoint/UpstreamStats.h"
#include "fec/FecRx.h"
#include "fec/FecTx.h"

#include <atomic>
#include <bfc/cv_reactor.hpp>
#include <bfc/shared_sized_buffer.hpp>
#include <bfc/sized_buffer.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>

namespace winject
{

// Routes datagrams between an app and the air, shared by UDP and queue
// upstreams. TX: app datagram -> FecTx or raw -> slot stamp -> tx_queue.
// RX: air SDU -> FecRx when its LC FEC flag is set, else raw -> deliver.
// The data path and FEC timers run on the CV reactor thread. stop() may run
// on any other thread: it waits out an in-flight data-path call or FEC timer,
// and neither runs after it returns. Counters may be read from any thread.
class DatagramAir
{
public:
    using DeliverApp = std::function<void(const uint8_t* data, size_t len)>;
    using Reactor = bfc::cv_reactor<>;
    using FecTimer = Reactor::timer_t;

    DatagramAir() = default;
    ~DatagramAir();

    DatagramAir(const DatagramAir&) = delete;
    DatagramAir& operator=(const DatagramAir&) = delete;

    bool start(std::shared_ptr<BoundedLossyQueue> tx_queue,
               DeliverApp deliver_app, std::shared_ptr<Reactor> reactor,
               const UpstreamConfig& cfg, uint8_t route_bus,
               UpstreamStats* tx_stats, std::mutex* tx_stats_mu);
    void stop();
    // Next FEC SDU id. Read or set it only while no data path runs (before
    // the reactor sees this instance, or after stop()).
    uint16_t fec_tx_sdu_seq() const
    {
        return fec_tx.sdu_seq();
    }
    void set_fec_tx_sdu_seq(uint16_t seq)
    {
        fec_tx.set_sdu_seq(seq);
    }
    void set_lifetime(std::weak_ptr<void> owner)
    {
        fec_tx.set_lifetime(owner);
        fec_rx.set_lifetime(std::move(owner));
    }

    // UDP truncation is known before enqueue; account it without touching FEC.
    // Oversize datagrams come from the app (the app_rx_* name follows the
    // console and metric keys: received from the app socket).
    void reject_oversize_datagram(size_t len)
    {
        app_tx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
        app_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
        app_rx_oversize_pkt_acc.fetch_add(1, std::memory_order_relaxed);
    }
    void accept_app_datagram(const uint8_t* data, size_t len);
    void on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec);

    uint64_t tx_queue_drops() const
    {
        return tx_queue_drops_acc.load();
    }
    uint64_t app_rx_bytes() const
    {
        return app_rx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_rx_packets() const
    {
        return app_rx_packets_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_rx_oversize_pkt() const
    {
        return app_rx_oversize_pkt_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_tx_bytes() const
    {
        return app_tx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_tx_packets() const
    {
        return app_tx_packets_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_bytes() const
    {
        return fec_air_rx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_packets() const
    {
        return fec_air_rx_packets_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_unexpected() const
    {
        return fec_air_rx_unexpected_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_tx_bytes() const
    {
        return fec_air_tx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_tx_packets() const
    {
        return fec_air_tx_packets_acc.load(std::memory_order_relaxed);
    }
    void tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
    {
        fec_tx.pending_stats(pkt, byt);
    }
    uint64_t fec_recovered() const
    {
        return fec_rx.recovered();
    }
    uint64_t fec_decode_fail() const
    {
        return fec_rx.decode_fail();
    }

private:
    void stop_locked();
    void push_tx(const uint8_t* data, size_t len, bool is_fec);
    void deliver(const uint8_t* data, size_t len);

    // Held by every data-path entry and by start/stop. FEC timers do not take
    // it; stop() fences them through DeadlineTimer::unbind.
    std::mutex mu;

    std::shared_ptr<BoundedLossyQueue> tx_queue;
    uint8_t route_bus = 0;
    UpstreamStats* tx_stats = nullptr;
    std::mutex* tx_stats_mu = nullptr;
    DeliverApp deliver_app;
    FecTx fec_tx;
    FecRx fec_rx;
    std::atomic<uint64_t> tx_queue_drops_acc{0};
    std::atomic<uint64_t> app_rx_bytes_acc{0};
    std::atomic<uint64_t> app_rx_packets_acc{0};
    std::atomic<uint64_t> app_rx_oversize_pkt_acc{0};
    std::atomic<uint64_t> app_tx_bytes_acc{0};
    std::atomic<uint64_t> app_tx_packets_acc{0};
    std::atomic<uint64_t> fec_air_rx_bytes_acc{0};
    std::atomic<uint64_t> fec_air_rx_packets_acc{0};
    std::atomic<uint64_t> fec_air_rx_unexpected_acc{0};
    std::atomic<uint64_t> fec_air_tx_bytes_acc{0};
    std::atomic<uint64_t> fec_air_tx_packets_acc{0};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_DATAGRAM_AIR_H_
