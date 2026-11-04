#ifndef WINJECT_L3_PUBLIC_NODE_H_
#define WINJECT_L3_PUBLIC_NODE_H_

#include "Config.h"
#include "winject_l3/Radio.h"
#include "winject_l3/Types.h"
#include "winject_l3/Upstream.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace winject_l3
{

struct Metric
{
    std::string key;
    std::string value;
};

// One l3 instance: reactor and data path. Hands out the two managers. A
// handle is cheap to copy and safe from any thread.
class Node
{
public:
    Node() = default;

    bool running() const;

    UpstreamManager upstreams() const;
    RadioManager radios() const;
    // No keys: all. Same keys and values as the console's get_metrics.
    Result<std::vector<Metric>> metrics(
        const std::vector<std::string>& keys = {}) const;
    // Runs fn(node) on l3's callback thread, off the caller's thread. For a
    // thread that must not wait, such as a shared reactor. ESRCH: no node.
    Status post(std::function<void(const Node&)> fn) const;

private:
    friend struct detail::HandleAccess;
    std::shared_ptr<detail::NodeCore> core;
};

using winject::Config;

// The threads run() starts with std::thread from its calling thread.
enum class Thread
{
    upstream,  // the upstream data path (UDP / queue app transports, FEC)
    node,      // the node's reactor: handle calls, radio m-plane, TX timers
};
// Called on run()'s calling thread just before each thread starts. On
// ESP-IDF a host sets esp_pthread_set_cfg here (stack, priority, core).
using ThreadSetup = std::function<void(Thread)>;

Version version();  // this build (console `version`)
// The process's node: valid any time; calls return ESRCH until run() has
// built the node and after it returns.
Node node();
// Config -> API: creates the radio, adds the upstreams, then adds the radio.
// Validates first (Config::validate). Stops at the first error.
Status apply(const Node& node, const Config& cfg);

// Runs the node. Blocks until stop(); returns 0, or 1 when the config does
// not validate or apply fails. The calling thread becomes l3's callback
// thread: it runs apply, then started, then every Done and every post until
// stop. The node's reactor and the upstream reactor get threads of their own.
int run(const Config& cfg, std::function<void(const Node&)> started = {},
        ThreadSetup thread_setup = {});
// Asks run() to return. Any thread, also before run() starts.
void stop();
// stop() for a signal handler: writes atomics and the wake fd only.
void stop_from_signal();

}  // namespace winject_l3

#endif  // WINJECT_L3_PUBLIC_NODE_H_
