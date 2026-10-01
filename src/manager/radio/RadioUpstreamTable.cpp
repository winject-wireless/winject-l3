#include "radio/RadioUpstreamTable.h"

#include "endpoint/Upstream.h"
#include "radio/WifiUdp.h"

namespace winject
{

bool RadioUpstreamTable::remove(size_t index)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (index >= entries_.size())
    {
        return false;
    }
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index));
    return true;
}

void RadioUpstreamTable::add(const std::shared_ptr<Upstream>& up,
                             const std::shared_ptr<WifiUdp>& radio,
                             uint8_t bus_tx, uint8_t bus_rx, size_t budget)
{
    std::lock_guard<std::mutex> lock(mu_);
    entries_.push_back(RadioUpstreamEntry{up, radio, bus_tx, bus_rx, budget});
}

bool RadioUpstreamTable::set_budget(size_t index, size_t budget)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (index >= entries_.size() || budget == 0)
    {
        return false;
    }
    entries_[index].budget = budget;
    return true;
}

bool RadioUpstreamTable::get_budget(size_t index, size_t* budget) const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (index >= entries_.size() || budget == nullptr)
    {
        return false;
    }
    *budget = entries_[index].budget;
    return true;
}

bool RadioUpstreamTable::peek_seq_lost(size_t index, uint64_t* lost) const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (lost == nullptr || index >= entries_.size())
    {
        return false;
    }
    if (entries_[index].up == nullptr)
    {
        return false;
    }
    *lost = entries_[index].up->stats().air_rx_gap_loss;
    return true;
}

uint64_t RadioUpstreamTable::collect_seq_loss_delta()
{
    std::lock_guard<std::mutex> lock(mu_);
    uint64_t total_lost = 0;
    for (size_t i = 0; i < entries_.size(); i++)
    {
        if (entries_[i].up == nullptr)
        {
            continue;
        }
        total_lost += entries_[i].up->collect_air_seq_loss_delta();
    }
    return total_lost;
}

}  // namespace winject
