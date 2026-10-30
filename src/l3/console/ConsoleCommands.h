#ifndef WINJECT_L3_CONSOLE_CONSOLE_COMMANDS_H_
#define WINJECT_L3_CONSOLE_CONSOLE_COMMANDS_H_

#include "ControlTypes.h"

#include <functional>
#include <stddef.h>
#include <string>

namespace winject
{

// The manager m-plane text commands (docs/mplane.md) over ControlOps, with no
// transport: one line in, one reply text out ("OK ...", "NOK <errno>" or a
// listing). ConsoleService serves it over UDP; winject_l3::Control::command
// lets a host console reuse it. Runs on the node's reactor thread.
class ConsoleCommands
{
public:
    // Called once per line, possibly later for radio forwards. Not called for
    // a line with no command.
    using Reply = std::function<void(const std::string& text)>;

    static constexpr size_t k_line_max = 512;

    ConsoleCommands() = default;
    explicit ConsoleCommands(ControlOps bound_ops);

    // All ops bound.
    bool valid() const;
    // line: one command without the cmd:<u8> tag.
    void handle(const char* line, Reply reply) const;

private:
    ControlOps ops;
};

}  // namespace winject

#endif  // WINJECT_L3_CONSOLE_CONSOLE_COMMANDS_H_
