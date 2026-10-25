#include "WinjectL3.h"
#include "utils/Log.h"

#include <string>

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
    return winject_l3::main(cfg);
}
