#ifndef WINJECT_MANAGER_RADIO_TX_EVENT_H_
#define WINJECT_MANAGER_RADIO_TX_EVENT_H_

#include <deque>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
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
        if (q.empty() || !std::holds_alternative<EventData>(q.back()))
        {
            q.emplace_back(EventData{});
        }
    }

    void push(TxEvent ev)
    {
        q.emplace_back(std::move(ev));
    }

    bool pop(TxEvent* out)
    {
        if (q.empty() || out == nullptr)
        {
            return false;
        }
        *out = std::move(q.front());
        q.pop_front();
        return true;
    }

    bool empty() const
    {
        return q.empty();
    }

    void drop_data()
    {
        std::deque<TxEvent> kept;
        for (auto& ev : q)
        {
            if (std::holds_alternative<EventCtrlSendSocketChange>(ev))
            {
                kept.emplace_back(std::move(ev));
            }
        }
        q = std::move(kept);
    }

private:
    std::deque<TxEvent> q;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_TX_EVENT_H_
