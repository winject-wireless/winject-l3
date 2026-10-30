#include "console/ConsoleCommands.h"

#include "console/ConsoleParse.h"
#include "fec/RsBlockErasure.h"
#include "mplane/MplaneErrno.h"
#include "utils/Version.h"
#include "WinjectBuildVersion.h"

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
    "radio_tx|rt [channel= tx_power=<dBm> modulation= cca=<0|1>]\n"
    "radio_mplane|rmp <radio command line>\n"
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
        if (!mplane_parse_kv(a, "ids=", &value))
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
        if (!mplane_parse_kv(a, "keys=", &value))
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

bool parse_upstream_spec(char* save, UpstreamView* spec, bool require_all,
                         bool* got_id, std::string* err)
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
        if (mplane_parse_kv(a, "id=", &value) &&
            mplane_parse_u8(value, &spec->id))
        {
            have_id = true;
            continue;
        }
        if (mplane_parse_kv(a, "txbus=", &value) &&
            mplane_parse_u8(value, &spec->bus_tx))
        {
            have_txbus = true;
            continue;
        }
        if (mplane_parse_kv(a, "rxbus=", &value) &&
            mplane_parse_u8(value, &spec->bus_rx))
        {
            have_rxbus = true;
            continue;
        }
        if (mplane_parse_kv(a, "type=", &value))
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
        if (mplane_parse_kv(a, "rx=", &value))
        {
            spec->rx = value;
            have_rx = true;
            continue;
        }
        if (mplane_parse_kv(a, "tx=", &value))
        {
            spec->tx = value;
            have_tx = true;
            continue;
        }
        if (mplane_parse_kv(a, "fec=", &value))
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
        if (mplane_parse_kv(a, "k=", &value) && mplane_parse_u(value, &v))
        {
            spec->fec_k = static_cast<int>(v);
            have_k = true;
            continue;
        }
        if (mplane_parse_kv(a, "n=", &value) && mplane_parse_u(value, &v))
        {
            spec->fec_n = static_cast<int>(v);
            have_n = true;
            continue;
        }
        if (mplane_parse_kv(a, "fec_timeout=", &value))
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
        if (mplane_parse_kv(a, "quanta=", &value) && mplane_parse_u(value, &v))
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
    else if (spec->fec_k < 1 || spec->fec_n <= spec->fec_k ||
             spec->fec_n > static_cast<int>(RsBlockErasure::k_max_n))
    {
        if (err != nullptr)
        {
            *err = k_einval;
        }
        return false;
    }
    return true;
}

void append_upstream_line(std::string* out, const UpstreamView& u)
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

std::string ok_args(const char* args)
{
    std::string text = "OK";
    if (args != nullptr && args[0] != '\0')
    {
        text += ' ';
        text += args;
    }
    return text;
}

std::string nok(const char* msg)
{
    std::string text = "NOK ";
    text += msg != nullptr ? msg : "error";
    return text;
}

}  // namespace

ConsoleCommands::ConsoleCommands(ControlOps bound_ops)
    : ops(std::move(bound_ops))
{
}

bool ConsoleCommands::valid() const
{
    return ops.add_upstream && ops.remove_upstream && ops.list_upstream &&
           ops.update_upstream && ops.list_upstream_rx_stat &&
           ops.list_upstream_tx_stat && ops.get_metrics && ops.radio_info &&
           ops.radio_caps_info && ops.radio_stats && ops.radio_tx &&
           ops.radio_reset && ops.config_slot && ops.radio_device;
}

void ConsoleCommands::handle(const char* line, Reply reply) const
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
    // Frozen across protocol versions: docs/mplane.md § Version discovery.
    if (console_cmd_is(cmd, "version", "ver"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply(nok(k_einval));
            return;
        }
        const WinjectVersion v = own_version();
        char buf[128];
        snprintf(buf, sizeof(buf), "version ver=%s proto=%u.%u",
                 WINJECT_VERSION_STRING, static_cast<unsigned>(v.x),
                 static_cast<unsigned>(v.y));
        reply(ok_args(buf));
        return;
    }

    if (console_cmd_is(cmd, "radio_device", "rd"))
    {
        RadioDeviceUpdate patch;
        std::string err;
        if (!console_parse_radio_device_args(save, &patch, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        RadioDeviceView view;
        if (!ops.radio_device(patch, &view, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        reply(ok_args(console_format_radio_device(view).c_str()));
        return;
    }

    if (console_cmd_is(cmd, "add_upstream", "au"))
    {
        UpstreamView spec;
        std::string err;
        if (!parse_upstream_spec(save, &spec, true, nullptr, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        if (!ops.add_upstream(spec, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
            return;
        }
        std::string args;
        append_upstream_line(&args, spec);
        reply(ok_args(args.c_str()));
        return;
    }

    if (console_cmd_is(cmd, "list_upstream", "lu"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        std::vector<UpstreamView> rows;
        if (!ops.list_upstream(ids, &rows, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
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
        reply(body);
        return;
    }

    if (console_cmd_is(cmd, "update_upstream", "uu"))
    {
        UpstreamUpdate patch;
        std::string err;
        bool got_id = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            unsigned long v = 0;
            if (mplane_parse_kv(a, "id=", &value) &&
                mplane_parse_u8(value, &patch.id))
            {
                got_id = true;
                continue;
            }
            if (mplane_parse_kv(a, "fec=", &value))
            {
                if (!console_parse_fec_type(value, &patch.fec))
                {
                    reply(nok(k_einval));
                    return;
                }
                patch.have_fec = true;
                continue;
            }
            if (mplane_parse_kv(a, "k=", &value) && mplane_parse_u(value, &v))
            {
                patch.fec_k = static_cast<int>(v);
                patch.have_k = true;
                continue;
            }
            if (mplane_parse_kv(a, "n=", &value) && mplane_parse_u(value, &v))
            {
                patch.fec_n = static_cast<int>(v);
                patch.have_n = true;
                continue;
            }
            if (mplane_parse_kv(a, "fec_timeout=", &value))
            {
                int ms = 0;
                if (!console_parse_duration_ms(value, &ms))
                {
                    reply(nok(k_einval));
                    return;
                }
                patch.fec_timeout_ms = ms;
                patch.have_fec_timeout = true;
                continue;
            }
            if (mplane_parse_kv(a, "quanta=", &value) &&
                mplane_parse_u(value, &v))
            {
                if (v == 0)
                {
                    reply(nok(k_einval));
                    return;
                }
                patch.quanta = static_cast<size_t>(v);
                patch.have_quanta = true;
                continue;
            }
            reply(nok(k_einval));
            return;
        }
        if (!got_id)
        {
            reply(nok(k_einval));
            return;
        }
        UpstreamView out;
        if (!ops.update_upstream(patch, &out, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
            return;
        }
        std::string args;
        append_upstream_line(&args, out);
        reply(ok_args(args.c_str()));
        return;
    }

    if (console_cmd_is(cmd, "remove_upstream", "ru"))
    {
        std::string err;
        const char* value = nullptr;
        char* arg = strtok_r(nullptr, " \t", &save);
        uint8_t id = 0;
        if (arg == nullptr || !mplane_parse_kv(arg, "id=", &value) ||
            !mplane_parse_u8(value, &id) ||
            strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply(nok(k_einval));
            return;
        }
        if (!ops.remove_upstream(id, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
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
            reply(nok(err.c_str()));
            return;
        }
        std::vector<UpstreamRxStatView> rows;
        if (!ops.list_upstream_rx_stat(ids, &rows, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
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
            char line[512];
            snprintf(line, sizeof(line),
                     "upstream_rx_stat id=%u rxbyt=%llu rxpkt=%llu "
                     "rx_oversize=%llu rxgap=%llu "
                     "fec=%s fec_rec=%llu fec_lost=%llu fec_rxbyt=%llu "
                     "fec_rxpkt=%llu fec_rxgap=%llu rx_push_drop=%llu",
                     static_cast<unsigned>(s.id),
                     static_cast<unsigned long long>(s.rxbyt),
                     static_cast<unsigned long long>(s.rxpkt),
                     static_cast<unsigned long long>(s.rx_oversize),
                     static_cast<unsigned long long>(s.rxgap), fec_buf,
                     static_cast<unsigned long long>(s.fec_rec),
                     static_cast<unsigned long long>(s.fec_lost),
                     static_cast<unsigned long long>(s.fec_rxbyt),
                     static_cast<unsigned long long>(s.fec_rxpkt),
                     static_cast<unsigned long long>(s.fec_rxgap),
                     static_cast<unsigned long long>(s.rx_push_drop));
            body += '\n';
            body += line;
        }
        body += '\n';
        reply(body);
        return;
    }

    if (console_cmd_is(cmd, "list_upstream_tx_stat", "lut"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        std::vector<UpstreamTxStatView> rows;
        if (!ops.list_upstream_tx_stat(ids, &rows, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
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
            char line[512];
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
        reply(body);
        return;
    }

    if (console_cmd_is(cmd, "get_metrics", "gm"))
    {
        std::vector<std::string> keys;
        std::string err;
        if (!parse_optional_keys(save, &keys, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        std::vector<MetricView> rows;
        if (!ops.get_metrics(keys, &rows, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
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
        reply(body);
        return;
    }

    if (console_cmd_is(cmd, "radio_info", "ri"))
    {
        ControlReply cr;
        cr.send_text = [reply](const std::string& text)
        {
            std::string out = text;
            if (!out.empty() && out.back() != '\n')
            {
                out += '\n';
            }
            reply(out);
        };
        cr.send_nok = [reply](const char* msg)
        {
            reply(nok(msg));
        };
        ops.radio_info(cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_caps_info", "rci"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply(nok(k_einval));
            return;
        }
        ControlReply cr;
        cr.send_text = [reply](const std::string& text)
        {
            reply(ok_args(text.c_str()));
        };
        cr.send_nok = [reply](const char* msg)
        {
            reply(nok(msg));
        };
        ops.radio_caps_info(cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_stats", "rs"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply(nok(k_einval));
            return;
        }
        ControlReply cr;
        cr.send_text = [reply](const std::string& text)
        {
            std::string out = text;
            if (!out.empty() && out.back() != '\n')
            {
                out += '\n';
            }
            reply(out);
        };
        cr.send_nok = [reply](const char* msg)
        {
            reply(nok(msg));
        };
        ops.radio_stats(cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_tx", "rt"))
    {
        RadioUpdate patch;
        std::string err;
        bool any = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            unsigned long v = 0;
            if (mplane_parse_kv(a, "channel=", &value) &&
                mplane_parse_u(value, &v))
            {
                patch.channel = static_cast<uint16_t>(v);
                patch.have_channel = true;
                any = true;
                continue;
            }
            long sv = 0;
            if (mplane_parse_kv(a, "tx_power=", &value) &&
                mplane_parse_i(value, &sv) && sv >= -128 && sv <= 127)
            {
                patch.tx_power = static_cast<int>(sv);
                patch.have_tx_power = true;
                any = true;
                continue;
            }
            if (mplane_parse_kv(a, "modulation=", &value))
            {
                patch.modulation = value;
                patch.have_modulation = true;
                any = true;
                continue;
            }
            if (mplane_parse_kv(a, "cca=", &value) &&
                mplane_parse_u(value, &v) && v <= 1)
            {
                patch.cca = v == 1;
                patch.have_cca = true;
                any = true;
                continue;
            }
            reply(nok(k_einval));
            return;
        }
        if (!any)
        {
            reply(nok(k_einval));
            return;
        }
        ControlReply cr;
        cr.send_text = [reply](const std::string& text)
        {
            reply(text);
        };
        cr.send_nok = [reply](const char* msg)
        {
            reply(nok(msg));
        };
        ops.radio_tx(patch, cr);
        return;
    }

    if (console_cmd_is(cmd, "radio_mplane", "rmp"))
    {
        // Transparent radio m-plane: the rest of the line goes to the radio
        // unchanged, for radio-specific commands the manager does not model.
        const char* rest = save != nullptr ? save : "";
        rest += strspn(rest, " \t");
        if (*rest == '\0')
        {
            reply(nok(k_einval));
            return;
        }
        if (!ops.radio_mplane)
        {
            reply(nok(k_enosys));
            return;
        }
        ControlReply cr;
        cr.send_text = [reply](const std::string& text)
        {
            reply(text);
        };
        cr.send_nok = [reply](const char* msg)
        {
            reply(nok(msg));
        };
        ops.radio_mplane(std::string(rest), cr);
        return;
    }

    if (console_cmd_is(cmd, "reset", "r"))
    {
        if (strtok_r(nullptr, " 	", &save) != nullptr)
        {
            reply(nok(k_einval));
            return;
        }
        ControlReply cr;
        cr.send_text = [reply](const std::string& text)
        {
            if (!text.empty())
            {
                reply(text);
                return;
            }
            reply(ok_args(nullptr));
        };
        cr.send_nok = [reply](const char* msg)
        {
            reply(nok(msg));
        };
        ops.radio_reset(cr);
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
            if (mplane_parse_kv(a, "slot=", &value) &&
                mplane_parse_u(value, &slot))
            {
                have_slot = true;
                continue;
            }
            reply(nok(k_einval));
            return;
        }
        if (!have_slot || slot > 255)
        {
            reply(nok(k_einval));
            return;
        }
        ControlReply cr;
        cr.send_text = [reply, slot](const std::string& text)
        {
            if (!text.empty())
            {
                reply(text);
                return;
            }
            char args[32];
            snprintf(args, sizeof(args), "slot=%lu", slot);
            reply(ok_args(args));
        };
        cr.send_nok = [reply](const char* msg)
        {
            reply(nok(msg));
        };
        ops.config_slot(static_cast<uint8_t>(slot), cr);
        return;
    }

    reply(nok(k_enosys));
}

}  // namespace winject
