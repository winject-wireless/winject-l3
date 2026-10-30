#include "console/ConsoleService.h"

#include "console/ConsoleParse.h"
#include "mplane/MplaneCorrelation.h"
#include "mplane/MplaneErrno.h"
#include "utils/Log.h"

#include <chrono>
#include <cinttypes>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>

namespace winject
{

ConsoleService::~ConsoleService()
{
    stop();
}

bool ConsoleService::start(IOReactor& reactor, const sockaddr_in& console_in,
                           ControlOps bound_ops, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    ConsoleCommands bound(std::move(bound_ops));
    if (!bound.valid())
    {
        return fail("invalid manager console args");
    }
    stop();
    this->reactor = &reactor;
    this->commands = std::move(bound);
    sock = bfc::socket(bfc::create_udp4());
    if (sock.fd() < 0)
    {
        return fail(strerror(errno));
    }
    const int one = 1;
    sock.set_sock_opt(SOL_SOCKET, SO_REUSEADDR, one);
    if (sock.bind(console_in) < 0)
    {
        const char* why = strerror(errno);
        stop();
        return fail(why);
    }
    if (!reactor.add_read_rdy(sock.fd(),
                              [this]()
                              {
                                  on_datagram();
                              }))
    {
        stop();
        return fail("epoll add failed");
    }
    rx_buf.reserve(k_line_max);
    sockaddr_in in_log = console_in;
    LOG_INF(
        "manager console in=%s",
        bfc::sockaddr_to_string(reinterpret_cast<sockaddr*>(&in_log)).c_str());
    return true;
}

void ConsoleService::stop()
{
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        bfc::socket(std::move(sock));
    }
    this->reactor = nullptr;
    this->commands = {};
    routes.clear();
    in_flight.clear();
    next_l3_id = 1;
}

ConsoleService::PeerKey ConsoleService::peer_key(const sockaddr_in& peer)
{
    return (static_cast<uint64_t>(peer.sin_addr.s_addr) << 16) |
           static_cast<uint64_t>(peer.sin_port);
}

void ConsoleService::send_to(const sockaddr_in& to, const char* text)
{
    if (sock.fd() < 0 || text == nullptr)
    {
        return;
    }
    const size_t n = strlen(text);
    if (n == 0)
    {
        return;
    }
    const bfc::const_buffer_view view(reinterpret_cast<const std::byte*>(text),
                                      n);
    sock.send(view, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

uint32_t ConsoleService::open_route(const sockaddr_in& peer, bool has_id,
                                    uint8_t id)
{
    uint32_t l3_id = next_l3_id++;
    if (next_l3_id == 0)
    {
        next_l3_id = 1;
    }
    Route route;
    route.peer = peer;
    route.has_client_id = has_id;
    route.client_id = id;
    routes[l3_id] = route;
    if (has_id)
    {
        in_flight[{peer_key(peer), id}] = l3_id;
    }
    return l3_id;
}

void ConsoleService::respond(uint32_t l3_id, const std::string& text)
{
    const auto it = routes.find(l3_id);
    if (it == routes.end())
    {
        LOG_WRN("manager console: reply for closed or unknown l3_id %" PRIu32,
                l3_id);
        return;
    }
    const Route route = it->second;
    routes.erase(it);
    if (route.has_client_id)
    {
        in_flight.erase({peer_key(route.peer), route.client_id});
    }
    std::string wire = route.has_client_id
                           ? format_correlated_reply(route.client_id, text)
                           : text;
    if (!wire.empty() && wire.back() != '\n')
    {
        wire += '\n';
    }
    send_to(route.peer, wire.c_str());
}

void ConsoleService::on_datagram()
{
    if (sock.fd() < 0)
    {
        return;
    }
    sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    if (rx_buf.capacity() < k_line_max)
    {
        rx_buf.reserve(k_line_max);
    }
    rx_buf.resize(k_line_max);
    const ssize_t n =
        sock.recv(rx_buf, 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n >= 0)
    {
        rx_buf.resize(static_cast<size_t>(n));
    }
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return;
        }
        LOG_WRN("manager console recv: %s", strerror(errno));
        return;
    }
    if (n == 0)
    {
        return;
    }
    if (peer.sin_family != AF_INET ||
        peer_len < static_cast<socklen_t>(sizeof(sockaddr_in)))
    {
        LOG_WRN("manager console: ignored non-IPv4 peer");
        return;
    }
    if (n >= static_cast<ssize_t>(k_line_max) - 1)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "NOK %s\n", k_einval);
        send_to(peer, buf);
        return;
    }
    char* line = reinterpret_cast<char*>(rx_buf.data());
    line[n] = '\0';
    console_trim_line(line);
    if (line[0] == '\0' || line[0] == '#')
    {
        return;
    }

    uint8_t client_id = 0;
    const char* body = line;
    bool malformed = false;
    const bool tagged = parse_mplane_cmd(line, &client_id, &body, &malformed);
    if (tagged && malformed)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "NOK %s\n", k_einval);
        send_to(peer, buf);
        return;
    }
    if (tagged)
    {
        const PeerKey pk = peer_key(peer);
        const auto inflight = in_flight.find({pk, client_id});
        if (inflight != in_flight.end())
        {
            LOG_WRN("manager console: drop retransmit cmd:%u", client_id);
            return;
        }
    }
    if (routes.size() >= k_max_routes)
    {
        std::string nok = std::string("NOK ") + k_ebusy;
        if (tagged)
        {
            nok = format_correlated_reply(client_id, nok);
        }
        if (nok.back() != '\n')
        {
            nok += '\n';
        }
        send_to(peer, nok.c_str());
        return;
    }
    const uint32_t l3_id = open_route(peer, tagged, client_id);
    handle_line(l3_id, body);
}

void ConsoleService::handle_line(uint32_t l3_id, const char* line)
{
    commands.handle(line,
                    [this, l3_id](const std::string& text)
                    {
                        respond(l3_id, text);
                    });
}

}  // namespace winject
