#include "console/ConsoleService.h"

#include "console/ConsoleParse.h"
#include "utils/Log.h"

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
    "radio_tx|rt [channel= tx_power= modulation=]\n"
    "reset|r id=<u8>\n"
    "config slot=<u8>\n";

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
                *err = "INVALID_ARGUMENT";
            }
            return false;
        }
        if (!console_parse_id_list(value, ids))
        {
            if (err != nullptr)
            {
                *err = "INVALID_ARGUMENT";
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
                *err = "INVALID_ARGUMENT";
            }
            return false;
        }
        if (!console_parse_string_list(value, keys))
        {
            if (err != nullptr)
            {
                *err = "INVALID_ARGUMENT";
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
                    *err = "INVALID_ARGUMENT";
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
                    *err = "INVALID_ARGUMENT";
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
                    *err = "INVALID_ARGUMENT";
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
                    *err = "INVALID_ARGUMENT";
                }
                return false;
            }
            spec->quanta = static_cast<size_t>(v);
            have_quanta = true;
            continue;
        }
        if (err != nullptr)
        {
            *err = "INVALID_ARGUMENT";
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
            *err = "INVALID_ARGUMENT";
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
            *err = "INVALID_ARGUMENT";
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
                           const sockaddr_in& console_out,
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
        !handlers.radio_caps_info || !handlers.radio_tx ||
        !handlers.radio_reset || !handlers.config_slot)
    {
        return fail("invalid manager console args");
    }
    stop();
    this->reactor = &reactor;
    handlers_ = std::move(handlers);
    out_addr = console_out;
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
    sockaddr_in out_log = console_out;
    LOG_INF(
        "manager console in=%s out=%s",
        bfc::sockaddr_to_string(reinterpret_cast<sockaddr*>(&in_log)).c_str(),
        bfc::sockaddr_to_string(reinterpret_cast<sockaddr*>(&out_log)).c_str());
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
    out_addr = {};
}

void ConsoleService::reply(const char* text)
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
    sock.send(view, 0, reinterpret_cast<const sockaddr*>(&out_addr),
              sizeof(out_addr));
}

void ConsoleService::reply_ok_args(const char* args)
{
    std::string text = "OK";
    if (args != nullptr && args[0] != '\0')
    {
        text += ' ';
        text += args;
    }
    if (text.back() != '\n')
    {
        text += '\n';
    }
    reply(text.c_str());
}

void ConsoleService::reply_nok(const char* msg)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "NOK %s\n", msg != nullptr ? msg : "error");
    reply(buf);
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
    if (n >= static_cast<ssize_t>(k_line_max) - 1)
    {
        reply_nok("INVALID_ARGUMENT");
        return;
    }
    char* line = reinterpret_cast<char*>(rx_buf.data());
    line[n] = '\0';
    console_trim_line(line);
    if (line[0] == '\0' || line[0] == '#')
    {
        return;
    }
    handle_line(line);
}

void ConsoleService::handle_line(const char* line)
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
        reply(k_help_text);
        return;
    }
    if (console_cmd_is(cmd, "ping", "p"))
    {
        reply("pong\n");
        return;
    }

    if (console_cmd_is(cmd, "add_upstream", "au"))
    {
        ManagerUpstreamView spec;
        std::string err;
        if (!parse_upstream_spec(save, &spec, true, nullptr, &err))
        {
            reply_nok(err.c_str());
            return;
        }
        if (!handlers_.add_upstream(spec, &err))
        {
            reply_nok(err.empty() ? "error" : err.c_str());
            return;
        }
        std::string args;
        append_upstream_line(&args, spec);
        reply_ok_args(args.c_str());
        return;
    }

    if (console_cmd_is(cmd, "list_upstream", "lu"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            reply_nok(err.c_str());
            return;
        }
        std::vector<ManagerUpstreamView> rows;
        if (!handlers_.list_upstream(ids, &rows, &err))
        {
            reply_nok(err.empty() ? "error" : err.c_str());
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
        reply(body.c_str());
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
                    reply_nok("INVALID_ARGUMENT");
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
                    reply_nok("INVALID_ARGUMENT");
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
                    reply_nok("INVALID_ARGUMENT");
                    return;
                }
                patch.quanta = static_cast<size_t>(v);
                patch.have_quanta = true;
                continue;
            }
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        if (!got_id)
        {
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        ManagerUpstreamView out;
        if (!handlers_.update_upstream(patch, &out, &err))
        {
            reply_nok(err.empty() ? "error" : err.c_str());
            return;
        }
        std::string args;
        append_upstream_line(&args, out);
        reply_ok_args(args.c_str());
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
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        if (!handlers_.remove_upstream(id, &err))
        {
            reply_nok(err.empty() ? "error" : err.c_str());
            return;
        }
        reply("OK\n");
        return;
    }

    if (console_cmd_is(cmd, "list_upstream_rx_stat", "lur"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            reply_nok(err.c_str());
            return;
        }
        std::vector<ManagerUpstreamRxStatView> rows;
        if (!handlers_.list_upstream_rx_stat(ids, &rows, &err))
        {
            reply_nok(err.empty() ? "error" : err.c_str());
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
        reply(body.c_str());
        return;
    }

    if (console_cmd_is(cmd, "list_upstream_tx_stat", "lut"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            reply_nok(err.c_str());
            return;
        }
        std::vector<ManagerUpstreamTxStatView> rows;
        if (!handlers_.list_upstream_tx_stat(ids, &rows, &err))
        {
            reply_nok(err.empty() ? "error" : err.c_str());
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
        reply(body.c_str());
        return;
    }

    if (console_cmd_is(cmd, "get_metrics", "gm"))
    {
        std::vector<std::string> keys;
        std::string err;
        if (!parse_optional_keys(save, &keys, &err))
        {
            reply_nok(err.c_str());
            return;
        }
        std::vector<ManagerMetricView> rows;
        if (!handlers_.get_metrics(keys, &rows, &err))
        {
            reply_nok(err.empty() ? "error" : err.c_str());
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
        reply(body.c_str());
        return;
    }

    if (console_cmd_is(cmd, "radio_info", "ri"))
    {
        ManagerConsoleReply cr;
        cr.send_text = [this](const std::string& text)
        {
            std::string out = text;
            if (!out.empty() && out.back() != '\n')
            {
                out += '\n';
            }
            reply(out.c_str());
        };
        cr.send_nok = [this](const char* msg)
        {
            reply_nok(msg);
        };
        handlers_.radio_info(cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_caps_info", "rci"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this](const std::string& text)
        {
            reply_ok_args(text.c_str());
        };
        cr.send_nok = [this](const char* msg)
        {
            reply_nok(msg);
        };
        handlers_.radio_caps_info(cr);
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
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        if (!any)
        {
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this](const std::string& text)
        {
            reply(text.c_str());
        };
        cr.send_nok = [this](const char* msg)
        {
            reply_nok(msg);
        };
        handlers_.radio_tx(patch, cr);
        return;
    }

    if (console_cmd_is(cmd, "reset", "r"))
    {
        const char* value = nullptr;
        unsigned long id = 0;
        bool have_id = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            if (console_parse_kv(a, "id=", &value) &&
                console_parse_u(value, &id))
            {
                have_id = true;
                continue;
            }
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        if (!have_id || id > 255)
        {
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this, id](const std::string& text)
        {
            if (!text.empty())
            {
                reply(text.c_str());
                return;
            }
            char args[32];
            snprintf(args, sizeof(args), "id=%lu", id);
            reply_ok_args(args);
        };
        cr.send_nok = [this](const char* msg)
        {
            reply_nok(msg);
        };
        handlers_.radio_reset(static_cast<uint8_t>(id), cr);
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
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        if (!have_slot || slot > 255)
        {
            reply_nok("INVALID_ARGUMENT");
            return;
        }
        ManagerConsoleReply cr;
        cr.send_text = [this, slot](const std::string& text)
        {
            if (!text.empty())
            {
                reply(text.c_str());
                return;
            }
            char args[32];
            snprintf(args, sizeof(args), "slot=%lu", slot);
            reply_ok_args(args);
        };
        cr.send_nok = [this](const char* msg)
        {
            reply_nok(msg);
        };
        handlers_.config_slot(static_cast<uint8_t>(slot), cr);
        return;
    }

    reply_nok("unknown command, type help");
}

}  // namespace winject
