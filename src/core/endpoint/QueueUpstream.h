#ifndef WINJECT_MANAGER_QUEUE_UPSTREAM_H_
#define WINJECT_MANAGER_QUEUE_UPSTREAM_H_
#include "endpoint/Upstream.h"
#include "endpoint/UpstreamQueue.h"
namespace winject
{
class QueueUpstream final : public Upstream
{
public:
    QueueUpstream(std::shared_ptr<Reactor> reactor,
                  const UpstreamConfig& config,
                  std::shared_ptr<BoundedLossyQueue> radio_tx_queue,
                  std::shared_ptr<ReactiveLossyQueue> app_tx_queue,
                  std::shared_ptr<ReactiveLossyQueue> app_rx_queue);
    ~QueueUpstream() override;
    std::shared_ptr<UpstreamQueue> app_transport() const
    {
        return app_transport_;
    }
    // Also releases the app TX queue to a replacement consumer.
    void teardown() override;

private:
    void deliver_datagram(const uint8_t* data, size_t len) override;
    void deliver_owned_datagram(bfc::shared_sized_buffer packet) override;
    void release_claim();
    std::shared_ptr<ReactiveLossyQueue> app_rx_queue;
    bool claimed = false;
    std::shared_ptr<UpstreamQueue> app_transport_;
};
}  // namespace winject
#endif
