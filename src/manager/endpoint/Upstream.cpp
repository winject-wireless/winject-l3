#include "endpoint/Upstream.h"

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
            &upstream_stats, &stats_mu))
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
    is_active.store(true, std::memory_order_release);
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
    // A handler of this instance on the reactor thread holds the data-path
    // mutex that stop() takes.
    if (reactor->is_reactor_thread())
    {
        throw std::logic_error("upstream teardown on cv reactor thread");
    }
    // No reactor fence: a stopped reactor never runs one. A handler already
    // dispatched sees !active() or a stopped air; stop() waits out the one
    // inside air and cancels FEC timers. Leases keep the queues alive past
    // the reactor's raw-pointer snapshot.
    unbind_callbacks();
    air.stop();
}

Upstream::TxSequences Upstream::tx_sequences() const
{
    if (bound)
    {
        throw std::logic_error("upstream sequences read while bound");
    }
    std::lock_guard<std::mutex> lock(stats_mu);
    return {upstream_stats.air_tx, air.fec_tx_sdu_seq()};
}

void Upstream::resume_tx_sequences(const TxSequences& sequences)
{
    if (bound)
    {
        throw std::logic_error("upstream sequences resumed while bound");
    }
    std::lock_guard<std::mutex> lock(stats_mu);
    upstream_stats.air_tx = sequences.air;
    air.set_fec_tx_sdu_seq(sequences.fec_sdu);
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
    is_active.store(false, std::memory_order_release);
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
        if (!active())
        {
            return;  // leave input to a replacement consumer
        }
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
        if (!active())
        {
            return;
        }
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
    return {air.app_tx_packets(), air.app_tx_bytes(), air.app_rx_oversize_pkt(),
            air.tx_queue_drops(),
            app_tx_queue_evictions_acc.load(std::memory_order_relaxed)};
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
