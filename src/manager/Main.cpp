#include "App.h"
#include "utils/Log.h"

#include <signal.h>

static winject::App* g_app = nullptr;

static void on_signal(int)
{
    if (g_app != nullptr)
    {
        g_app->stop();
    }
}

static void install_handler(int signum, void (*handler)(int))
{
    struct sigaction sa = {};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(signum, &sa, nullptr);
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "winject.conf";
    install_handler(SIGPIPE, SIG_IGN);
    winject::App instance;
    g_app = &instance;
    install_handler(SIGINT, on_signal);
    install_handler(SIGTERM, on_signal);
    if (!instance.load(path))
    {
        LOG_ERR("usage: winject-manager <config>");
        return 1;
    }
    return instance.run();
}
