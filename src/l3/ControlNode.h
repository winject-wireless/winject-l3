#ifndef WINJECT_L3_CONTROL_NODE_H_
#define WINJECT_L3_CONTROL_NODE_H_

#include "Control.h"
#include "ControlTypes.h"
#include "console/ConsoleCommands.h"
#include "utils/IOReactor.h"

#include <mutex>

namespace winject_l3
{

// The node side of a Control handle. App attaches its reactor and ops before
// running the reactor and detaches on the reactor thread once it stopped, so
// ops and commands are only touched on that thread.
struct Control::Impl
{
    void attach(winject::IOReactor* node_reactor, winject::ControlOps node_ops);
    void detach();

    // Guards reactor (read by callers to post work).
    std::mutex mutex;
    winject::IOReactor* reactor = nullptr;
    winject::ControlOps ops;
    winject::ConsoleCommands commands;
};

}  // namespace winject_l3

#endif  // WINJECT_L3_CONTROL_NODE_H_
