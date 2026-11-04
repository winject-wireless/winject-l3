#include "console/ConsoleCommands.h"

#include "WinjectBuildVersion.h"
#include "console/ConsoleParse.h"
#include "mplane/MplaneErrno.h"
#include "winject_l3/Console.h"

#include <chrono>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <vector>

namespace winject
{

namespace
{

using winject_l3::FecType;
using winject_l3::Node;
using winject_l3::Result;
using winject_l3::Status;

// One upstream as add_upstream and list_upstream print it.
struct UpstreamLine
{
    uint8_t id = 0;
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    std::string type = "UDP";
    std::string rx;
    std::string tx;
    FecType fec = FecType::none;
    int fec_k = 0;
    int fec_n = 0;
    int fec_timeout_ms = 0;
    size_t quanta = 0;
};

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
    "radio_domain [id=<u8>]\n"
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

bool parse_upstream_spec(char* save, UpstreamLine* spec, bool require_all,
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
    // The largest n is the API's to check (EINVAL from add).
    else if (spec->fec_k < 1 || spec->fec_n <= spec->fec_k)
    {
        if (err != nullptr)
        {
            *err = k_einval;
        }
        return false;
    }
    return true;
}

void append_upstream_line(std::string* out, const UpstreamLine& u)
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

std::string nok_status(const Status& s)
{
    return nok(s.error.empty() ? "error" : s.error.c_str());
}

// radio_info / radio_stats replies end with a newline.
std::string with_newline(std::string text)
{
    if (!text.empty() && text.back() != '\n')
    {
        text += '\n';
    }
    return text;
}

UpstreamLine line_of(const winject_l3::UpstreamSpec& u)
{
    UpstreamLine line;
    line.id = u.id;
    line.bus_tx = u.bus_tx;
    line.bus_rx = u.bus_rx;
    line.type = u.transport == winject_l3::Transport::queue ? "QUEUE" : "UDP";
    line.rx = u.rx;
    line.tx = u.tx;
    line.fec = u.fec.type;
    line.fec_k = u.fec.k;
    line.fec_n = u.fec.n;
    line.fec_timeout_ms = static_cast<int>(u.fec.timeout.count());
    line.quanta = u.quanta;
    return line;
}

// The upstreams a listing covers: ids, or every upstream in add order.
bool listing_upstreams(const Node& node, const std::vector<uint8_t>& ids,
                       std::vector<winject_l3::Upstream>* out,
                       std::string* error)
{
    out->clear();
    winject_l3::UpstreamManager upstreams = node.upstreams();
    if (ids.empty())
    {
        auto all = upstreams.list();
        if (!all.ok())
        {
            *error = all.status.error;
            return false;
        }
        *out = std::move(all.value);
        return true;
    }
    for (uint8_t id : ids)
    {
        auto up = upstreams.get(id);
        if (!up.ok())
        {
            *error = up.status.error;
            return false;
        }
        out->push_back(up.value);
    }
    return true;
}

// Calls a radio command in the callback or the blocking form.
template <class T>
void radio_call(bool blocking, std::chrono::milliseconds timeout,
                const std::function<void(winject_l3::Done<T>)>& async,
                const std::function<Result<T>(std::chrono::milliseconds)>& sync,
                std::function<void(const Result<T>&)> then)
{
    if (blocking)
    {
        then(sync(timeout));
        return;
    }
    async(
        [then](Result<T> r)
        {
            then(r);
        });
}

struct NoValue
{
};

// The Status-only radio commands (reset, config slot) as Result<NoValue>.
void radio_status_call(
    bool blocking, std::chrono::milliseconds timeout,
    const std::function<void(winject_l3::StatusDone)>& async,
    const std::function<Status(std::chrono::milliseconds)>& sync,
    std::function<void(const Status&)> then)
{
    if (blocking)
    {
        then(sync(timeout));
        return;
    }
    async(
        [then](Status s)
        {
            then(s);
        });
}

}  // namespace

void ConsoleCommands::handle(const Node& node, const char* line, Reply reply,
                             bool blocking, std::chrono::milliseconds timeout)
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
        const winject_l3::Version v = winject_l3::version();
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
        if (patch.id != 0)
        {
            reply(nok(k_einval));
            return;
        }
        auto radio = node.radios().get();
        if (!radio.ok())
        {
            reply(nok_status(radio.status));
            return;
        }
        auto device = radio.value.device();
        if (!device.ok())
        {
            reply(nok_status(device.status));
            return;
        }
        winject_l3::RadioDevice d = device.value;
        if (patch.have_dplane && !d.dplane_tx.empty())
        {
            // A queue d-plane is fixed at startup.
            reply(nok(k_einval));
            return;
        }
        if (patch.have_mplane)
        {
            d.mplane = patch.mplane;
        }
        if (patch.have_dplane)
        {
            d.dplane = patch.dplane;
        }
        if (patch.have_fcs)
        {
            d.fcs = patch.fcs;
        }
        const Status set = radio.value.set_device(d);
        if (!set.ok())
        {
            reply(nok_status(set));
            return;
        }
        reply(ok_args(console_format_radio_device(0, d).c_str()));
        return;
    }

    if (console_cmd_is(cmd, "radio_domain", nullptr))
    {
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            uint8_t id = 0;
            if (!mplane_parse_kv(a, "id=", &value) ||
                !mplane_parse_u8(value, &id) || id != 0)
            {
                reply(nok(k_einval));
                return;
            }
        }
        auto radio = node.radios().get();
        if (!radio.ok())
        {
            reply(nok_status(radio.status));
            return;
        }
        auto domain = radio.value.domain();
        if (!domain.ok())
        {
            reply(nok_status(domain.status));
            return;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "radio_domain id=0 domain=%x",
                 static_cast<unsigned>(domain.value));
        reply(ok_args(buf));
        return;
    }

    if (console_cmd_is(cmd, "add_upstream", "au"))
    {
        UpstreamLine spec;
        std::string err;
        if (!parse_upstream_spec(save, &spec, true, nullptr, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        winject_l3::UpstreamSpec add;
        add.id = spec.id;
        add.bus_tx = spec.bus_tx;
        add.bus_rx = spec.bus_rx;
        add.transport = winject_l3::Transport::udp;
        add.rx = spec.rx;
        add.tx = spec.tx;
        add.fec.type = spec.fec;
        add.fec.k = spec.fec_k;
        add.fec.n = spec.fec_n;
        add.fec.timeout = std::chrono::milliseconds(spec.fec_timeout_ms);
        add.quanta = spec.quanta;
        const auto added = node.upstreams().add(add);
        if (!added.ok())
        {
            reply(nok_status(added.status));
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
        std::vector<winject_l3::Upstream> ups;
        if (!listing_upstreams(node, ids, &ups, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
            return;
        }
        std::vector<UpstreamLine> rows;
        for (const auto& up : ups)
        {
            const auto spec = up.spec();
            if (!spec.ok())
            {
                if (!ids.empty())
                {
                    reply(nok_status(spec.status));
                    return;
                }
                continue;  // removed meanwhile
            }
            rows.push_back(line_of(spec.value));
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
        uint8_t id = 0;
        bool got_id = false;
        bool have_fec = false;
        winject_l3::FecType fec = winject_l3::FecType::none;
        bool have_k = false;
        int fec_k = 0;
        bool have_n = false;
        int fec_n = 0;
        bool have_timeout = false;
        int fec_timeout_ms = 0;
        bool have_quanta = false;
        size_t quanta = 0;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            unsigned long v = 0;
            if (mplane_parse_kv(a, "id=", &value) &&
                mplane_parse_u8(value, &id))
            {
                got_id = true;
                continue;
            }
            if (mplane_parse_kv(a, "fec=", &value))
            {
                if (!console_parse_fec_type(value, &fec))
                {
                    reply(nok(k_einval));
                    return;
                }
                have_fec = true;
                continue;
            }
            if (mplane_parse_kv(a, "k=", &value) && mplane_parse_u(value, &v))
            {
                fec_k = static_cast<int>(v);
                have_k = true;
                continue;
            }
            if (mplane_parse_kv(a, "n=", &value) && mplane_parse_u(value, &v))
            {
                fec_n = static_cast<int>(v);
                have_n = true;
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
                fec_timeout_ms = ms;
                have_timeout = true;
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
                quanta = static_cast<size_t>(v);
                have_quanta = true;
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
        auto up = node.upstreams().get(id);
        if (!up.ok())
        {
            reply(nok_status(up.status));
            return;
        }
        const bool fec_field = have_fec || have_k || have_n || have_timeout;
        if (!fec_field && !have_quanta)
        {
            reply(nok(k_einval));
            return;
        }
        // An explicit fec_timeout below 1 ms is invalid, as before; the API
        // only sees the merged FEC.
        if (have_timeout && fec_timeout_ms < 1)
        {
            reply(nok(k_einval));
            return;
        }
        winject_l3::UpstreamPatch patch;
        if (fec_field)
        {
            const auto current = up.value.spec();
            if (!current.ok())
            {
                reply(nok_status(current.status));
                return;
            }
            winject_l3::Fec merged = current.value.fec;
            if (have_fec)
            {
                merged.type = fec;
            }
            if (have_k)
            {
                merged.k = fec_k;
            }
            if (have_n)
            {
                merged.n = fec_n;
            }
            if (have_timeout)
            {
                merged.timeout = std::chrono::milliseconds(fec_timeout_ms);
            }
            patch.fec = merged;
        }
        if (have_quanta)
        {
            patch.quanta = quanta;
        }
        const auto updated = up.value.update(patch);
        if (!updated.ok())
        {
            reply(nok_status(updated.status));
            return;
        }
        std::string args;
        append_upstream_line(&args, line_of(updated.value));
        reply(ok_args(args.c_str()));
        return;
    }

    if (console_cmd_is(cmd, "remove_upstream", "ru"))
    {
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
        const Status removed = node.upstreams().remove(id);
        if (!removed.ok())
        {
            reply(nok_status(removed));
            return;
        }
        reply("OK\n");
        return;
    }

    const bool rx_stat = console_cmd_is(cmd, "list_upstream_rx_stat", "lur");
    if (rx_stat || console_cmd_is(cmd, "list_upstream_tx_stat", "lut"))
    {
        std::vector<uint8_t> ids;
        std::string err;
        if (!parse_optional_ids(save, &ids, &err))
        {
            reply(nok(err.c_str()));
            return;
        }
        std::vector<winject_l3::Upstream> ups;
        if (!listing_upstreams(node, ids, &ups, &err))
        {
            reply(nok(err.empty() ? "error" : err.c_str()));
            return;
        }
        std::vector<std::string> lines;
        for (const auto& up : ups)
        {
            if (rx_stat)
            {
                const auto s = up.rx_stats();
                if (!s.ok())
                {
                    reply(nok_status(s.status));
                    return;
                }
                lines.push_back(winject_l3::console::format(up.id(), s.value));
            }
            else
            {
                const auto s = up.tx_stats();
                if (!s.ok())
                {
                    reply(nok_status(s.status));
                    return;
                }
                lines.push_back(winject_l3::console::format(up.id(), s.value));
            }
        }
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "N=%zu T=%llu", lines.size(),
                 static_cast<unsigned long long>(epoch_us()));
        std::string body = hdr;
        for (const auto& l : lines)
        {
            body += '\n';
            body += l;
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
        const auto rows = node.metrics(keys);
        if (!rows.ok())
        {
            reply(nok_status(rows.status));
            return;
        }
        std::string body;
        const size_t count = rows.value.size();
        const unsigned n_field =
            count > 65535u ? 65535u : static_cast<unsigned>(count);
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "N=%u T=%llu", n_field,
                 static_cast<unsigned long long>(epoch_us()));
        body = hdr;
        for (const auto& row : rows.value)
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

    if (console_cmd_is(cmd, "radio_mplane", "rmp"))
    {
        // Transparent radio m-plane: the rest of the line goes to the radio
        // unchanged, for radio-specific commands l3 does not model.
        const char* rest = save != nullptr ? save : "";
        rest += strspn(rest, " \t");
        if (*rest == '\0')
        {
            reply(nok(k_einval));
            return;
        }
        auto got = node.radios().get();
        if (!got.ok())
        {
            reply(nok_status(got.status));
            return;
        }
        auto radio = std::make_shared<winject_l3::Radio>(got.value);
        const std::string line(rest);
        radio_call<std::string>(
            blocking, timeout,
            [radio, line](winject_l3::Done<std::string> done)
            {
                radio->mplane(line, std::move(done));
            },
            [radio, line](std::chrono::milliseconds t)
            {
                return radio->mplane(line, t);
            },
            [reply](const Result<std::string>& r)
            {
                if (!r.ok())
                {
                    reply(nok_status(r.status));
                    return;
                }
                reply(r.value);
            });
        return;
    }

    const bool is_info = console_cmd_is(cmd, "radio_info", "ri");
    const bool is_caps = console_cmd_is(cmd, "radio_caps_info", "rci");
    const bool is_stats = console_cmd_is(cmd, "radio_stats", "rs");
    const bool is_tx = console_cmd_is(cmd, "radio_tx", "rt");
    const bool is_reset = console_cmd_is(cmd, "reset", "r");
    const bool is_config = strcmp(cmd, "config") == 0;
    if (!(is_info || is_caps || is_stats || is_tx || is_reset || is_config))
    {
        reply(nok(k_enosys));
        return;
    }

    // Arguments first, as before: a bad line is EINVAL with or without a
    // radio.
    winject_l3::RadioPhyPatch phy;
    unsigned long slot = 0;
    if ((is_caps || is_stats || is_reset) &&
        strtok_r(nullptr, " \t", &save) != nullptr)
    {
        reply(nok(k_einval));
        return;
    }
    if (is_tx)
    {
        bool any = false;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            unsigned long v = 0;
            if (mplane_parse_kv(a, "channel=", &value) &&
                mplane_parse_u(value, &v))
            {
                // The radio checks the range; values no radio accepts
                // would wrap in the patch.
                if (v > 255)
                {
                    reply(nok(k_einval));
                    return;
                }
                phy.channel = static_cast<uint8_t>(v);
                any = true;
                continue;
            }
            long sv = 0;
            if (mplane_parse_kv(a, "tx_power=", &value) &&
                mplane_parse_i(value, &sv))
            {
                if (sv < -128 || sv > 127)
                {
                    reply(nok(k_einval));
                    return;
                }
                phy.power_dbm = static_cast<int8_t>(sv);
                any = true;
                continue;
            }
            if (mplane_parse_kv(a, "modulation=", &value))
            {
                phy.modulation = std::string(value);
                any = true;
                continue;
            }
            if (mplane_parse_kv(a, "cca=", &value) &&
                mplane_parse_u(value, &v) && v <= 1)
            {
                phy.cca = v == 1;
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
    }
    if (is_config)
    {
        const char* value = nullptr;
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
    }

    auto got = node.radios().get();
    if (!got.ok())
    {
        reply(nok_status(got.status));
        return;
    }
    // Copies: the callback forms may answer after this returns.
    auto radio = std::make_shared<winject_l3::Radio>(got.value);
    using winject_l3::RadioCaps;
    using winject_l3::RadioInfo;
    using winject_l3::RadioStats;
    if (is_info || is_tx)
    {
        std::function<void(winject_l3::Done<RadioInfo>)> async;
        std::function<Result<RadioInfo>(std::chrono::milliseconds)> sync;
        if (is_info)
        {
            async = [radio](winject_l3::Done<RadioInfo> done)
            {
                radio->info(std::move(done));
            };
            sync = [radio](std::chrono::milliseconds t)
            {
                return radio->info(t);
            };
        }
        else
        {
            async = [radio, phy](winject_l3::Done<RadioInfo> done)
            {
                radio->set_phy(phy, std::move(done));
            };
            sync = [radio, phy](std::chrono::milliseconds t)
            {
                return radio->set_phy(phy, t);
            };
        }
        radio_call<RadioInfo>(
            blocking, timeout, async, sync,
            [reply, is_info](const Result<RadioInfo>& r)
            {
                if (!r.ok())
                {
                    reply(nok_status(r.status));
                    return;
                }
                // radio_tx relays the radio's reply as is.
                reply(is_info ? with_newline(r.value.text) : r.value.text);
            });
        return;
    }
    if (is_caps)
    {
        radio_call<RadioCaps>(
            blocking, timeout,
            [radio](winject_l3::Done<RadioCaps> done)
            {
                radio->caps(std::move(done));
            },
            [radio](std::chrono::milliseconds t)
            {
                return radio->caps(t);
            },
            [reply](const Result<RadioCaps>& r)
            {
                if (!r.ok())
                {
                    reply(nok_status(r.status));
                    return;
                }
                reply(ok_args(r.value.text.c_str()));
            });
        return;
    }
    if (is_stats)
    {
        radio_call<RadioStats>(
            blocking, timeout,
            [radio](winject_l3::Done<RadioStats> done)
            {
                radio->stats(std::move(done));
            },
            [radio](std::chrono::milliseconds t)
            {
                return radio->stats(t);
            },
            [reply](const Result<RadioStats>& r)
            {
                if (!r.ok())
                {
                    reply(nok_status(r.status));
                    return;
                }
                reply(with_newline(winject_l3::console::format(r.value)));
            });
        return;
    }
    if (is_reset)
    {
        radio_status_call(
            blocking, timeout,
            [radio](winject_l3::StatusDone done)
            {
                radio->reset(std::move(done));
            },
            [radio](std::chrono::milliseconds t)
            {
                return radio->reset(t);
            },
            [reply](const Status& s)
            {
                if (!s.ok())
                {
                    reply(nok_status(s));
                    return;
                }
                reply(ok_args(nullptr));
            });
        return;
    }
    const uint8_t slot_u8 = static_cast<uint8_t>(slot);
    radio_status_call(
        blocking, timeout,
        [radio, slot_u8](winject_l3::StatusDone done)
        {
            radio->set_config_slot(slot_u8, std::move(done));
        },
        [radio, slot_u8](std::chrono::milliseconds t)
        {
            return radio->set_config_slot(slot_u8, t);
        },
        [reply, slot](const Status& s)
        {
            if (!s.ok())
            {
                reply(nok_status(s));
                return;
            }
            char args[32];
            snprintf(args, sizeof(args), "slot=%lu", slot);
            reply(ok_args(args));
        });
}

}  // namespace winject
