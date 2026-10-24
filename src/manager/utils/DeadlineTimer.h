#ifndef WINJECT_MANAGER_UTILS_DEADLINE_TIMER_H_
#define WINJECT_MANAGER_UTILS_DEADLINE_TIMER_H_

#include <algorithm>
#include <bfc/cv_reactor.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <stdint.h>
#include <utility>

namespace winject
{

// One CV reactor timer that follows a moving deadline. Arm and cancel only on
// the reactor thread (or while it is not running): the reactor re-reads its
// next deadline after every pass, so arming needs no wake_up.
//
// timer::schedule extracts every due callback before running any, so a
// callback cancelled or destroyed by an earlier one in the same pass still
// runs; it checks the shared token and generation and does nothing.
class DeadlineTimer
{
public:
    using Reactor = bfc::cv_reactor<>;
    using Fire = std::function<void()>;

    DeadlineTimer() : token(std::make_shared<Token>())
    {
        token->owner = this;
    }
    ~DeadlineTimer()
    {
        unbind();
        token->owner = nullptr;
    }
    DeadlineTimer(const DeadlineTimer&) = delete;
    DeadlineTimer& operator=(const DeadlineTimer&) = delete;

    void bind(std::shared_ptr<Reactor> reactor, Fire fire)
    {
        unbind();
        this->reactor = std::move(reactor);
        this->fire = std::move(fire);
    }
    void unbind()
    {
        cancel();
        reactor.reset();
        fire = nullptr;
    }
    // A fired callback first locks this owner and bails out once it is gone,
    // so an owner destroyed on another thread is never re-entered.
    void set_lifetime(std::weak_ptr<void> owner)
    {
        token->lifetime = std::move(owner);
        token->managed = true;
    }

    // Fires once at `at`; re-arming with the same deadline keeps the timer.
    void arm(std::chrono::steady_clock::time_point at)
    {
        if (!reactor || (armed && at == armed_at))
        {
            return;
        }
        cancel();
        const auto delay = std::chrono::ceil<std::chrono::microseconds>(
                               at - std::chrono::steady_clock::now())
                               .count();
        std::weak_ptr<Token> weak = token;
        const uint64_t armed_generation = generation;
        timer_id = reactor->get_timer().wait_us(
            std::max<int64_t>(0, delay),
            [weak, armed_generation]()
            {
                auto t = weak.lock();
                if (!t)
                {
                    return;
                }
                auto lease = t->lifetime.lock();
                if (t->managed && !lease)
                {
                    return;
                }
                DeadlineTimer* self = t->owner;
                if (self && self->armed && self->generation == armed_generation)
                {
                    self->armed = false;
                    self->fire();
                }
            });
        armed = true;
        armed_at = at;
    }
    void cancel()
    {
        ++generation;
        if (armed && reactor)
        {
            reactor->get_timer().cancel(timer_id);
        }
        armed = false;
    }
    bool is_armed() const
    {
        return armed;
    }

private:
    struct Token
    {
        DeadlineTimer* owner = nullptr;
        std::weak_ptr<void> lifetime;
        bool managed = false;
    };

    std::shared_ptr<Token> token;
    std::shared_ptr<Reactor> reactor;
    Fire fire;
    Reactor::timer_t::timer_id_t timer_id{};
    std::chrono::steady_clock::time_point armed_at{};
    uint64_t generation = 0;
    bool armed = false;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_DEADLINE_TIMER_H_
