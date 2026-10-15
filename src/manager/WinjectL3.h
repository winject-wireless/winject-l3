#ifndef WINJECT_MANAGER_WINJECT_L3_H_
#define WINJECT_MANAGER_WINJECT_L3_H_

namespace winject_l3
{

// Runs winject-manager: argv[1] is the config path (default winject.conf).
// Blocks until stop() and returns the process exit code. On Linux it also
// handles SIGINT / SIGTERM (stop) and ignores SIGPIPE.
int main(int argc, char* argv[]);

// Asks a running main() to return. Safe from any thread.
void stop();

}  // namespace winject_l3

#endif  // WINJECT_MANAGER_WINJECT_L3_H_
