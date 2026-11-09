#ifndef WINJECT_MANAGER_UTILS_IO_REACTOR_H_
#define WINJECT_MANAGER_UTILS_IO_REACTOR_H_

// l3's IO reactor is the project's bfc default reactor: poll or epoll and the
// callback type come from the project's bfc_config.hpp, so l3, vstreamer and
// the firmware share one reactor type and one registry
// (bfc::default_reactor_registry).
#include <bfc/default_reactor.hpp>
#include <bfc/timer.hpp>

namespace winject::core
{

using IOReactor = bfc::default_reactor<>;
using IOTimerId = bfc::timer<>::timer_id_t;

}  // namespace winject::core

#endif  // WINJECT_MANAGER_UTILS_IO_REACTOR_H_
