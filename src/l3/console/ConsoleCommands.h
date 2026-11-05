#ifndef WINJECT_L3_CONSOLE_CONSOLE_COMMANDS_H_
#define WINJECT_L3_CONSOLE_CONSOLE_COMMANDS_H_

#include "winject_l3/L3.h"

#include <chrono>
#include <functional>
#include <stddef.h>
#include <string>

namespace winject::core
{

// The manager m-plane text commands (docs/mplane.md) over the public API,
// with no transport: one line in, one reply text out ("OK ...",
// "NOK <errno>" or a listing). Uses only winject_l3 handles, never l3.
class ConsoleCommands
{
public:
    // Called once per line, possibly later for radio commands. Not called for
    // a line with no command.
    using Reply = std::function<void(const std::string& text)>;

    static constexpr size_t k_line_max = 512;

    // line: one command without the cmd:<u8> tag.
    // blocking false: upstream and metric calls block, radio calls use the
    // callback form, so reply may come later on l3's callback thread. Run
    // this on that thread (console::execute posts it there).
    // blocking true: every call blocks (radio calls for up to timeout), and
    // reply runs before this returns. Any thread.
    static void handle(const winject::L3& l3, const char* line, Reply reply,
                       bool blocking, std::chrono::milliseconds timeout);
};

}  // namespace winject::core

#endif  // WINJECT_L3_CONSOLE_CONSOLE_COMMANDS_H_
