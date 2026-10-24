#include "endpoint/QueueUpstream.h"

#include <stdexcept>
#include <unordered_map>
namespace winject
{
namespace
{
std::mutex claims_mutex;
std::unordered_map<ReactiveLossyQueue*, std::weak_ptr<void>> claims;
std::shared_ptr<ReactiveLossyQueue> validate_app_queues(
    const std::shared_ptr<ReactiveLossyQueue>& app_tx_queue,
    const std::shared_ptr<ReactiveLossyQueue>& app_rx_queue)
{
    if (!app_tx_queue || !app_rx_queue || app_tx_queue == app_rx_queue ||
        !app_tx_queue->capacity() || !app_rx_queue->capacity())
    {
        throw std::invalid_argument(
            "queue upstream requires distinct nonempty queues");
    }
    return app_tx_queue;
}
}  // namespace
QueueUpstream::QueueUpstream(std::shared_ptr<Reactor> reactor,
                             const UpstreamConfig& config,
                             std::shared_ptr<BoundedLossyQueue> radio_tx_queue,
                             std::shared_ptr<ReactiveLossyQueue> app_tx_queue,
                             std::shared_ptr<ReactiveLossyQueue> app_rx_queue)
    : Upstream(std::move(reactor), config, std::move(radio_tx_queue),
               validate_app_queues(app_tx_queue, app_rx_queue)),
      app_rx_queue(std::move(app_rx_queue))
{
    if (config.mode != UpstreamMode::queue)
    {
        throw std::invalid_argument("queue upstream requires queue mode");
    }
    claim = std::make_shared<int>(0);
    std::lock_guard<std::mutex> lock(claims_mutex);
    if (!claims[app_tx_queue.get()].expired())
    {
        throw std::invalid_argument("upstream queue already has a consumer");
    }
    claims[app_tx_queue.get()] = claim;
}
QueueUpstream::~QueueUpstream()
{
    unbind_callbacks();
    std::lock_guard<std::mutex> lock(claims_mutex);
    claims.erase(app_tx_queue.get());
}
void QueueUpstream::deliver_datagram(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lock(rx_counters_mu);
    ++stored_rx_counters.delivery_attempts;
    const auto capacity = app_rx_queue->capacity();
    if (!capacity)
    {
        ++stored_rx_counters.send_errors;
        return;
    }
    if (app_rx_queue->push(bfc::shared_sized_buffer::copy_from(data, len)) >=
        capacity)
    {
        ++stored_rx_counters.queue_evictions;
    }
    ++stored_rx_counters.accepted_packets;
    stored_rx_counters.accepted_bytes += len;
}
}  // namespace winject
