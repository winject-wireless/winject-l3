#include "endpoint/QueueUpstream.h"

#include <stdexcept>
#include <unordered_set>
namespace winject
{
namespace
{
// App TX queues that have a consumer; one consumer per queue.
std::mutex claims_mutex;
std::unordered_set<ReactiveLossyQueue*> claims;
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
    std::lock_guard<std::mutex> lock(claims_mutex);
    if (!claims.insert(this->app_tx_queue.get()).second)
    {
        throw std::invalid_argument("upstream queue already has a consumer");
    }
    claimed = true;
}
QueueUpstream::~QueueUpstream()
{
    unbind_callbacks();
    release_claim();
}
void QueueUpstream::teardown()
{
    Upstream::teardown();
    release_claim();
}
void QueueUpstream::release_claim()
{
    std::lock_guard<std::mutex> lock(claims_mutex);
    if (claimed)
    {
        claims.erase(app_tx_queue.get());
        claimed = false;
    }
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
