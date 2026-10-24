#ifndef WINJECT_MANAGER_RADIO_RADIO_UPSTREAM_ENTRY_H_
#define WINJECT_MANAGER_RADIO_RADIO_UPSTREAM_ENTRY_H_

#include "endpoint/UpstreamQueues.h"

#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace winject
{

class Upstream;
class WifiUdp;

// One configured host upstream on the shared radio (not an MPDU PduSlot).
struct RadioUpstreamEntry
{
    std::weak_ptr<Upstream>
        up;  // statistics only; registration does not own endpoint
    std::shared_ptr<ReactiveLossyQueue> rx_queue;
    // Oldest packets the demux evicted from rx_queue.
    std::shared_ptr<std::atomic<uint64_t>> rx_queue_evictions;
    std::shared_ptr<BoundedLossyQueue> tx_queue;
    std::shared_ptr<WifiUdp> radio;
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    size_t budget = 0;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RADIO_UPSTREAM_ENTRY_H_
