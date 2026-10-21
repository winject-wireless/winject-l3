#ifndef WINJECT_MANAGER_QUEUE_ENDPOINT_H_
#define WINJECT_MANAGER_QUEUE_ENDPOINT_H_

#include "endpoint/HostEndpoint.h"
#include "utils/IOReactor.h"

#include <atomic>
#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string>

namespace winject
{

// In-process peer of UdpEndpoint. The host registers two distinct,
// nonzero-capacity lossy queues of bfc::shared_sized_buffer on
// bfc::cv_reactor_circular_lossy_queue_registry and a running
// bfc::cv_reactor<> on bfc::cv_reactor_registry. This endpoint does not
// create or stop them.
//
// UpstreamConfig::endpoint.rx is consumed here (one consumer). It is the
// sender's queue_transport_tx: each entry is one application datagram, the
// same bytes a UDP payload would have been. endpoint.tx is produced here
// and woken on that reactor; it is the sender's queue_transport_rx.
//
// FEC flush uses the manager IOReactor. Arming the timer wakes that
// reactor, because the datagram arrived on the CV reactor thread.
class QueueEndpoint : public HostEndpoint
{
public:
    QueueEndpoint() = default;
    ~QueueEndpoint() override;

    bool open(IOReactor& io_reactor, const UpstreamConfig& cfg,
              std::function<void()> tx_wake = nullptr);
    void close() override;

    const char* type_name() const override
    {
        return "QUEUE";
    }
    std::string endpoint_rx() const override
    {
        return queue_in_name_;
    }
    std::string endpoint_tx() const override
    {
        return queue_out_name_;
    }
    uint64_t queue_out_drops() const
    {
        return queue_out_drops_.load(std::memory_order_relaxed);
    }

protected:
    bool can_deliver_app() const override;
    void deliver_app(const uint8_t* data, size_t len) override;

private:
    using AppQueue =
        bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
    using AppReactor = bfc::cv_reactor<>;

    void on_queue_in();

    IOReactor* io_reactor_ = nullptr;
    std::string queue_in_name_;
    std::string queue_out_name_;
    mutable std::mutex transport_mu_;
    bool opened_ = false;
    std::shared_ptr<AppQueue> queue_in_;
    std::shared_ptr<AppQueue> queue_out_;
    std::shared_ptr<AppReactor> reactor_;
    std::atomic<uint64_t> queue_out_drops_{0};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_QUEUE_ENDPOINT_H_
