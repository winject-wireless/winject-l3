#include "endpoint/QueueUpstream.h"

#include <stdexcept>
#include <unordered_map>
namespace winject
{
namespace
{
std::mutex claims_mutex;
std::unordered_map<ReactiveLossyQueue*, std::weak_ptr<void>> claims;
std::shared_ptr<ReactiveLossyQueue> validate_input(
    const std::shared_ptr<ReactiveLossyQueue>& in,
    const std::shared_ptr<ReactiveLossyQueue>& out)
{
    if (!in || !out || in == out || !in->capacity() || !out->capacity())
    {
        throw std::invalid_argument(
            "queue upstream requires distinct nonempty queues");
    }
    return in;
}
}  // namespace
QueueUpstream::QueueUpstream(std::shared_ptr<Reactor> reactor,
                             const UpstreamConfig& config,
                             std::shared_ptr<BoundedLossyQueue> radio_tx_queue,
                             std::shared_ptr<ReactiveLossyQueue> input,
                             std::shared_ptr<ReactiveLossyQueue> output)
    : Upstream(std::move(reactor), config, std::move(radio_tx_queue),
               validate_input(input, output)),
      output_(std::move(output))
{
    if (config.mode != UpstreamMode::queue)
    {
        throw std::invalid_argument("queue upstream requires queue mode");
    }
    auto claim = std::make_shared<int>(0);
    std::lock_guard<std::mutex> lock(claims_mutex);
    if (!claims[app_tx_queue_.get()].expired())
    {
        throw std::invalid_argument("upstream queue already has a consumer");
    }
    claims[app_tx_queue_.get()] = claim;
    claim_ = std::move(claim);
}
QueueUpstream::~QueueUpstream()
{
    unbind_callbacks();
    std::lock_guard<std::mutex> lock(claims_mutex);
    claims.erase(app_tx_queue_.get());
}
void QueueUpstream::deliver_datagram(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lock(rx_counters_mu_);
    ++rx_counters_.delivery_attempts;
    const auto capacity = output_->capacity();
    if (!capacity)
    {
        ++rx_counters_.send_errors;
        return;
    }
    if (output_->push(bfc::shared_sized_buffer::copy_from(data, len)) >=
        capacity)
    {
        ++rx_counters_.queue_evictions;
    }
    ++rx_counters_.accepted_packets;
    rx_counters_.accepted_bytes += len;
}
}  // namespace winject
