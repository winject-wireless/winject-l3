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
        : capacity_(capacity), on_push_(std::move(on_push))
    {
        slots_.resize(capacity_);
    }

    size_t push(bfc::shared_sized_buffer packet)
    {
        const size_t packet_bytes = packet.size();
        size_t before = 0;
        bool enqueued = false;
        std::optional<bfc::shared_sized_buffer> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            before = count_;
            if (capacity_ == 0)
            {
                dropped.emplace(std::move(packet));
            }
            else if (count_ < capacity_)
            {
                enqueued = true;
                const size_t tail = (head_ + count_) % capacity_;
                slots_[tail].emplace(std::move(packet));
                ++count_;
            }
            else
            {
                enqueued = true;
                dropped.emplace(std::move(*slots_[head_]));
                slots_[head_].emplace(std::move(packet));
                head_ = (head_ + 1) % capacity_;
            }
            if (enqueued)
            {
                if (dropped)
                {
                    bytes_ -= dropped->size();
                }
                bytes_ += packet_bytes;
            }
        }
        if (enqueued && on_push_)
        {
            on_push_();
        }
        return before;
    }

    size_t head_size()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0)
        {
            return 0;
        }
        return slots_[head_]->size();
    }

    std::optional<bfc::shared_sized_buffer> pop(size_t max = SIZE_MAX)
    {
        std::optional<bfc::shared_sized_buffer> item;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (count_ == 0 || slots_[head_]->size() > max)
            {
                return {};
            }
            item.emplace(std::move(*slots_[head_]));
            slots_[head_].reset();
            head_ = (head_ + 1) % capacity_;
            --count_;
            bytes_ -= item->size();
        }
        return item;
    }

    bool has_data()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ != 0;
    }

    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    size_t capacity() const
    {
        return capacity_;
    }

    void pending_stats(uint64_t* packets, uint64_t* bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (packets)
        {
            *packets = count_;
        }
        if (bytes)
        {
            *bytes = bytes_;
        }
    }

private:
    std::mutex mutex_;
    const size_t capacity_;
    bfc::light_function<void()> on_push_;
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t bytes_ = 0;
    std::vector<std::optional<bfc::shared_sized_buffer>> slots_;
};
}  // namespace winject
#endif  // WINJECT_MANAGER_UTILS_RADIO_QUEUES_H_
