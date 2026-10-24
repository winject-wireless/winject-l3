#include "endpoint/Upstream.h"

#include "utils/Log.h"

#include <condition_variable>
#include <stdexcept>

namespace winject
{
Upstream::Upstream(std::shared_ptr<Reactor> reactor,
                   const UpstreamConfig& config,
                   std::shared_ptr<BoundedLossyQueue> radio_tx_queue,
                   std::shared_ptr<ReactiveLossyQueue> app_tx_queue)
    : app_tx_queue(app_tx_queue ? std::move(app_tx_queue)
                                 : std::make_shared<ReactiveLossyQueue>()),
      reactor(std::move(reactor)),
      upstream_config(config),
      host_rx_queue(std::make_shared<ReactiveLossyQueue>()),
      radio_tx_queue(std::move(radio_tx_queue))
{
    if (!this->reactor || !this->radio_tx_queue ||
        !this->radio_tx_queue->capacity() || !upstream_config.rx_queue_capacity)
    {
        throw std::invalid_argument(
            "upstream requires reactor and nonempty TX queue");
    }
    host_rx_queue->set_capacity(upstream_config.rx_queue_capacity);
    if (upstream_config.bus_tx == 0)
    {
        throw std::invalid_argument("upstream requires nonzero bus_tx");
    }
    if (!air.start(
            this->radio_tx_queue,
            [this](const uint8_t* p, size_t n)
            {
                deliver_datagram(p, n);
            },
            this->reactor, upstream_config, upstream_config.bus_tx,
            &upstream_stats, &stats_mu, type_name()))
    {
        throw std::invalid_argument("invalid upstream FEC configuration");
    }
}

void Upstream::bind_callbacks()
{
    if (bound)
    {
        return;
    }
    const auto weak = weak_from_this();
    if (weak.expired())
    {
        throw std::logic_error(
            "bind callbacks after establishing shared ownership");
    }
    air.set_lifetime(weak);
    bound = true;  // destructor also cleans up a partially failed registration
    reactor->add_read_rdy(*host_rx_queue,
                           [weak]
                           {
                               if (auto self = weak.lock())
                               {
                                   self->drain_radio_rx();
                               }
                           });
    reactor->add_read_rdy(*app_tx_queue,
                           [weak]
                           {
                               if (auto self = weak.lock())
                               {
                                   self->drain_app_tx_queue();
                               }
                           });
    host_rx_queue->set_reactor(reactor);
    app_tx_queue->set_reactor(reactor);
    reactor->wake_up();  // discover input queued before construction
}

void Upstream::teardown()
{
    if (!bound)
    {
        return;
    }
    if (reactor->is_reactor_thread())
    {
        throw std::logic_error("upstream teardown on cv reactor thread");
    }
    reactor->remove_read_rdy(*host_rx_queue);
    reactor->remove_read_rdy(*app_tx_queue);
    host_rx_queue->set_reactor(nullptr);
    app_tx_queue->set_reactor(nullptr);
    // The fence runs after any in-flight queue handler or FEC timer. A stopped
    // reactor never runs it, so the wait is bounded and the closure must not
    // borrow this once the wait gives up.
    struct Fence
    {
        std::mutex mutex;
        std::condition_variable cv;
        DatagramAir* air = nullptr;
        bool done = false;
    };
    auto fence = std::make_shared<Fence>();
    fence->air = &air;
    reactor->wake_up(
        [fence]()
        {
            std::lock_guard<std::mutex> lock(fence->mutex);
            if (fence->air)
            {
                fence->air->stop();
            }
            fence->done = true;
            fence->cv.notify_one();
        });
    std::unique_lock<std::mutex> lock(fence->mutex);
    if (!fence->cv.wait_for(lock, k_fence_timeout,
                            [&fence]
                            {
                                return fence->done;
                            }))
    {
        fence->air = nullptr;
        lock.unlock();
        LOG_WRN("upstream teardown: cv reactor fence timed out, stopping inline");
        air.stop();  // fences an in-flight timer itself
    }
    bound = false;
}

Upstream::~Upstream()
{
    unbind_callbacks();
    air.stop();
}

void Upstream::unbind_callbacks()
{
    if (!bound)
    {
        return;
    }
    reactor->remove_read_rdy(*host_rx_queue);
    reactor->remove_read_rdy(*app_tx_queue);
    host_rx_queue->set_reactor(nullptr);
    app_tx_queue->set_reactor(nullptr);
    // CV scans snapshot raw queue pointers; retain both until that scan ends.
    auto leases = std::make_shared<
        std::pair<std::shared_ptr<ReactiveLossyQueue>, std::shared_ptr<ReactiveLossyQueue>>>(
        host_rx_queue, app_tx_queue);
    reactor->wake_up([leases] {});
    bound = false;
}

void Upstream::submit_datagram(const uint8_t* data, size_t len)
{
    air.accept_app_datagram(data, len);
}
void Upstream::drain_app_tx_queue()
{
    for (int n = 0; n < 32; ++n)
    {
        auto packet = app_tx_queue->pop();
        if (!packet)
        {
            return;
        }
        submit_datagram(packet->u8(), packet->size());
    }
    if (app_tx_queue->has_data())
    {
        reactor->wake_up();
    }
}
void Upstream::drain_radio_rx()
{
    for (int n = 0; n < 32; ++n)
    {
        auto packet = host_rx_queue->pop();
        if (!packet)
        {
            return;
        }
        const uint8_t* payload = nullptr;
        size_t size = 0;
        bool fec = false;
        {
            std::lock_guard<std::mutex> lock(stats_mu);
            if (!accept_air_payload(&upstream_stats, packet->u8(), packet->size(),
                                    &payload, &size, &fec))
            {
                continue;
            }
        }
        air.on_radio_rx(packet->subview(size_t(payload - packet->u8()), size),
                         fec);
    }
    if (host_rx_queue->has_data())
    {
        reactor->wake_up();
    }
}
UpstreamStats Upstream::stats() const
{
    std::lock_guard<std::mutex> lock(stats_mu);
    return upstream_stats;
}
uint64_t Upstream::collect_air_seq_loss_delta()
{
    std::lock_guard<std::mutex> lock(stats_mu);
    const uint64_t delta =
        upstream_stats.air_rx_gap_loss - upstream_stats.air_rx_gap_loss_seen;
    upstream_stats.air_rx_gap_loss_seen = upstream_stats.air_rx_gap_loss;
    return delta;
}
Upstream::RxCounters Upstream::rx_counters() const
{
    std::lock_guard<std::mutex> lock(rx_counters_mu);
    return stored_rx_counters;
}
Upstream::TxCounters Upstream::tx_counters() const
{
    return {air.app_tx_packets(), air.app_tx_bytes(),
            air.app_rx_oversize_pkt(), air.tx_queue_drops()};
}
void Upstream::get_fec(FecType* type, int* k, int* n) const
{
    if (type)
    {
        *type = upstream_config.fec_type;
    }
    if (k)
    {
        *k = upstream_config.fec_type == FecType::none ? 0 : upstream_config.fec_k;
    }
    if (n)
    {
        *n = upstream_config.fec_type == FecType::none ? 0 : upstream_config.fec_n;
    }
}
void Upstream::tx_pending_stats(uint64_t* packets, uint64_t* bytes) const
{
    uint64_t p = 0, b = 0, qp = 0, qb = 0;
    air.tx_pending_stats(&p, &b);
    radio_tx_queue->pending_stats(&qp, &qb);
    if (packets)
    {
        *packets = p + qp;
    }
    if (bytes)
    {
        *bytes = b + qb;
    }
}
}  // namespace winject
