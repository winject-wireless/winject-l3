// winject-manager: reads the config file, runs l3, serves the UDP
// console on its own reactor. Signals live here, not in the library.

#include "WinjectL3.h"
#include "utils/Log.h"
#include "winject_l3/Console.h"

#include <csignal>
#include <future>
#include <memory>
#include <signal.h>
#include <string.h>
#include <string>
#include <thread>

namespace
{

constexpr const char k_console_reactor[] = "winject-manager-console";

void on_stop_signal(int)
{
    winject::L3::stop_from_signal();
}

void install_handler(int signum, void (*handler)(int))
{
    struct sigaction sa = {};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(signum, &sa, nullptr);
}

}  // namespace

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "winject.conf";
    winject::Config cfg;
    std::string err;
    if (!cfg.load(path, &err))
    {
        LOG_ERR("%s", err.c_str());
        LOG_ERR("usage: winject-manager <config>");
        return 1;
    }
    install_handler(SIGPIPE, SIG_IGN);
    install_handler(SIGINT, on_stop_signal);
    install_handler(SIGTERM, on_stop_signal);

    // The console's reactor: this binary's to make, name and run.
    using winject::l3::console::Reactor;
    using winject::l3::console::ReactorRegistry;
    auto reactor = std::make_shared<Reactor>();
    ReactorRegistry::get_instance().add(k_console_reactor, reactor);
    // A stop() before run() set the reactor running would be lost: wait
    // until it runs.
    std::promise<void> running;
    auto is_running = running.get_future();
    reactor->wake_up(
        [&running]()
        {
            running.set_value();
        });
    std::thread console_thread(
        [reactor]()
        {
            reactor->run();
        });
    is_running.wait();

    winject::l3::console::Service console;
    bool console_failed = false;
    const int rc = winject::L3::run(
        cfg,
        [&](const winject::L3& l3)
        {
            if (cfg.console_in.empty())
            {
                return;
            }
            const winject::Status s =
                console.start(l3, cfg.console_in, k_console_reactor);
            if (!s.ok())
            {
                LOG_ERR("manager console %s: %s", cfg.console_in.c_str(),
                        s.error.c_str());
                console_failed = true;
                winject::L3::stop();
            }
        });
    console.stop();
    reactor->stop();
    console_thread.join();
    ReactorRegistry::get_instance().rem(k_console_reactor);
    return console_failed ? 1 : rc;
}
