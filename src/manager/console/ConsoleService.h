#ifndef WINJECT_MANAGER_CONSOLE_SERVICE_H_
#define WINJECT_MANAGER_CONSOLE_SERVICE_H_

#include "console/ManagerConsoleTypes.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <bfc/sized_buffer.hpp>
#include <map>
#include <stddef.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace winject
{

// Local UDP console for runtime manager controls (not the radio console).
// Binds console_in for commands; replies to the sender of each request,
// tagged with the sender's id when the request had one.
// Protocol: docs/mplane.md
class ConsoleService
{
public:
    ~ConsoleService();

    bool start(IOReactor& reactor, const sockaddr_in& console_in,
               ManagerConsoleHandlers handlers, std::string* error);
    void stop();

private:
    static constexpr size_t k_line_max = 512;
    static constexpr size_t k_max_routes = 256;

    using PeerKey = uint64_t;
    static PeerKey peer_key(const sockaddr_in& peer);

    struct Route
    {
        sockaddr_in peer{};
        bool has_client_id = false;
        uint8_t client_id = 0;
        bool closed = false;
    };

    void on_datagram();
    void send_to(const sockaddr_in& to, const char* text);
    uint32_t open_route(const sockaddr_in& peer, bool has_id, uint8_t id);
    void respond(uint32_t l3_id, const std::string& text);
    void respond_ok_args(uint32_t l3_id, const char* args);
    void respond_nok(uint32_t l3_id, const char* msg);
    void handle_line(uint32_t l3_id, const char* line);

    IOReactor* reactor = nullptr;
    bfc::socket sock;
    ManagerConsoleHandlers handlers_;
    bfc::sized_buffer rx_buf;
    uint32_t next_l3_id_ = 1;
    std::unordered_map<uint32_t, Route> routes_;
    std::map<std::pair<PeerKey, uint8_t>, uint32_t> in_flight_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_SERVICE_H_
