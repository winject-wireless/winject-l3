#ifndef WINJECT_MANAGER_UPSTREAM_QUEUE_H_
#define WINJECT_MANAGER_UPSTREAM_QUEUE_H_

#include "endpoint/UpstreamQueues.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <memory>

namespace winject
{

// App-facing queue transport, created and owned by L3 for a queue upstream
// with no registry names (Config::on_upstream_queue). A retained handle is
// safe after teardown, but connected() becomes false and sends are rejected.
class UpstreamQueue
{
public:
    UpstreamQueue(std::shared_ptr<ReactiveLossyQueue> ingress,
                  std::shared_ptr<ReactiveLossyQueue> egress,
                  std::shared_ptr<std::atomic<uint64_t>> radio_evictions = {})
        : ingress_(std::move(ingress)),
          egress_(std::move(egress)),
          radio_evictions_(std::move(radio_evictions))
    {
    }
    uint64_t radio_queue_evictions() const
    {
        return radio_evictions_ ? radio_evictions_->load() : 0;
    }
    bool connected() const
    {
        return active_.load();
    }
    // False when disconnected or len is outside 1..k_stream_payload_max; an
    // accepted packet may still evict the oldest queued one.
    bool try_send(const uint8_t* data, size_t len)
    {
        if (!connected() || !data || !len || len > k_stream_payload_max)
        {
            return false;
        }
        if (ingress_->push(bfc::shared_sized_buffer::copy_from(data, len)) >=
            ingress_->capacity())
        {
            ++ingress_evictions;
        }
        return true;
    }
    auto receive()
    {
        return egress_->pop();
    }
    void disconnect()
    {
        active_.store(false);
    }

    std::atomic<uint32_t> ingress_evictions{0};
    std::atomic<uint64_t> rx_packets{0};
    std::atomic<uint64_t> rx_evictions{0};

private:
    std::shared_ptr<ReactiveLossyQueue> ingress_;
    std::shared_ptr<ReactiveLossyQueue> egress_;
    std::shared_ptr<std::atomic<uint64_t>> radio_evictions_;
    std::atomic<bool> active_{true};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UPSTREAM_QUEUE_H_
