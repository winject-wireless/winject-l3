#ifndef WINJECT_MANAGER_UTILS_ONE_SHOT_TIMER_H_
#define WINJECT_MANAGER_UTILS_ONE_SHOT_TIMER_H_

#include "utils/IOReactor.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace winject
{

// One pending reactor timer. arm() replaces the previous arm. bfc may still
// run a timer cancelled while it was due, so a callback runs only if it is
// the latest arm. Reactor thread only; must outlive the reactor's timers.
class OneShotTimer
{
public:
    OneShotTimer() = default;
    OneShotTimer(const OneShotTimer&) = delete;
    OneShotTimer& operator=(const OneShotTimer&) = delete;

    void bind(IOReactor* reactor)
    {
        this->reactor = reactor;
    }

    void arm(std::chrono::microseconds after, std::function<void()> fn)
    {
        cancel();
        if (this->reactor == nullptr)
        {
            return;
        }
        const uint64_t seq = ++this->seq;
        const IOTimerId id = this->reactor->get_timer().wait_us(
            after.count() > 0 ? after.count() : 0,
            [this, seq, fn = std::move(fn)]()
            {
                if (!armed_state || armed_state->seq != seq)
                {
                    return;
                }
                armed_state.reset();
                fn();
            });
        armed_state = Armed{id, seq};
    }

    void cancel()
    {
        if (armed_state && this->reactor != nullptr)
        {
            this->reactor->get_timer().cancel(armed_state->id);
        }
        armed_state.reset();
    }

    bool armed() const
    {
        return armed_state.has_value();
    }

private:
    struct Armed
    {
        IOTimerId id;
        uint64_t seq;
    };

    IOReactor* reactor = nullptr;
    std::optional<Armed> armed_state;
    uint64_t seq = 0;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_ONE_SHOT_TIMER_H_
