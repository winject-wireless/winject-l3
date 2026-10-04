#include "console/ConsoleService.h"

#include "console/ConsoleParse.h"
#include "console/MplaneCorrelation.h"
#include "console/MplaneErrno.h"
#include "utils/Log.h"
#include "utils/Version.h"
#include "WinjectBuildVersion.h"

#include <chrono>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>

namespace winject
{

namespace
{

constexpr const char k_help_text[] =
    "help|?|h\n"
    "ping|p\n"
    "add_upstream|au id=<u8> txbus=<u8> rxbus=<u8> type=<UDP> "
    "rx=<address:port> tx=<interface:port> fec=<NONE|BLOCK> k=<n> n=<n> "
    "fec_timeout=<ms> quanta=<bytes>\n"
    "list_upstream|lu [ids=<u8>,...]\n"
    "update_upstream|uu id=<u8> [fec= k= n= fec_timeout= quanta=]\n"
    "remove_upstream|ru id=<u8>\n"
    "list_upstream_rx_stat|lur [ids=<u8>,...]\n"
    "list_upstream_tx_stat|lut [ids=<u8>,...]\n"
    "get_metrics|gm [keys=<string>,...]\n"
    "radio_info|ri\n"
    "radio_caps_info|rci\n"
    "radio_stats|rs\n"
    "radio_tx|rt [channel= tx_power= modulation= cca=<0|1>]\n"
    "reset|r\n"
    "config slot=<u8>\n"
    "radio_device|rd id=<u8> [mplane=<ip:port>] [dplane=<ip:port>] "
    "[fcs=AUTO|SIGNAL|ACTUAL]\n"
    "version|ver\n";

bool parse_optional_ids(char* save, std::vector<uint8_t>* ids, std::string* err)
{
    ids->clear();
    for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
         a = strtok_r(nullptr, " \t", &save))
    {
        const char* value = nullptr;
        if (!console_parse_kv(a, "ids=", &value))
        {
            if (err != nullptr)
            {
                *err = k_einval;
            }
            return false;
        }
        if (!console_parse_id_list(value, ids))
        {
            if (err != nullptr)
            {
                *err = k_einval;
            }
            return false;
        }
    }
    return true;
}

bool parse_optional_keys(char* save, std::vector<std::string>* keys,
                         std::string* err)
{
    keys->clear();
    for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
         a = strtok_r(nullptr, " \t", &save))
    {
        const char* value = nullptr;
        if (!console_parse_kv(a, "keys=", &value))
        {
            if (err != nullptr)
            {
                *err = k_einval;
            }
            return false;
        }
        if (!console_parse_string_list(value, keys))
        {
            if (err != nullptr)
            {
                *err = k_einval;
            }
            return false;
        }
    }
    return true;
}

bool parse_upstream_spec(char* save, ManagerUpstreamView* spec,
                         bool require_all, bool* got_id, std::string* err)
{
    bool have_id = false;
    bool have_txbus = false;
    bool have_rxbus = false;
    bool have_type = false;
    bool have_rx = false;
    bool have_tx = false;
    bool have_fec = false;
    bool have_k = false;
    bool have_n = false;
    bool have_timeout = false;
    bool have_quanta = false;

    for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
         a = strtok_r(nullptr, " \t", &save))
    {
        const char* value = nullptr;
        unsigned long v = 0;
        if (console_parse_kv(a, "id=", &value) &&
            console_parse_u8(value, &spec->id))
        {
            have_id = true;
            continue;
        }
        if (console_parse_kv(a, "txbus=", &value) &&
            console_parse_u8(value, &spec->bus_tx))
        {
            have_txbus = true;
            continue;
        }
        if (console_parse_kv(a, "rxbus=", &value) &&
            console_parse_u8(value, &spec->bus_rx))
        {
            have_rxbus = true;
            continue;
        }
        if (console_parse_kv(a, "type=", &value))
        {
            if (strcmp(value, "UDP") != 0)
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            spec->type = "UDP";
            have_type = true;
            continue;
        }
        if (console_parse_kv(a, "rx=", &value))
        {
            spec->rx = value;
            have_rx = true;
            continue;
        }
        if (console_parse_kv(a, "tx=", &value))
        {
            spec->tx = value;
            have_tx = true;
            continue;
        }
        if (console_parse_kv(a, "fec=", &value))
        {
            if (!console_parse_fec_type(value, &spec->fec))
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            have_fec = true;
            continue;
        }
        if (console_parse_kv(a, "k=", &value) && console_parse_u(value, &v))
        {
            spec->fec_k = static_cast<int>(v);
            have_k = true;
            continue;
        }
        if (console_parse_kv(a, "n=", &value) && console_parse_u(value, &v))
        {
            spec->fec_n = static_cast<int>(v);
            have_n = true;
            continue;
        }
        if (console_parse_kv(a, "fec_timeout=", &value))
        {
            int ms = 0;
            if (!console_parse_duration_ms(value, &ms))
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            spec->fec_timeout_ms = ms;
            have_timeout = true;
            continue;
        }
        if (console_parse_kv(a, "quanta=", &value) &&
            console_parse_u(value, &v))
        {
            if (v == 0)
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            spec->quanta = static_cast<size_t>(v);
            have_quanta = true;
            continue;
        }
        if (err != nullptr)
        {
            *err = k_einval;
        }
        return false;
    }

    if (got_id != nullptr)
    {
        *got_id = have_id;
    }
    if (!require_all)
    {
        return true;
    }
    if (!have_id || !have_txbus || !have_rxbus || !have_type || !have_rx ||
        !have_tx || !have_fec || !have_k || !have_n || !have_timeout ||
        !have_quanta)
    {
        if (err != nullptr)
        {
            *err = k_einval;
        }
        return false;
    }
    if (spec->fec == FecType::none)
    {
        spec->fec_k = 0;
        spec->fec_n = 0;
    }
    else if (spec->fec_k < 1 || spec->fec_n <= spec->fec_k || spec->fec_n > 255)
    {
        if (err != nullptr)
        {
            *err = k_einval;
        }
        return false;
    }
    return true;
}

void append_upstream_line(std::string* out, const ManagerUpstreamView& u)
{
    char fec_buf[32];
    console_format_fec_display(u.fec, u.fec_k, u.fec_n, fec_buf,
                               sizeof(fec_buf));
    char buf[384];
    snprintf(buf, sizeof(buf),
             "upstream id=%u txbus=%u rxbus=%u type=%s rx=%s tx=%s fec=%s "
             "fec_timeout=%dms quanta=%zu",
             static_cast<unsigned>(u.id), static_cast<unsigned>(u.bus_tx),
             static_cast<unsigned>(u.bus_rx), u.type.c_str(), u.rx.c_str(),
             u.tx.c_str(), fec_buf, u.fec_timeout_ms, u.quanta);
    if (!out->empty())
    {
        *out += '\n';
    }
    *out += buf;
}

uint64_t epoch_us()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch())
            .count());
}

}  // namespace

ConsoleService::~ConsoleService()
{
    stop();
}

bool ConsoleService::start(IOReactor& reactor, const sockaddr_in& console_in,
                           ManagerConsoleHandlers handlers, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (!handlers.add_upstream || !handlers.remove_upstream ||
        !handlers.list_upstream || !handlers.update_upstream ||
        !handlers.list_upstream_rx_stat || !handlers.list_upstream_tx_stat ||
        !handlers.get_metrics || !handlers.radio_info ||
        !handlers.radio_caps_info || !handlers.radio_stats ||
        !handlers.radio_tx || !handlers.radio_reset || !handlers.config_slot ||
        !handlers.radio_device)
    {
        return fail("invalid manager console args");
    }
    stop();
    this->reactor = &reactor;
    handlers_ = std::move(handlers);
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
    reactor = nullptr;
    handlers_ = {};
    routes_.clear();
    in_flight_.clear();
    next_l3_id_ = 1;
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
    uint32_t l3_id = next_l3_id_++;
    if (next_l3_id_ == 0)
    {
        next_l3_id_ = 1;
    }
    Route route;
    route.peer = peer;
    route.has_client_id = has_id;
    route.client_id = id;
    routes_[l3_id] = route;
    if (has_id)
    {
        in_flight_[{peer_key(peer), id}] = l3_id;
    }
    return l3_id;
}

void ConsoleService::respond(uint32_t l3_id, const std::string& text)
{
    const auto it = routes_.find(l3_id);
    if (it == routes_.end())
    {
        LOG_WRN("manager console: reply for closed or unknown l3_id %u", l3_id);
        return;
    }
    const Route route = it->second;
    routes_.erase(it);
    if (route.has_client_id)
    {
        in_flight_.erase({peer_key(route.peer), route.client_id});
    }
    std::string wire =
        route.has_client_id ? format_correlated_reply(route.client_id, text)
                            : text;
    if (!wire.empty() && wire.back() != '\n')
    {
        wire += '\n';
    }
    send_to(route.peer, wire.c_str());
}

void ConsoleService::respond_ok_args(uint32_t l3_id, const char* args)
{
    std::string text = "OK";
    if (args != nullptr && args[0] != '\0')
    {
        text += ' ';
        text += args;
    }
    respond(l3_id, text);
}

void ConsoleService::respond_nok(uint32_t l3_id, const char* msg)
{
    std::string text = "NOK ";
    text += msg != nullptr ? msg : "error";
    respond(l3_id, text);
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
    const bool tagged =
        parse_mplane_cmd(line, &client_id, &body, &malformed);
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
        const auto inflight = in_flight_.find({pk, client_id});
        if (inflight != in_flight_.end())
        {
            LOG_WRN("manager console: drop retransmit cmd:%u", client_id);
            return;
        }
    }
    if (routes_.size() >= k_max_routes)
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
    char copy[k_line_max];
    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char* save = nullptr;
    char* cmd = strtok_r(copy, " \t", &save);
    if (cmd == nullptr || *cmd == '\0')
    {
        return;
    }

    if (console_cmd_is(cmd, "help", "?", "h"))
    {
        respond(l3_id, k_help_text);
        return;
    }
    if (console_cmd_is(cmd, "ping", "p"))
    {
        respond(l3_id, "pong\n");
        return;
    }
    // Frozen across protocol versions: docs/mplane.md § Version discovery.
    if (console_cmd_is(cmd, "version", "ver"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        const WinjectVersion v = own_version();
        char buf[128];
        snprintf(buf, sizeof(buf), "version ver=%s proto=%u.%u",
                 WINJECT_VERSION_STRING, static_cast<unsigned>(v.x),
                 static_cast<unsigned>(v.y));
        respond_ok_args(l3_id, buf);
        return;
    }

    if (console_cmd_is(cmd, "radio_device", "rd"))
    {
        ManagerRadioDeviceUpdate patch;
        std::string err;
        if (!console_parse_radio_device_args(save, &patch, &err))
        {
            respond_nok(l3_id, err.c_str());
            return;
        }
        ManagerRadioDeviceView view;
        if (!handlers_.radio_device(patch, &view, &err))
        {
            respond_nok(l3_id, err.c_str());
            return;
        }
        respond_ok_args(l3_id, console_format_radio_device(view).c_str());
        return;
    }

    if (console_cmd_is(cmd, "add_upstream", "au"))
    {
        ManagerUpstreamView spec;
        std::string err;
        if (!parse_upstream_spec(save, &spec, true, nullptr, &err))
        {
            respond_nok(l3_id, err.c_str());
            return;
        }
        if (!handlers_.add_upstream(spec, &err))
        {
            respond_nok(l3_id, err.empty() ? "error" : err.c_str());
            return;
        }
        std::string args;
        append_upstream_line(&args, spec);
        respond_ok_args(l3_id, args.c_str());
        return;
    }

    if (console_cmd_is(cmd, "list_upstream", "lu"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            respond_nok(l3_id, err.c_str());
            return;
        }
        std::vector<ManagerUpstreamView> rows;
        if (!handlers_.list_upstream(ids, &rows, &err))
        {
            respond_nok(l3_id, err.empty() ? "error" : err.c_str());
            return;
        }
        std::string body;
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "N=%zu", rows.size());
        body = hdr;
        for (const auto& u : rows)
        {
            append_upstream_line(&body, u);
        }
        body += '\n';
        respond(l3_id, body.c_str());
        return;
    }

    if (console_cmd_is(cmd, "update_upstream", "uu"))
    {
        ManagerUpstreamUpdate patch;
        std::string err;
        bool got_id = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            unsigned long v = 0;
            if (console_parse_kv(a, "id=", &value) &&
                console_parse_u8(value, &patch.id))
            {
                got_id = true;
                continue;
            }
            if (console_parse_kv(a, "fec=", &value))
            {
                if (!console_parse_fec_type(value, &patch.fec))
                {
                    respond_nok(l3_id, k_einval);
                    return;
                }
                patch.have_fec = true;
                continue;
            }
            if (console_parse_kv(a, "k=", &value) && console_parse_u(value, &v))
            {
                patch.fec_k = static_cast<int>(v);
                patch.have_k = true;
                continue;
            }
            if (console_parse_kv(a, "n=", &value) && console_parse_u(value, &v))
            {
                patch.fec_n = static_cast<int>(v);
                patch.have_n = true;
                continue;
            }
            if (console_parse_kv(a, "fec_timeout=", &value))
            {
                int ms = 0;
                if (!console_parse_duration_ms(value, &ms))
                {
                    respond_nok(l3_id, k_einval);
                    return;
                }
                patch.fec_timeout_ms = ms;
                patch.have_fec_timeout = true;
                continue;
            }
            if (console_parse_kv(a, "quanta=", &value) &&
                console_parse_u(value, &v))
            {
                if (v == 0)
                {
                    respond_nok(l3_id, k_einval);
                    return;
                }
                patch.quanta = static_cast<size_t>(v);
                patch.have_quanta = true;
                continue;
            }
            respond_nok(l3_id, k_einval);
            return;
        }
        if (!got_id)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        ManagerUpstreamView out;
        if (!handlers_.update_upstream(patch, &out, &err))
        {
            respond_nok(l3_id, err.empty() ? "error" : err.c_str());
            return;
        }
        std::string args;
        append_upstream_line(&args, out);
        respond_ok_args(l3_id, args.c_str());
        return;
    }

    if (console_cmd_is(cmd, "remove_upstream", "ru"))
    {
        std::string err;
        const char* value = nullptr;
        char* arg = strtok_r(nullptr, " \t", &save);
        uint8_t id = 0;
        if (arg == nullptr || !console_parse_kv(arg, "id=", &value) ||
            !console_parse_u8(value, &id) ||
            strtok_r(nullptr, " \t", &save) != nullptr)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        if (!handlers_.remove_upstream(id, &err))
        {
            respond_nok(l3_id, err.empty() ? "error" : err.c_str());
            return;
        }
        respond(l3_id, "OK\n");
        return;
    }

    if (console_cmd_is(cmd, "list_upstream_rx_stat", "lur"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            respond_nok(l3_id, err.c_str());
            return;
        }
        std::vector<ManagerUpstreamRxStatView> rows;
        if (!handlers_.list_upstream_rx_stat(ids, &rows, &err))
        {
            respond_nok(l3_id, err.empty() ? "error" : err.c_str());
            return;
        }
        std::string body;
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "N=%zu T=%llu", rows.size(),
                 static_cast<unsigned long long>(epoch_us()));
        body = hdr;
        for (const auto& s : rows)
        {
            char fec_buf[32];
            console_format_fec_display(s.fec, s.fec_k, s.fec_n, fec_buf,
                                       sizeof(fec_buf));
            char line[320];
            snprintf(line, sizeof(line),
                     "upstream_rx_stat id=%u rxbyt=%llu rxpkt=%llu "
                     "rx_oversize=%llu rxgap=%llu "
                     "fec=%s fec_rec=%llu fec_lost=%llu fec_rxbyt=%llu "
                     "fec_rxpkt=%llu fec_rxgap=%llu",
                     static_cast<unsigned>(s.id),
                     static_cast<unsigned long long>(s.rxbyt),
                     static_cast<unsigned long long>(s.rxpkt),
                     static_cast<unsigned long long>(s.rx_oversize),
                     static_cast<unsigned long long>(s.rxgap), fec_buf,
                     static_cast<unsigned long long>(s.fec_rec),
                     static_cast<unsigned long long>(s.fec_lost),
                     static_cast<unsigned long long>(s.fec_rxbyt),
                     static_cast<unsigned long long>(s.fec_rxpkt),
                     static_cast<unsigned long long>(s.fec_rxgap));
            body += '\n';
            body += line;
        }
        body += '\n';
        respond(l3_id, body.c_str());
        return;
    }

    if (console_cmd_is(cmd, "list_upstream_tx_stat", "lut"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            respond_nok(l3_id, err.c_str());
            return;
        }
        std::vector<ManagerUpstreamTxStatView> rows;
        if (!handlers_.list_upstream_tx_stat(ids, &rows, &err))
        {
            respond_nok(l3_id, err.empty() ? "error" : err.c_str());
            return;
        }
        std::string body;
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "N=%zu T=%llu", rows.size(),
                 static_cast<unsigned long long>(epoch_us()));
        body = hdr;
        for (const auto& s : rows)
        {
            char fec_buf[32];
            console_format_fec_display(s.fec, s.fec_k, s.fec_n, fec_buf,
                                       sizeof(fec_buf));
            char line[320];
            snprintf(line, sizeof(line),
                     "upstream_tx_stat id=%u txbyt=%llu txpkt=%llu fec=%s "
                     "fec_txbyt=%llu fec_txpkt=%llu tx_pending_byt=%llu "
                     "tx_pending_pkt=%llu",
                     static_cast<unsigned>(s.id),
                     static_cast<unsigned long long>(s.txbyt),
                     static_cast<unsigned long long>(s.txpkt), fec_buf,
                     static_cast<unsigned long long>(s.fec_txbyt),
                     static_cast<unsigned long long>(s.fec_txpkt),
                     static_cast<unsigned long long>(s.tx_pending_byt),
                     static_cast<unsigned long long>(s.tx_pending_pkt));
            body += '\n';
            body += line;
        }
        body += '\n';
        respond(l3_id, body.c_str());
        return;
    }

    if (console_cmd_is(cmd, "get_metrics", "gm"))
    {
        std::vector<std::string> keys;
        std::string err;
        if (!parse_optional_keys(save, &keys, &err))
        {
            respond_nok(l3_id, err.c_str());
            return;
        }
        std::vector<ManagerMetricView> rows;
        if (!handlers_.get_metrics(keys, &rows, &err))
        {
            respond_nok(l3_id, err.empty() ? "error" : err.c_str());
            return;
        }
        std::string body;
        const size_t count = rows.size();
        const unsigned n_field =
            count > 65535u ? 65535u : static_cast<unsigned>(count);
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "N=%u T=%llu", n_field,
                 static_cast<unsigned long long>(epoch_us()));
        body = hdr;
        for (const auto& row : rows)
        {
            body += '\n';
            body += row.key;
            body += '=';
            body += row.value;
        }
        body += '\n';
        respond(l3_id, body.c_str());
        return;
    }

    if (console_cmd_is(cmd, "radio_info", "ri"))
    {
        ManagerConsoleReply cr;
        cr.send_text = [this, l3_id](const std::string& text)
        {
            std::string out = text;
            if (!out.empty() && out.back() != '\n')
            {
                out += '\n';
            }
            respond(l3_id, out);
        };
        cr.send_nok = [this, l3_id](const char* msg)
        {
            respond_nok(l3_id, msg);
        };
        handlers_.radio_info(cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_caps_info", "rci"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this, l3_id](const std::string& text)
        {
            respond_ok_args(l3_id, text.c_str());
        };
        cr.send_nok = [this, l3_id](const char* msg)
        {
            respond_nok(l3_id, msg);
        };
        handlers_.radio_caps_info(cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_stats", "rs"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this, l3_id](const std::string& text)
        {
            std::string out = text;
            if (!out.empty() && out.back() != '\n')
            {
                out += '\n';
            }
            respond(l3_id, out);
        };
        cr.send_nok = [this, l3_id](const char* msg)
        {
            respond_nok(l3_id, msg);
        };
        handlers_.radio_stats(cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_tx", "rt"))
    {
        ManagerRadioUpdate patch;
        std::string err;
        bool any = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            unsigned long v = 0;
            if (console_parse_kv(a, "channel=", &value) &&
                console_parse_u(value, &v))
            {
                patch.channel = static_cast<uint16_t>(v);
                patch.have_channel = true;
                any = true;
                continue;
            }
            if (console_parse_kv(a, "tx_power=", &value) &&
                console_parse_u(value, &v))
            {
                patch.tx_power = static_cast<int>(v);
                patch.have_tx_power = true;
                any = true;
                continue;
            }
            if (console_parse_kv(a, "modulation=", &value))
            {
                patch.modulation = value;
                patch.have_modulation = true;
                any = true;
                continue;
            }
            if (console_parse_kv(a, "cca=", &value) &&
                console_parse_u(value, &v) && v <= 1)
            {
                patch.cca = v == 1;
                patch.have_cca = true;
                any = true;
                continue;
            }
            respond_nok(l3_id, k_einval);
            return;
        }
        if (!any)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this, l3_id](const std::string& text)
        {
            respond(l3_id, text);
        };
        cr.send_nok = [this, l3_id](const char* msg)
        {
            respond_nok(l3_id, msg);
        };
        handlers_.radio_tx(patch, cr);
        return;
    }

    if (console_cmd_is(cmd, "reset", "r"))
    {
        if (strtok_r(nullptr, " 	", &save) != nullptr)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this, l3_id](const std::string& text)
        {
            if (!text.empty())
            {
                respond(l3_id, text);
                return;
            }
            respond_ok_args(l3_id, nullptr);
        };
        cr.send_nok = [this, l3_id](const char* msg)
        {
            respond_nok(l3_id, msg);
        };
        handlers_.radio_reset(cr);
        return;
    }

    if (strcmp(cmd, "config") == 0)
    {
        const char* value = nullptr;
        unsigned long slot = 0;
        bool have_slot = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            if (console_parse_kv(a, "slot=", &value) &&
                console_parse_u(value, &slot))
            {
                have_slot = true;
                continue;
            }
            respond_nok(l3_id, k_einval);
            return;
        }
        if (!have_slot || slot > 255)
        {
            respond_nok(l3_id, k_einval);
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this, l3_id, slot](const std::string& text)
        {
            if (!text.empty())
            {
                respond(l3_id, text);
                return;
            }
            char args[32];
            snprintf(args, sizeof(args), "slot=%lu", slot);
            respond_ok_args(l3_id, args);
        };
        cr.send_nok = [this, l3_id](const char* msg)
        {
            respond_nok(l3_id, msg);
        };
        handlers_.config_slot(static_cast<uint8_t>(slot), cr);
        return;
    }

    respond_nok(l3_id, k_enosys);
}

}  // namespace winject
