#ifndef WINJECT_MANAGER_UTILS_RADIO_QUEUES_H_
#define WINJECT_MANAGER_UTILS_RADIO_QUEUES_H_

#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace winject
{
// Reactive BFC queue; packets retain the slot prefix (route + upstream air
// header). Upstream interprets sequence/FEC; demux only reads the route bus
// byte.
using ReactiveLossyQueue =
    bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;

// Bounded lossy FIFO. push returns occupancy before the call; a non-zero
// capacity and a return equal to capacity means the oldest item was dropped.
// The optional push notification runs after releasing the queue mutex.
class BoundedLossyQueue
{
public:
    explicit BoundedLossyQueue(size_t capacity = 1024,
                               bfc::light_function<void()> on_push = {})
        : max_slots(capacity), on_push_fn(std::move(on_push))
    {
        slots.resize(max_slots);
    }

    size_t push(bfc::shared_sized_buffer packet)
    {
        const size_t packet_bytes = packet.size();
        size_t before = 0;
        bool enqueued = false;
        std::optional<bfc::shared_sized_buffer> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex);
            before = count;
            if (max_slots == 0)
            {
                dropped.emplace(std::move(packet));
            }
            else if (count < max_slots)
            {
                enqueued = true;
                const size_t tail = (head + count) % max_slots;
                slots[tail].emplace(std::move(packet));
                ++count;
            }
            else
            {
                enqueued = true;
                dropped.emplace(std::move(*slots[head]));
                slots[head].emplace(std::move(packet));
                head = (head + 1) % max_slots;
            }
            if (enqueued)
            {
                if (dropped)
                {
                    bytes -= dropped->size();
                }
                bytes += packet_bytes;
            }
        }
        if (enqueued && on_push_fn)
        {
            on_push_fn();
        }
        return before;
    }

    size_t head_size()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (count == 0)
        {
            return 0;
        }
        return slots[head]->size();
    }

    std::optional<bfc::shared_sized_buffer> pop(size_t max = SIZE_MAX)
    {
        std::optional<bfc::shared_sized_buffer> item;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (count == 0 || slots[head]->size() > max)
            {
                return {};
            }
            item.emplace(std::move(*slots[head]));
            slots[head].reset();
            head = (head + 1) % max_slots;
            --count;
            bytes -= item->size();
        }
        return item;
    }

    bool has_data()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return count != 0;
    }

    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return count;
    }

    size_t capacity() const
    {
        return max_slots;
    }

    void pending_stats(uint64_t* packets, uint64_t* bytes)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (packets)
        {
            *packets = count;
        }
        if (bytes)
        {
            *bytes = this->bytes;
        }
    }

private:
    std::mutex mutex;
    const size_t max_slots;
    bfc::light_function<void()> on_push_fn;
    size_t head = 0;
    size_t count = 0;
    uint64_t bytes = 0;
    std::vector<std::optional<bfc::shared_sized_buffer>> slots;
};
}  // namespace winject
#endif  // WINJECT_MANAGER_UTILS_RADIO_QUEUES_H_
