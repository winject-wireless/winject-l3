#ifndef WINJECT_MANAGER_RADIO_TX_EVENT_H_
#define WINJECT_MANAGER_RADIO_TX_EVENT_H_

#include <deque>
#include <memory>
#include <netinet/in.h>
#include <variant>

namespace winject
{

class WifiUdp;

struct EventData
{
};

struct EventCtrlSendSocketChange
{
    std::shared_ptr<WifiUdp> radio;
    sockaddr_in dplane{};
};

using TxEvent = std::variant<EventData, EventCtrlSendSocketChange>;

class TxEventQueue
{
public:
    void push_data()
    {
        if (q_.empty() || !std::holds_alternative<EventData>(q_.back()))
        {
            q_.emplace_back(EventData{});
        }
    }

    void push(TxEvent ev)
    {
        q_.emplace_back(std::move(ev));
    }

    bool pop(TxEvent* out)
    {
        if (q_.empty() || out == nullptr)
        {
            return false;
        }
        *out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    bool empty() const
    {
        return q_.empty();
    }

    void drop_data()
    {
        std::deque<TxEvent> kept;
        for (auto& ev : q_)
        {
            if (std::holds_alternative<EventCtrlSendSocketChange>(ev))
            {
                kept.emplace_back(std::move(ev));
            }
        }
        q_ = std::move(kept);
    }

private:
    std::deque<TxEvent> q_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_TX_EVENT_H_
