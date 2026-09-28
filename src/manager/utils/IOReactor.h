#ifndef WINJECT_MANAGER_UTILS_IO_REACTOR_H_
#define WINJECT_MANAGER_UTILS_IO_REACTOR_H_

#include <bfc/epoll_reactor.hpp>
#include <functional>

namespace winject
{

using IOReactor = bfc::epoll_reactor<std::function<void()>>;

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_IO_REACTOR_H_
