#ifndef WINJECT_MANAGER_UPSTREAM_H_
#define WINJECT_MANAGER_UPSTREAM_H_

#include "Config.h"
#include "endpoint/DatagramAir.h"
#include "endpoint/UpstreamQueues.h"
#include "endpoint/UpstreamStats.h"

#include <atomic>
#include <bfc/cv_reactor.hpp>
#include <memory>
#include <mutex>
#include <utility>

namespace winject
{
// After make_shared, call bind_callbacks before publishing the instance.
// Hot-swap calls teardown() on the IO thread before dropping the last ref;
// once it returns no handler or FEC timer of this instance runs.
class Upstream : public std::enable_shared_from_this<Upstream>
{
public:
    using Reactor = bfc::cv_reactor<>;
    struct RxCounters
    {
        uint64_t delivery_attempts = 0;
        uint64_t accepted_packets = 0;
        uint64_t accepted_bytes = 0;
        uint64_t no_peer_drops = 0;
        uint64_t closed_drops = 0;
        uint64_t send_errors = 0;
        uint64_t queue_evictions = 0;
    };

    struct TxCounters
    {
        // Datagrams taken from the app, including oversize drops.
        uint64_t packets = 0;
        uint64_t bytes = 0;
        uint64_t oversize_drops = 0;
        // Oldest shard dropped from the radio TX queue.
        uint64_t queue_evictions = 0;
        // Oldest datagram dropped from the app TX queue by a UDP receive. A
        // queue upstream's app pushes that queue itself and counts its own.
        uint64_t app_queue_evictions = 0;
    };

    // Next air and FEC SDU sequence numbers, carried into a replacement.
    struct TxSequences
    {
        uint16_t air = 0;
        uint16_t fec_sdu = 0;
    };

    virtual void bind_callbacks();
    // Any thread but the CV reactor's; returns without waiting for the reactor.
    virtual void teardown();
    virtual ~Upstream();
    // Only while unbound: after teardown(), and before bind_callbacks().
    TxSequences tx_sequences() const;
    void resume_tx_sequences(const TxSequences& sequences);
    std::shared_ptr<ReactiveLossyQueue> rx_queue() const
    {
        return host_rx_queue;
    }
    // Shared with the radio demux, which counts oldest packets it evicts from
    // rx_queue().
    std::shared_ptr<std::atomic<uint64_t>> rx_queue_eviction_counter() const
    {
        return rx_queue_evictions_acc;
    }
    uint64_t rx_queue_evictions() const
    {
        return rx_queue_evictions_acc->load(std::memory_order_relaxed);
    }
    const UpstreamConfig& config() const
    {
        return upstream_config;
    }
    const char* type_name() const
    {
        return config().mode == UpstreamMode::queue ? "QUEUE" : "UDP";
    }
    std::string endpoint_rx() const
    {
        return config().endpoint.rx;
    }
    std::string endpoint_tx() const
    {
        return config().endpoint.tx;
    }
    RxCounters rx_counters() const;
    TxCounters tx_counters() const;
    UpstreamStats stats() const;
    uint64_t collect_air_seq_loss_delta();
    void get_fec(FecType* type, int* k, int* n) const;
    int fec_timeout_ms() const
    {
        return config().fec_timeout_ms;
    }
    void tx_pending_stats(uint64_t* packets, uint64_t* bytes) const;

    uint64_t app_rx_bytes() const
    {
        return air.app_rx_bytes();
    }
    uint64_t app_rx_packets() const
    {
        return air.app_rx_packets();
    }
    uint64_t app_rx_oversize_pkt() const
    {
        return air.app_rx_oversize_pkt();
    }
    uint64_t app_tx_bytes() const
    {
        return air.app_tx_bytes();
    }
    uint64_t app_tx_packets() const
    {
        return air.app_tx_packets();
    }
    uint64_t fec_air_rx_bytes() const
    {
        return air.fec_air_rx_bytes();
    }
    uint64_t fec_air_rx_packets() const
    {
        return air.fec_air_rx_packets();
    }
    uint64_t fec_air_rx_unexpected() const
    {
        return air.fec_air_rx_unexpected();
    }
    uint64_t fec_air_tx_bytes() const
    {
        return air.fec_air_tx_bytes();
    }
    uint64_t fec_air_tx_packets() const
    {
        return air.fec_air_tx_packets();
    }
    uint64_t fec_recovered() const
    {
        return air.fec_recovered();
    }
    uint64_t fec_decode_fail() const
    {
        return air.fec_decode_fail();
    }
    uint64_t tx_queue_drops() const
    {
        return air.tx_queue_drops();
    }

protected:
    Upstream(std::shared_ptr<Reactor> reactor, const UpstreamConfig& config,
             std::shared_ptr<BoundedLossyQueue> radio_tx_queue,
             std::shared_ptr<ReactiveLossyQueue> app_tx_queue = {});
    // Executed only by the supplied CV reactor, including FEC timer delivery.
    virtual void deliver_datagram(const uint8_t* data, size_t len) = 0;
    void unbind_callbacks();
    // False once teardown starts: a handler already dispatched stops early.
    bool active() const
    {
        return is_active.load(std::memory_order_acquire);
    }
    void count_app_tx_queue_push(size_t before, size_t capacity)
    {
        if (capacity != 0 && before >= capacity)
        {
            app_tx_queue_evictions_acc.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void submit_datagram(const uint8_t* data, size_t len);
    void reject_oversize_datagram(size_t len)
    {
        air.reject_oversize_datagram(len);
    }
    std::shared_ptr<ReactiveLossyQueue> app_tx_queue;
    std::shared_ptr<Reactor> reactor;
    UpstreamConfig upstream_config;
    mutable std::mutex rx_counters_mu;
    RxCounters stored_rx_counters;

private:
    void drain_radio_rx();
    void drain_app_tx_queue();
    std::shared_ptr<ReactiveLossyQueue> host_rx_queue;
    std::shared_ptr<BoundedLossyQueue> radio_tx_queue;
    std::shared_ptr<std::atomic<uint64_t>> rx_queue_evictions_acc =
        std::make_shared<std::atomic<uint64_t>>(0);
    std::atomic<uint64_t> app_tx_queue_evictions_acc{0};
    DatagramAir air;
    mutable std::mutex stats_mu;
    UpstreamStats upstream_stats;
    std::atomic<bool> is_active{false};
    bool bound = false;
};
}  // namespace winject
#endif
