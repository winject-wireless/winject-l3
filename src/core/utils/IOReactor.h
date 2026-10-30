#ifndef WINJECT_MANAGER_UTILS_IO_REACTOR_H_
#define WINJECT_MANAGER_UTILS_IO_REACTOR_H_

#include <functional>

#if defined(ESP_PLATFORM)
// ESP-IDF has poll() but no epoll. Timers wait in whole milliseconds here
// (epoll_pwait2 gives microseconds on Linux).
#include <bfc/poll_reactor.hpp>
#else
#include <bfc/epoll_reactor.hpp>
#endif

namespace winject
{

#if defined(ESP_PLATFORM)
using IOReactor = bfc::poll_reactor<std::function<void()>>;
#else
using IOReactor = bfc::epoll_reactor<std::function<void()>>;
#endif
using IOTimerId = bfc::timer<std::function<void()>>::timer_id_t;

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_IO_REACTOR_H_
