#ifndef WINJECT_L3_PUBLIC_CONSOLE_H_
#define WINJECT_L3_PUBLIC_CONSOLE_H_

#include "winject_l3/Node.h"
#include "winject_l3/Radio.h"
#include "winject_l3/Types.h"
#include "winject_l3/Upstream.h"

#include <bfc/default_reactor.hpp>
#include <bfc/object_registry.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace winject_l3::console
{

// The text m-plane over the public API (docs/mplane.md, wire unchanged). One
// line in, one reply out: "OK ...", "NOK <errno>" or a listing.
//
// The callback form does its work in one Node::post and never blocks the
// caller; done runs on l3's callback thread. done is not called for a line
// with no command. Without a node the reply is "NOK ESRCH".
void execute(const Node& node, const std::string& line,
             std::function<void(const std::string& reply)> done);
// Blocks its caller, so not for a shared reactor. "" for a line with no
// command, "NOK ETIMEDOUT" if no reply in time.
std::string execute(const Node& node, const std::string& line,
                    std::chrono::milliseconds t = k_default_timeout);

// The console's text for each API struct. Public, so another console can
// reuse them.
std::string format(const RadioStats& s);  // the two radio_stats lines
std::string format(uint8_t id, const UpstreamRxStats& s);
std::string format(uint8_t id, const UpstreamTxStats& s);

// Same type and registry as vstreamer's default reactor.
using Reactor = bfc::default_reactor<std::function<void()>>;
using ReactorRegistry = bfc::object_registry<bfc::object_tag<Reactor>>;

// The UDP console: a socket and cmd:<u8> correlation on a named reactor the
// host registered and runs. It calls execute() and never waits on the node.
class Service
{
public:
    Service();
    ~Service();  // stop()
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    // Binds bind_host_port on the reactor named reactor_name and waits for the
    // result, so that reactor must be running (ETIMEDOUT otherwise). ENOENT:
    // no reactor under that name. EALREADY: started. EINVAL: bad address.
    // EADDRINUSE / EACCES / EIO: bind failed.
    Status start(const Node& node, const std::string& bind_host_port,
                 const std::string& reactor_name);
    // From any thread but the named reactor's (EDEADLK there). A reply that
    // arrives after stop is dropped.
    Status stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}  // namespace winject_l3::console

#endif  // WINJECT_L3_PUBLIC_CONSOLE_H_
