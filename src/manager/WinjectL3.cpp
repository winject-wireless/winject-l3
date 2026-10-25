#include "WinjectL3.h"

#include "App.h"
#include "utils/Log.h"

#include <atomic>
#include <memory>
#include <mutex>

#if !defined(ESP_PLATFORM)
#include <signal.h>
#endif

namespace winject_l3
{

namespace
{

// g_app only changes under g_app_mutex, so stop() never touches an App that
// main() is destroying. g_stop_requested keeps a stop() that lands before
// main() has published the App; it is cleared when main() returns.
std::mutex g_app_mutex;
std::atomic<winject::App*> g_app{nullptr};
std::atomic<bool> g_stop_requested{false};

#if !defined(ESP_PLATFORM)
void on_signal(int)
{
    // A signal handler must not take the mutex. App::stop() only writes
    // atomics and a wake fd, and set_app() blocks these signals while g_app
    // changes.
    g_stop_requested.store(true);
    winject::App* app = g_app.load();
    if (app != nullptr)
    {
        app->stop();
    }
}

void install_handler(int signum, void (*handler)(int))
{
    struct sigaction sa = {};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(signum, &sa, nullptr);
}

void block_stop_signals(bool block)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, nullptr);
}
#endif

void set_app(winject::App* app)
{
#if !defined(ESP_PLATFORM)
    block_stop_signals(true);
#endif
    {
        std::lock_guard<std::mutex> lg(g_app_mutex);
        g_app.store(app);
    }
#if !defined(ESP_PLATFORM)
    block_stop_signals(false);
#endif
}

}  // namespace

void stop()
{
    std::lock_guard<std::mutex> lg(g_app_mutex);
    g_stop_requested.store(true);
    winject::App* app = g_app.load();
    if (app != nullptr)
    {
        app->stop();
    }
}

int main(const winject::Config& config)
{
#if !defined(ESP_PLATFORM)
    install_handler(SIGPIPE, SIG_IGN);
#endif
    // On the heap: App is ~13 KB, too big for an ESP-IDF task stack.
    auto instance = std::make_unique<winject::App>();
    set_app(instance.get());
    if (g_stop_requested.load())
    {
        instance->stop();
    }
#if !defined(ESP_PLATFORM)
    install_handler(SIGINT, on_signal);
    install_handler(SIGTERM, on_signal);
#endif
    int rc = 1;
    if (instance->load(config))
    {
        rc = instance->run();
    }
    set_app(nullptr);
    g_stop_requested.store(false);
    return rc;
}

}  // namespace winject_l3
