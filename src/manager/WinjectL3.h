#ifndef WINJECT_MANAGER_WINJECT_L3_H_
#define WINJECT_MANAGER_WINJECT_L3_H_

#include "Config.h"

namespace winject_l3
{

// Runs the manager with an already built config. Blocks until stop() and
// returns the process exit code. On Linux it also handles SIGINT / SIGTERM
// (stop) and ignores SIGPIPE. The Linux binary parses argv into a Config
// before calling this; an ESP-IDF host fills the struct itself.
int main(const winject::Config& config);

// Asks a running main() to return. Safe from any thread.
void stop();

}  // namespace winject_l3

#endif  // WINJECT_MANAGER_WINJECT_L3_H_
