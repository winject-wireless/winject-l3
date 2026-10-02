#ifndef WINJECT_MANAGER_CONSOLE_SERVICE_H_
#define WINJECT_MANAGER_CONSOLE_SERVICE_H_

#include "console/ManagerConsoleTypes.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <bfc/sized_buffer.hpp>
#include <stddef.h>
#include <string>
#include <vector>

namespace winject
{

// Local UDP console for runtime manager controls (not the radio console).
// Binds console_in for commands; always sends replies to console_out.
// Protocol: docs/mplane.md
class ConsoleService
{
public:
    ~ConsoleService();

    bool start(IOReactor& reactor, const sockaddr_in& console_in,
               const sockaddr_in& console_out, ManagerConsoleHandlers handlers,
               std::string* error);
    void stop();

private:
    static constexpr size_t k_line_max = 512;

    void on_datagram();
    void reply(const char* text);
    void reply_ok_args(const char* args);
    void reply_nok(const char* msg);
    void handle_line(const char* line);

    IOReactor* reactor = nullptr;
    bfc::socket sock;
    sockaddr_in out_addr{};
    ManagerConsoleHandlers handlers_;
    bfc::sized_buffer rx_buf;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_SERVICE_H_
