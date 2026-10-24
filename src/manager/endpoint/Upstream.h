#ifndef WINJECT_MANAGER_UPSTREAM_H_
#define WINJECT_MANAGER_UPSTREAM_H_

#include "Config.h"
#include "endpoint/DatagramAir.h"
#include "endpoint/UpstreamQueues.h"
#include "endpoint/UpstreamStats.h"

#include <bfc/cv_reactor.hpp>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

namespace winject
{
// Call bind_callbacks after establishing shared ownership, before publication.
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
        uint64_t packets = 0;
        uint64_t bytes = 0;
        uint64_t oversize_drops = 0;
        uint64_t queue_evictions = 0;
    };

    virtual void bind_callbacks();
    virtual ~Upstream();
    std::shared_ptr<ReactiveLossyQueue> rx_queue() const
    {
        return rx_queue_;
    }
    const UpstreamConfig& config() const
    {
        return config_;
    }
    const char* type_name() const
    {
        return config_.mode == UpstreamMode::queue ? "QUEUE" : "UDP";
    }
    std::string endpoint_rx() const
    {
        return config_.endpoint.rx;
    }
    std::string endpoint_tx() const
    {
        return config_.endpoint.tx;
    }
    RxCounters rx_counters() const;
    TxCounters tx_counters() const;
    UpstreamStats stats() const;
    uint64_t collect_air_seq_loss_delta();
    void get_fec(FecType* type, int* k, int* n) const;
    int fec_timeout_ms() const
    {
        return config_.fec_timeout_ms;
    }
    void tx_pending_stats(uint64_t* packets, uint64_t* bytes) const;

    uint64_t app_rx_bytes() const
    {
        return air_.app_rx_bytes();
    }
    uint64_t app_rx_packets() const
    {
        return air_.app_rx_packets();
    }
    uint64_t app_rx_oversize_pkt() const
    {
        return air_.app_rx_oversize_pkt();
    }
    uint64_t app_tx_bytes() const
    {
        return air_.app_tx_bytes();
    }
    uint64_t app_tx_packets() const
    {
        return air_.app_tx_packets();
    }
    uint64_t fec_air_rx_bytes() const
    {
        return air_.fec_air_rx_bytes();
    }
    uint64_t fec_air_rx_packets() const
    {
        return air_.fec_air_rx_packets();
    }
    uint64_t fec_air_rx_unexpected() const
    {
        return air_.fec_air_rx_unexpected();
    }
    uint64_t fec_air_tx_bytes() const
    {
        return air_.fec_air_tx_bytes();
    }
    uint64_t fec_air_tx_packets() const
    {
        return air_.fec_air_tx_packets();
    }
    uint64_t fec_recovered() const
    {
        return air_.fec_recovered();
    }
    uint64_t fec_decode_fail() const
    {
        return air_.fec_decode_fail();
    }
    uint64_t tx_queue_drops() const
    {
        return air_.tx_queue_drops();
    }

protected:
    Upstream(std::shared_ptr<Reactor> reactor, const UpstreamConfig& config,
             std::shared_ptr<BoundedLossyQueue> radio_tx_queue,
             std::shared_ptr<ReactiveLossyQueue> app_tx_queue = {});
    // Executed only by the supplied CV reactor, including FEC timer delivery.
    virtual void deliver_datagram(const uint8_t* data, size_t len) = 0;
    void unbind_callbacks();
    void submit_datagram(const uint8_t* data, size_t len);
    void reject_oversize_datagram(size_t len)
    {
        air_.reject_oversize_datagram(len);
    }
    std::shared_ptr<ReactiveLossyQueue> app_tx_queue_;
    std::shared_ptr<Reactor> reactor_;
    UpstreamConfig config_;
    mutable std::mutex rx_counters_mu_;
    RxCounters rx_counters_;

private:
    friend class App;
    // App waits for destructor completion when replacing an in-flight callback.
    std::promise<void> destroyed_;
    std::shared_future<void> destruction_ = destroyed_.get_future().share();
    void drain_radio_rx();
    void drain_app_tx_queue();
    std::shared_ptr<ReactiveLossyQueue> rx_queue_;
    std::shared_ptr<BoundedLossyQueue> radio_tx_queue_;
    DatagramAir air_;
    mutable std::mutex stats_mu_;
    UpstreamStats stats_;
    bool bound_ = false;
};
}  // namespace winject
#endif
