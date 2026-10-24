#include "endpoint/Upstream.h"

#include <stdexcept>

namespace winject
{
Upstream::Upstream(std::shared_ptr<Reactor> reactor,
                   const UpstreamConfig& config,
                   std::shared_ptr<BoundedLossyQueue> radio_tx_queue,
                   std::shared_ptr<ReactiveLossyQueue> app_tx_queue)
    : app_tx_queue_(app_tx_queue ? std::move(app_tx_queue)
                                 : std::make_shared<ReactiveLossyQueue>()),
      reactor_(std::move(reactor)),
      config_(config),
      rx_queue_(std::make_shared<ReactiveLossyQueue>()),
      radio_tx_queue_(std::move(radio_tx_queue))
{
    if (!reactor_ || !radio_tx_queue_ || !radio_tx_queue_->capacity() ||
        !config.rx_queue_capacity)
    {
        throw std::invalid_argument(
            "upstream requires reactor and nonempty TX queue");
    }
    rx_queue_->set_capacity(config.rx_queue_capacity);
    if (config_.bus_tx == 0)
    {
        throw std::invalid_argument("upstream requires nonzero bus_tx");
    }
    if (!air_.start(
            radio_tx_queue_,
            [this](const uint8_t* p, size_t n)
            {
                deliver_datagram(p, n);
            },
            reactor_, config_, config_.bus_tx, &stats_, &stats_mu_,
            type_name()))
    {
        throw std::invalid_argument("invalid upstream FEC configuration");
    }
}

void Upstream::bind_callbacks()
{
    if (bound_)
    {
        return;
    }
    const auto weak = weak_from_this();
    if (weak.expired())
    {
        throw std::logic_error(
            "bind callbacks after establishing shared ownership");
    }
    air_.set_lifetime(weak);
    bound_ = true;  // destructor also cleans up a partially failed registration
    reactor_->add_read_rdy(*rx_queue_,
                           [weak]
                           {
                               if (auto self = weak.lock())
                               {
                                   self->drain_radio_rx();
                               }
                           });
    reactor_->add_read_rdy(*app_tx_queue_,
                           [weak]
                           {
                               if (auto self = weak.lock())
                               {
                                   self->drain_app_tx_queue();
                               }
                           });
    rx_queue_->set_reactor(reactor_);
    app_tx_queue_->set_reactor(reactor_);
    reactor_->wake_up();  // discover input queued before construction
}

Upstream::~Upstream()
{
    unbind_callbacks();
    air_.stop();
    destroyed_.set_value();
}

void Upstream::unbind_callbacks()
{
    if (bound_)
    {
        reactor_->remove_read_rdy(*rx_queue_);
        reactor_->remove_read_rdy(*app_tx_queue_);
        rx_queue_->set_reactor(nullptr);
        app_tx_queue_->set_reactor(nullptr);
        // CV scans snapshot raw queue pointers; retain both until that scan
        // ends.
        auto leases = std::make_shared<
            std::pair<std::shared_ptr<ReactiveLossyQueue>, std::shared_ptr<ReactiveLossyQueue>>>(
            rx_queue_, app_tx_queue_);
        reactor_->wake_up([leases] {});
        bound_ = false;
    }
}

void Upstream::submit_datagram(const uint8_t* data, size_t len)
{
    air_.accept_app_datagram(data, len);
}
void Upstream::drain_app_tx_queue()
{
    for (int n = 0; n < 32; ++n)
    {
        auto packet = app_tx_queue_->pop();
        if (!packet)
        {
            return;
        }
        submit_datagram(packet->u8(), packet->size());
    }
    if (app_tx_queue_->has_data())
    {
        reactor_->wake_up();
    }
}
void Upstream::drain_radio_rx()
{
    for (int n = 0; n < 32; ++n)
    {
        auto packet = rx_queue_->pop();
        if (!packet)
        {
            return;
        }
        const uint8_t* payload = nullptr;
        size_t size = 0;
        bool fec = false;
        {
            std::lock_guard<std::mutex> lock(stats_mu_);
            if (!accept_air_payload(&stats_, packet->u8(), packet->size(),
                                    &payload, &size, &fec))
            {
                continue;
            }
        }
        air_.on_radio_rx(packet->subview(size_t(payload - packet->u8()), size),
                         fec);
    }
    if (rx_queue_->has_data())
    {
        reactor_->wake_up();
    }
}
UpstreamStats Upstream::stats() const
{
    std::lock_guard<std::mutex> lock(stats_mu_);
    return stats_;
}
uint64_t Upstream::collect_air_seq_loss_delta()
{
    std::lock_guard<std::mutex> lock(stats_mu_);
    const uint64_t delta = stats_.air_rx_gap_loss - stats_.air_rx_gap_loss_seen;
    stats_.air_rx_gap_loss_seen = stats_.air_rx_gap_loss;
    return delta;
}
Upstream::RxCounters Upstream::rx_counters() const
{
    std::lock_guard<std::mutex> lock(rx_counters_mu_);
    return rx_counters_;
}
Upstream::TxCounters Upstream::tx_counters() const
{
    return {air_.app_tx_packets(), air_.app_tx_bytes(),
            air_.app_rx_oversize_pkt(), air_.tx_queue_drops()};
}
void Upstream::get_fec(FecType* type, int* k, int* n) const
{
    if (type)
    {
        *type = config_.fec_type;
    }
    if (k)
    {
        *k = config_.fec_type == FecType::none ? 0 : config_.fec_k;
    }
    if (n)
    {
        *n = config_.fec_type == FecType::none ? 0 : config_.fec_n;
    }
}
void Upstream::tx_pending_stats(uint64_t* packets, uint64_t* bytes) const
{
    uint64_t p = 0, b = 0, qp = 0, qb = 0;
    air_.tx_pending_stats(&p, &b);
    radio_tx_queue_->pending_stats(&qp, &qb);
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
