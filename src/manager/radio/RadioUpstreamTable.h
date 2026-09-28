#ifndef WINJECT_MANAGER_RADIO_RADIO_UPSTREAM_TABLE_H_
#define WINJECT_MANAGER_RADIO_RADIO_UPSTREAM_TABLE_H_

#include "radio/RadioUpstreamEntry.h"

#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace winject
{

class Upstream;
class WifiUdp;

// Per-upstream registration shared by TxMux (host→air) and RxDemux (air→host).
class RadioUpstreamTable
{
public:
    void add(const std::shared_ptr<Upstream>& up,
             const std::shared_ptr<WifiUdp>& radio, uint8_t bus_tx,
             uint8_t bus_rx, size_t budget);
    bool set_budget(size_t index, size_t budget);
    bool get_budget(size_t index, size_t* budget) const;
    bool peek_seq_lost(size_t index, uint64_t* lost) const;
    uint64_t collect_seq_loss_delta();

    std::mutex& mutex()
    {
        return mu_;
    }
    const std::mutex& mutex() const
    {
        return mu_;
    }
    std::vector<RadioUpstreamEntry>& entries()
    {
        return entries_;
    }
    const std::vector<RadioUpstreamEntry>& entries() const
    {
        return entries_;
    }

private:
    mutable std::mutex mu_;
    std::vector<RadioUpstreamEntry> entries_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RADIO_UPSTREAM_TABLE_H_
