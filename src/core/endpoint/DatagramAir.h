#ifndef WINJECT_MANAGER_DATAGRAM_AIR_H_
#define WINJECT_MANAGER_DATAGRAM_AIR_H_

#include "endpoint/UpstreamConfig.h"
#include "endpoint/UpstreamQueues.h"
#include "endpoint/UpstreamStats.h"
#include "frames/LCHeader.h"

#include <atomic>
#include <bfc/cv_reactor.hpp>
#include <bfc/shared_sized_buffer.hpp>
#include <bfc/sized_buffer.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <winject_fec/read_stream.hpp>
#include <winject_fec/write_stream.hpp>

namespace winject::core
{

// Routes datagrams between an app and the air, shared by UDP and queue
// upstreams, through a winject-fec write and read stream on the upstream's CV
// reactor.
//
// TX: app datagram -> copied once into [LC room][fec slot][data] -> write
// stream (plain, or a data shard now and parity at its place in the spread)
// -> LC prefix stamped into the room -> radio tx_queue.
// RX: LC body -> read stream (plain at once, FEC data in sequence, holes
// rebuilt from parity or given up) -> app, as a view of the radio buffer.
//
// The data path runs on the CV reactor thread; the streams' timers release
// parity, flush short blocks and give up holes there. stop() may run on any
// other thread: it waits out an in-flight data-path call and none runs after
// it returns. Counters may be read from any thread.
class DatagramAir
{
public:
    using DeliverApp = std::function<void(const uint8_t* data, size_t len)>;
    using DeliverOwnedApp = std::function<void(bfc::shared_sized_buffer)>;
    using Reactor = bfc::cv_reactor<>;
    using FecTimer = Reactor::timer_t;

    DatagramAir() = default;
    ~DatagramAir();

    DatagramAir(const DatagramAir&) = delete;
    DatagramAir& operator=(const DatagramAir&) = delete;

    // The write stream config for an upstream config; false when its FEC
    // parameters are invalid.
    static bool fec_write_config(const UpstreamConfig& cfg,
                                 winject::fec::fec_write_stream::config* out);

    bool start(std::shared_ptr<BoundedLossyQueue> tx_queue,
               DeliverApp deliver_app, std::shared_ptr<Reactor> reactor,
               const UpstreamConfig& cfg, uint8_t route_bus,
               UpstreamStats* tx_stats, std::mutex* tx_stats_mu,
               DeliverOwnedApp deliver_owned_app = {});
    void stop();
    // Next FEC data shard id. Read or set it only while no data path runs
    // (before the reactor sees this instance, or after stop()).
    uint16_t fec_tx_sdu_seq() const;
    void set_fec_tx_sdu_seq(uint16_t seq);
    // Binds the streams' outputs to the reactor; their callbacks hold owner
    // while they run.
    void set_lifetime(std::weak_ptr<void> owner);

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
    // body: the slot past its LC prefix.
    void on_radio_rx(bfc::shared_sized_buffer body, LcType type);

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
    // FEC holds its pending parity inside the write stream; only the radio
    // queue's backlog is reported (UpstreamLink adds it).
    void tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
    {
        if (pkt)
        {
            *pkt = 0;
        }
        if (byt)
        {
            *byt = 0;
        }
    }
    // Data shards rebuilt from parity.
    uint64_t fec_recovered() const
    {
        return fec_recovered_acc.load(std::memory_order_relaxed);
    }
    // Blocks that lost data (given up, failed to decode or evicted), plus
    // malformed and k/n-mismatched shards.
    uint64_t fec_decode_fail() const
    {
        return fec_decode_fail_acc.load(std::memory_order_relaxed);
    }

private:
    void stop_locked();
    void drain_tx();
    void drain_tx_locked();
    void drain_rx();
    void drain_rx_locked();
    void deliver(bfc::shared_sized_buffer datagram);
    void sync_fec_counters();

    // Held by every data-path entry and by start/stop. The streams' timers do
    // not take it: they only fill the stream outputs, which drain_tx/drain_rx
    // empty under it.
    std::mutex mu;

    std::shared_ptr<Reactor> reactor;
    std::shared_ptr<BoundedLossyQueue> tx_queue;
    uint8_t route_bus = 0;
    UpstreamStats* tx_stats = nullptr;
    std::mutex* tx_stats_mu = nullptr;
    DeliverApp deliver_app;
    DeliverOwnedApp deliver_owned_app;
    bool fec_tx_on = false;
    size_t max_app_datagram = 0;
    std::unique_ptr<winject::fec::fec_write_stream> tx_stream;
    std::unique_ptr<winject::fec::fec_read_stream> rx_stream;
    bool outputs_bound = false;
    // The FEC sequence across stop() and a replacement's start().
    uint16_t fec_seq = 0;
    bool fec_seq_set = false;

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
    std::atomic<uint64_t> fec_recovered_acc{0};
    std::atomic<uint64_t> fec_decode_fail_acc{0};
};

}  // namespace winject::core

#endif  // WINJECT_MANAGER_DATAGRAM_AIR_H_
