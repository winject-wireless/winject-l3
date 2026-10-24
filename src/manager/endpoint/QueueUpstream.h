#ifndef WINJECT_MANAGER_QUEUE_UPSTREAM_H_
#define WINJECT_MANAGER_QUEUE_UPSTREAM_H_
#include "endpoint/Upstream.h"
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

private:
    void deliver_datagram(const uint8_t* data, size_t len) override;
    std::shared_ptr<ReactiveLossyQueue> app_rx_queue_;
    std::shared_ptr<void> claim_;
};
}  // namespace winject
#endif
