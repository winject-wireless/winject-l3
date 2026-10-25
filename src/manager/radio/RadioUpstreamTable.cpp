#include "radio/RadioUpstreamTable.h"

#include "endpoint/Upstream.h"
#include "radio/RadioDplane.h"

namespace winject
{

bool RadioUpstreamTable::remove(size_t index)
{
    std::lock_guard<std::mutex> lock(mu);
    if (index >= upstream_entries.size())
    {
        return false;
    }
    upstream_entries.erase(upstream_entries.begin() + static_cast<ptrdiff_t>(index));
    return true;
}

void RadioUpstreamTable::add(const std::shared_ptr<Upstream>& up,
                             const std::shared_ptr<BoundedLossyQueue>& tx_queue,
                             const std::shared_ptr<RadioDplane>& radio,
                             uint8_t bus_tx, uint8_t bus_rx, size_t budget)
{
    std::lock_guard<std::mutex> lock(mu);
    upstream_entries.push_back(RadioUpstreamEntry{
        up, up->rx_queue(), up->rx_queue_eviction_counter(), tx_queue, radio,
        bus_tx, bus_rx, budget});
}

void RadioUpstreamTable::add(std::shared_ptr<ReactiveLossyQueue> rx_queue,
                             std::shared_ptr<BoundedLossyQueue> tx_queue,
                             const std::shared_ptr<RadioDplane>& radio,
                             uint8_t bus_tx, uint8_t bus_rx, size_t budget)
{
    std::lock_guard<std::mutex> lock(mu);
    upstream_entries.push_back(RadioUpstreamEntry{
                                          {},
                                          std::move(rx_queue),
                                          std::make_shared<std::atomic<uint64_t>>(0),
                                          std::move(tx_queue),
                                          radio,
                                          bus_tx,
                                          bus_rx,
                                          budget});
}
void RadioUpstreamTable::clear()
{
    std::lock_guard<std::mutex> lock(mu);
    upstream_entries.clear();
}

bool RadioUpstreamTable::set_budget(size_t index, size_t budget)
{
    std::lock_guard<std::mutex> lock(mu);
    if (index >= upstream_entries.size() || budget == 0)
    {
        return false;
    }
    upstream_entries[index].budget = budget;
    return true;
}

bool RadioUpstreamTable::get_budget(size_t index, size_t* budget) const
{
    std::lock_guard<std::mutex> lock(mu);
    if (index >= upstream_entries.size() || budget == nullptr)
    {
        return false;
    }
    *budget = upstream_entries[index].budget;
    return true;
}

bool RadioUpstreamTable::peek_seq_lost(size_t index, uint64_t* lost) const
{
    std::lock_guard<std::mutex> lock(mu);
    if (lost == nullptr || index >= upstream_entries.size())
    {
        return false;
    }
    auto up = upstream_entries[index].up.lock();
    if (!up)
    {
        return false;
    }
    *lost = up->stats().air_rx_gap_loss;
    return true;
}

uint64_t RadioUpstreamTable::collect_seq_loss_delta()
{
    std::lock_guard<std::mutex> lock(mu);
    uint64_t total_lost = 0;
    for (size_t i = 0; i < upstream_entries.size(); i++)
    {
        auto up = upstream_entries[i].up.lock();
        if (!up)
        {
            continue;
        }
        total_lost += up->collect_air_seq_loss_delta();
    }
    return total_lost;
}

}  // namespace winject
