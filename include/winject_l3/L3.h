#ifndef WINJECT_L3_PUBLIC_L3_H_
#define WINJECT_L3_PUBLIC_L3_H_

#include "Config.h"
#include "winject_l3/Radio.h"
#include "winject_l3/Types.h"
#include "winject_l3/Upstream.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace winject
{

struct Metric
{
    std::string key;
    std::string value;
};

using core::Config;

// The l3 applet: radio, upstreams and the data path between them, with its
// own reactor threads. One per process. An L3 is a handle to it: cheap to
// copy, safe from any thread; it controls the applet and carries no data
// (data goes through the upstreams' sockets and queues).
class L3
{
public:
    // The threads run() starts with std::thread from its calling thread.
    enum class Thread
    {
        upstream,  // the upstream data path (UDP / queue app transports, FEC)
        l3,        // l3's reactor: handle calls, radio m-plane, TX timers
    };
    // Called on run()'s calling thread just before each thread starts. On
    // ESP-IDF a host sets esp_pthread_set_cfg here (stack, priority, core).
    // That setting stays on the calling thread: threads the host starts from
    // started, a Done or a post inherit the last one (Thread::l3) unless
    // they set their own.
    using ThreadSetup = std::function<void(Thread)>;

    // Runs l3. Blocks until stop(); returns 0, or 1 when the config does not
    // validate or apply fails. The calling thread becomes l3's callback
    // thread: it runs apply, then started, then every Done and every post
    // until stop. l3's reactor and the upstream reactor get threads of their
    // own.
    static int run(const Config& cfg,
                   std::function<void(const L3&)> started = {},
                   ThreadSetup thread_setup = {});

    // The host's reactors for run(), by bfc registry name. A named reactor
    // must be registered and running before run() and outlive it; l3 starts
    // no thread for it (thread_setup is not called for it) and, on stop,
    // removes its fds and timers and leaves it running. Empty: l3 runs that
    // reactor on a thread of its own, as run() above.
    struct Reactors
    {
        std::string io;        // bfc::default_reactor_registry (Thread::l3)
        std::string upstream;  // bfc::cv_reactor_registry (Thread::upstream)
    };
    // As run() above, on the host's reactors. Returns 1 also when a named
    // reactor is not registered.
    static int run(const Config& cfg, std::function<void(const L3&)> started,
                   ThreadSetup thread_setup, const Reactors& reactors);
    // Asks run() to return. Any thread, also before run() starts: a stop()
    // while no run() is active makes the next run() return at once, without
    // applying the config.
    static void stop();
    // stop() for a signal handler: writes atomics and the wake fd only.
    static void stop_from_signal();
    // The process's l3: valid any time; calls return ESRCH until run() has
    // started it and after it returns.
    static L3 instance();
    static Version version();  // this build (console `version`)

    L3() = default;

    bool running() const;

    UpstreamManager upstreams() const;
    RadioManager radios() const;
    // Config -> API: creates the radio, adds the upstreams, then adds the
    // radio. Validates first (Config::validate). Stops at the first error.
    Status apply(const Config& cfg) const;
    // No keys: all. Same keys and values as the console's get_metrics.
    Result<std::vector<Metric>> metrics(
        const std::vector<std::string>& keys = {}) const;
    // Runs fn(l3) on l3's callback thread, off the caller's thread. For a
    // thread that must not wait, such as a shared reactor. ESRCH: not
    // running.
    Status post(std::function<void(const L3&)> fn) const;

private:
    friend struct l3::detail::HandleAccess;
    std::shared_ptr<l3::detail::L3Core> core;
};

}  // namespace winject

#endif  // WINJECT_L3_PUBLIC_L3_H_
