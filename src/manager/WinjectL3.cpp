#include "WinjectL3.h"

#include "App.h"
#include "utils/Log.h"

#include <atomic>

#if !defined(ESP_PLATFORM)
#include <signal.h>
#endif

namespace winject_l3
{

namespace
{

std::atomic<winject::App*> g_app{nullptr};

#if !defined(ESP_PLATFORM)
void on_signal(int)
{
    stop();
}

void install_handler(int signum, void (*handler)(int))
{
    struct sigaction sa = {};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(signum, &sa, nullptr);
}
#endif

}  // namespace

void stop()
{
    winject::App* app = g_app.load();
    if (app != nullptr)
    {
        app->stop();
    }
}

int main(int argc, char* argv[])
{
    const char* path = argc > 1 ? argv[1] : "winject.conf";
#if !defined(ESP_PLATFORM)
    install_handler(SIGPIPE, SIG_IGN);
#endif
    winject::App instance;
    g_app.store(&instance);
#if !defined(ESP_PLATFORM)
    install_handler(SIGINT, on_signal);
    install_handler(SIGTERM, on_signal);
#endif
    int rc = 1;
    if (!instance.load(path))
    {
        LOG_ERR("usage: winject-manager <config>");
    }
    else
    {
        rc = instance.run();
    }
    g_app.store(nullptr);
    return rc;
}

}  // namespace winject_l3
