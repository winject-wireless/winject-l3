#include "console/ConsoleService.h"

#include "console/ConsoleParse.h"
#include "utils/Log.h"

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
                           const sockaddr_in& console_out, set_fec_fn set_fec,
                           set_budget_fn set_budget, get_fec_fn get_fec,
                           get_budget_fn get_budget, get_ci_fn get_ci,
                           set_modulation_fn set_modulation,
                           get_modulation_fn get_modulation,
                           set_tx_pacing_fn set_tx_pacing,
                           get_tx_pacing_fn get_tx_pacing, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (!set_fec || !set_budget || !get_fec || !get_budget || !get_ci ||
        !set_modulation || !get_modulation || !set_tx_pacing || !get_tx_pacing)
    {
        return fail("invalid manager console args");
    }
    stop();
    this->reactor = &reactor;
    this->set_fec = std::move(set_fec);
    this->set_budget = std::move(set_budget);
    this->get_fec = std::move(get_fec);
    this->get_budget = std::move(get_budget);
    this->get_ci = std::move(get_ci);
    this->set_modulation = std::move(set_modulation);
    this->get_modulation = std::move(get_modulation);
    this->set_tx_pacing = std::move(set_tx_pacing);
    this->get_tx_pacing = std::move(get_tx_pacing);
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
    set_fec = nullptr;
    set_budget = nullptr;
    get_fec = nullptr;
    get_budget = nullptr;
    get_ci = nullptr;
    set_modulation = nullptr;
    get_modulation = nullptr;
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

void ConsoleService::reply_ok()
{
    reply("ok\n");
}

void ConsoleService::reply_ok_args(const char* args)
{
    std::string text = "ok";
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
    snprintf(buf, sizeof(buf), "nok %s\n", msg != nullptr ? msg : "error");
    reply(buf);
}

void ConsoleService::on_datagram()
{
    if (sock.fd() < 0)
    {
        return;
    }
    while (true)
    {
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        if (rx_buf.capacity() < k_line_max - 1)
        {
            rx_buf.reserve(k_line_max - 1);
        }
        rx_buf.resize(k_line_max - 1);
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
            continue;
        }
        char* line = reinterpret_cast<char*>(rx_buf.data());
        line[n] = '\0';
        console_trim_line(line);
        if (line[0] == '\0' || line[0] == '#')
        {
            continue;
        }
        handle_line(line);
    }
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

    if (console_cmd_is(cmd, "help", "?"))
    {
        reply(
            "set_upstream_fec|suf <index> <NONE|RS_BLOCK_ERASURE> <k> <n>\n"
            "get_upstream_fec|guf <index>\n"
            "set_upstream_scheduler_budget|sus <index> <budget>\n"
            "get_upstream_scheduler_budget|gus <index>\n"
            "set_modulation|sd <modulation>\n"
            "get_modulation|gd\n"
            "set_tx_pacing|stp "
            "max_rate_kbps=N max_data_per_tick=N tx_burst_size=N "
            "tx_burst_interval_us=N\n"
            "get_tx_pacing|gtp\n"
            "get_channel_info|gci\n"
            "ping\n"
            "help\n");
        return;
    }
    if (console_cmd_is(cmd, "ping", "ping"))
    {
        reply("pong\n");
        return;
    }

    if (console_cmd_is(cmd, "set_upstream_fec", "suf") ||
        strcmp(cmd, "set_upsteram_fec") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* a_type = strtok_r(nullptr, " \t", &save);
        char* a_k = strtok_r(nullptr, " \t", &save);
        char* a_n = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        unsigned long k = 0;
        unsigned long n = 0;
        FecType type = FecType::none;
        if (extra != nullptr || !console_parse_u(a_index, &idx) ||
            !console_parse_fec_type(a_type, &type) ||
            !console_parse_u(a_k, &k) || !console_parse_u(a_n, &n))
        {
            reply_nok(
                "usage set_upstream_fec <index> "
                "<NONE|RS_BLOCK_ERASURE> <k> <n>");
            return;
        }
        if (type != FecType::none && (k < 1 || n <= k || n > 255))
        {
            reply_nok("need 1 <= k < n <= 255");
            return;
        }
        std::string err;
        if (!set_fec(static_cast<size_t>(idx), type, static_cast<int>(k),
                     static_cast<int>(n), &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok();
        return;
    }

    if (console_cmd_is(cmd, "get_upstream_fec", "guf") ||
        strcmp(cmd, "get_upsteram_fec") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        if (extra != nullptr || !console_parse_u(a_index, &idx))
        {
            reply_nok("usage get_upstream_fec <index>");
            return;
        }
        FecType type = FecType::none;
        int k = 0;
        int n = 0;
        std::string err;
        if (!get_fec(static_cast<size_t>(idx), &type, &k, &n, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        char args[64];
        if (type == FecType::none)
        {
            snprintf(args, sizeof(args), "%s 0 0", console_fec_type_name(type));
        }
        else
        {
            snprintf(args, sizeof(args), "%s %d %d",
                     console_fec_type_name(type), k, n);
        }
        reply_ok_args(args);
        return;
    }

    if (console_cmd_is(cmd, "set_upstream_scheduler_budget", "sus") ||
        strcmp(cmd, "set_upsteram_scheduler_budget") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* a_budget = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        unsigned long budget = 0;
        if (extra != nullptr || !console_parse_u(a_index, &idx) ||
            !console_parse_u(a_budget, &budget) || budget == 0)
        {
            reply_nok("usage set_upstream_scheduler_budget <index> <budget>");
            return;
        }
        std::string err;
        if (!set_budget(static_cast<size_t>(idx), static_cast<size_t>(budget),
                        &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok();
        return;
    }

    if (console_cmd_is(cmd, "get_upstream_scheduler_budget", "gus") ||
        strcmp(cmd, "get_upsteram_scheduler_budget") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        if (extra != nullptr || !console_parse_u(a_index, &idx))
        {
            reply_nok("usage get_upstream_scheduler_budget <index>");
            return;
        }
        size_t budget = 0;
        std::string err;
        if (!get_budget(static_cast<size_t>(idx), &budget, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        char args[32];
        snprintf(args, sizeof(args), "%zu", budget);
        reply_ok_args(args);
        return;
    }

    if (console_cmd_is(cmd, "set_modulation", "sd"))
    {
        char* a_mod = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        if (extra != nullptr || a_mod == nullptr || a_mod[0] == '\0')
        {
            reply_nok("usage set_modulation <modulation>");
            return;
        }
        std::string err;
        if (!set_modulation(a_mod, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok();
        return;
    }

    if (console_cmd_is(cmd, "get_modulation", "gd"))
    {
        char* extra = strtok_r(nullptr, " \t", &save);
        if (extra != nullptr)
        {
            reply_nok("usage get_modulation");
            return;
        }
        std::string name;
        std::string err;
        if (!get_modulation(&name, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok_args(name.c_str());
        return;
    }

    if (console_cmd_is(cmd, "set_tx_pacing", "stp"))
    {
        uint32_t rate = 0;
        size_t mpt = 0;
        size_t burst = 0;
        uint32_t burst_us = 0;
        bool have_rate = false;
        bool have_mpt = false;
        bool have_burst = false;
        bool have_burst_us = false;
        bool any = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            unsigned long v = 0;
            if (console_parse_kv(a, "max_rate_kbps=", &value) &&
                console_parse_u(value, &v))
            {
                rate = static_cast<uint32_t>(v);
                have_rate = true;
                any = true;
                continue;
            }
            if (console_parse_kv(a, "max_data_per_tick=", &value) &&
                console_parse_u(value, &v) && v >= 1 && v <= 32)
            {
                mpt = static_cast<size_t>(v);
                have_mpt = true;
                any = true;
                continue;
            }
            if (console_parse_kv(a, "tx_burst_size=", &value) &&
                console_parse_u(value, &v) && v >= 1 &&
                v <= k_radio_tx_queue_depth)
            {
                burst = static_cast<size_t>(v);
                have_burst = true;
                any = true;
                continue;
            }
            if (console_parse_kv(a, "tx_burst_interval_us=", &value) &&
                console_parse_u(value, &v))
            {
                burst_us = static_cast<uint32_t>(v);
                have_burst_us = true;
                any = true;
                continue;
            }
            reply_nok(
                "usage set_tx_pacing max_rate_kbps=N max_data_per_tick=N "
                "tx_burst_size=N tx_burst_interval_us=N");
            return;
        }
        if (!any)
        {
            reply_nok(
                "usage set_tx_pacing max_rate_kbps=N max_data_per_tick=N "
                "tx_burst_size=N tx_burst_interval_us=N");
            return;
        }
        std::string err;
        if (!set_tx_pacing(have_rate ? &rate : nullptr,
                           have_mpt ? &mpt : nullptr,
                           have_burst ? &burst : nullptr,
                           have_burst_us ? &burst_us : nullptr, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok();
        return;
    }

    if (console_cmd_is(cmd, "get_tx_pacing", "gtp"))
    {
        char* extra = strtok_r(nullptr, " \t", &save);
        if (extra != nullptr)
        {
            reply_nok("usage get_tx_pacing");
            return;
        }
        uint32_t rate = 0;
        size_t mpt = 0;
        size_t burst = 0;
        uint32_t burst_us = 0;
        std::string err;
        if (!get_tx_pacing(&rate, &mpt, &burst, &burst_us, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        char args[160];
        snprintf(args, sizeof(args),
                 "max_rate_kbps=%u max_data_per_tick=%zu tx_burst_size=%zu "
                 "tx_burst_interval_us=%u",
                 rate, mpt, burst, burst_us);
        reply_ok_args(args);
        return;
    }

    if (console_cmd_is(cmd, "get_channel_info", "gci"))
    {
        char* extra = strtok_r(nullptr, " \t", &save);
        if (extra != nullptr)
        {
            reply_nok("usage get_channel_info");
            return;
        }
        ChannelInfoView view;
        std::string err;
        if (!get_ci(&view, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        char flow[32];
        char rssi[16];
        char snr[16];
        if (view.flow_valid)
        {
            snprintf(flow, sizeof(flow), "%u/%u", view.tx_queue_size,
                     view.tx_queue_capacity);
        }
        else
        {
            snprintf(flow, sizeof(flow), "-");
        }
        if (view.air_valid)
        {
            snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(view.rssi));
            snprintf(snr, sizeof(snr), "%d", static_cast<int>(view.snr));
        }
        else
        {
            snprintf(rssi, sizeof(rssi), "-");
            snprintf(snr, sizeof(snr), "-");
        }
        std::string args;
        char line[384];
        snprintf(line, sizeof(line),
                 "flow=%s flow_t=%s rssi=%s rssi_t=%s snr=%s", flow,
                 view.flow_t, rssi, view.rssi_t, snr);
        args = line;
        snprintf(line, sizeof(line),
                 "\nstream tx_byte=%llu rx_byte=%llu tx_pkt=%llu rx_pkt=%llu "
                 "rx_pkt_loss=%llu",
                 static_cast<unsigned long long>(view.tx_byte),
                 static_cast<unsigned long long>(view.rx_byte),
                 static_cast<unsigned long long>(view.tx_pkt),
                 static_cast<unsigned long long>(view.rx_pkt),
                 static_cast<unsigned long long>(view.rx_pkt_loss));
        args += line;
        reply_ok_args(args.c_str());
        return;
    }

    reply_nok("unknown command, type help");
}

}  // namespace winject
