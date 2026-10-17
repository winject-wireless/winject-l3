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
        reactor_ = reactor;
    }

    void arm(std::chrono::microseconds after, std::function<void()> fn)
    {
        cancel();
        if (reactor_ == nullptr)
        {
            return;
        }
        const uint64_t seq = ++seq_;
        const IOTimerId id =
            reactor_->get_timer().wait_us(after.count() > 0 ? after.count() : 0,
                                          [this, seq, fn = std::move(fn)]()
                                          {
                                              if (!armed_ || armed_->seq != seq)
                                              {
                                                  return;
                                              }
                                              armed_.reset();
                                              fn();
                                          });
        armed_ = Armed{id, seq};
    }

    void cancel()
    {
        if (armed_ && reactor_ != nullptr)
        {
            reactor_->get_timer().cancel(armed_->id);
        }
        armed_.reset();
    }

    bool armed() const
    {
        return armed_.has_value();
    }

private:
    struct Armed
    {
        IOTimerId id;
        uint64_t seq;
    };

    IOReactor* reactor_ = nullptr;
    std::optional<Armed> armed_;
    uint64_t seq_ = 0;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_ONE_SHOT_TIMER_H_
