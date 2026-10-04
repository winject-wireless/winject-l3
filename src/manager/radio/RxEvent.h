#ifndef WINJECT_MANAGER_RADIO_RX_EVENT_H_
#define WINJECT_MANAGER_RADIO_RX_EVENT_H_

#include <netinet/in.h>
#include <variant>

namespace winject
{

struct EventCtrlRecvSocketChange
{
    sockaddr_in dplane{};
};

using RxEvent = std::variant<EventCtrlRecvSocketChange>;

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RX_EVENT_H_
