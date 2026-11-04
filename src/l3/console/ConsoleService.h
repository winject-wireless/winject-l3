#ifndef WINJECT_MANAGER_CONSOLE_SERVICE_H_
#define WINJECT_MANAGER_CONSOLE_SERVICE_H_

#include "console/ConsoleCommands.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"
#include "winject_l3/L3.h"

#include <bfc/sized_buffer.hpp>
#include <map>
#include <memory>
#include <stddef.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace winject::core
{

// Local UDP console for runtime manager controls (not the radio console).
// Binds console_in for commands; replies to the sender of each request,
// tagged with the sender's id when the request had one. Each line goes to
// winject::l3::console::execute; replies come back on l3's callback thread and
// are posted to this reactor, so the socket stays on one thread. Runs on, and
// must be started and stopped on, its reactor's thread.
// Protocol: docs/mplane.md
class ConsoleService
{
public:
    ~ConsoleService();

    bool start(const std::shared_ptr<IOReactor>& reactor,
               const sockaddr_in& console_in, winject::L3 l3,
               std::string* error);
    void stop();

private:
    static constexpr size_t k_line_max = ConsoleCommands::k_line_max;
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
    void handle_line(uint32_t l3_id, const char* line);

    std::shared_ptr<IOReactor> reactor;
    bfc::socket sock;
    winject::L3 l3;
    // Replies hold a weak copy: one that lands after stop() is dropped.
    std::shared_ptr<ConsoleService*> alive;
    bfc::sized_buffer rx_buf;
    uint32_t next_l3_id = 1;
    std::unordered_map<uint32_t, Route> routes;
    std::map<std::pair<PeerKey, uint8_t>, uint32_t> in_flight;
};

}  // namespace winject::core

#endif  // WINJECT_MANAGER_CONSOLE_SERVICE_H_
