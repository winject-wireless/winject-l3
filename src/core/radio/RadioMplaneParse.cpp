#include "radio/RadioMplaneParse.h"

#include "mplane/MplaneText.h"
#include "radio/PhyAirtime.h"

#include <initializer_list>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

namespace winject::core
{

bool parse_radio_tx_line(const std::string& line, RadioPhyView* out)
{
    static const char k_prefix[] = "radio_tx ";
    if (line.rfind(k_prefix, 0) != 0 || out == nullptr)
    {
        return false;
    }
    std::string args = line.substr(sizeof(k_prefix) - 1);
    std::vector<char> buf(args.begin(), args.end());
    buf.push_back('\0');
    char* save = nullptr;
    for (char* tok = strtok_r(buf.data(), " \t", &save); tok != nullptr;
         tok = strtok_r(nullptr, " \t", &save))
    {
        const char* value = nullptr;
        unsigned long v = 0;
        if (mplane_parse_kv(tok, "channel=", &value) &&
            mplane_parse_u(value, &v))
        {
            out->channel = static_cast<uint16_t>(v);
            continue;
        }
        long sv = 0;
        if (mplane_parse_kv(tok, "tx_power=", &value) &&
            mplane_parse_i(value, &sv))
        {
            out->tx_power = static_cast<int>(sv);
            continue;
        }
        if (mplane_parse_kv(tok, "modulation=", &value))
        {
            out->modulation = value;
            continue;
        }
        if (mplane_parse_kv(tok, "cca=", &value))
        {
            out->cca =
                (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
            out->cca_valid = true;
        }
    }
    return true;
}

bool parse_radio_info_body(const std::string& body, RadioPhyView* out)
{
    if (out == nullptr)
    {
        return false;
    }
    size_t off = 0;
    bool any = false;
    while (off < body.size())
    {
        const auto nl = body.find('\n', off);
        const std::string line = nl == std::string::npos
                                     ? body.substr(off)
                                     : body.substr(off, nl - off);
        if (line.rfind("radio_tx ", 0) == 0)
        {
            parse_radio_tx_line(line, out);
            any = true;
        }
        else if (line.rfind("radio_rx", 0) == 0)
        {
            const auto pos = line.find("rssi=");
            if (pos != std::string::npos)
            {
                const char* value = line.c_str() + pos + 5;
                char* end = nullptr;
                const long rssi = strtol(value, &end, 10);
                if (end != value)
                {
                    out->rssi = static_cast<int8_t>(rssi);
                    out->rssi_valid = true;
                    any = true;
                }
            }
        }
        if (nl == std::string::npos)
        {
            break;
        }
        off = nl + 1;
    }
    return any;
}

namespace
{

struct CounterKey
{
    const char* key;
    uint64_t* field;
};

// "<name> k=v k=v ...": fills every key in keys, ignores others.
bool parse_counter_line(const std::string& line, const char* name,
                        std::initializer_list<CounterKey> keys)
{
    const size_t name_len = strlen(name);
    if (line.compare(0, name_len, name) != 0 ||
        (line.size() > name_len && line[name_len] != ' '))
    {
        return false;
    }
    std::vector<char> buf(line.begin() + static_cast<ptrdiff_t>(name_len),
                          line.end());
    buf.push_back('\0');
    size_t found = 0;
    char* save = nullptr;
    for (char* tok = strtok_r(buf.data(), " \t", &save); tok != nullptr;
         tok = strtok_r(nullptr, " \t", &save))
    {
        const char* eq = strchr(tok, '=');
        if (eq == nullptr)
        {
            return false;
        }
        for (const CounterKey& k : keys)
        {
            const size_t key_len = strlen(k.key);
            if (static_cast<size_t>(eq - tok) != key_len ||
                strncmp(tok, k.key, key_len) != 0)
            {
                continue;
            }
            char* end = nullptr;
            const char* value = eq + 1;
            if (*value == '\0' || *value == '-')
            {
                return false;
            }
            const unsigned long long v = strtoull(value, &end, 10);
            if (end == nullptr || *end != '\0')
            {
                return false;
            }
            *k.field = static_cast<uint64_t>(v);
            ++found;
            break;
        }
    }
    return found == keys.size();
}

std::string trim_copy(const std::string& s)
{
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool parse_fcs_token(const char* tok, RadioFcsMode* out_mode)
{
    const char* value = nullptr;
    if (!mplane_parse_kv(tok, "fcs=", &value) || value == nullptr)
    {
        return false;
    }
    if (strcasecmp(value, "SIGNAL") == 0)
    {
        *out_mode = RadioFcsMode::signal;
        return true;
    }
    if (strcasecmp(value, "ACTUAL") == 0)
    {
        *out_mode = RadioFcsMode::actual;
        return true;
    }
    return false;
}

}  // namespace

bool parse_radio_counters(const std::vector<std::string>& lines,
                          RadioCounters* out)
{
    if (out == nullptr || lines.size() != 2)
    {
        return false;
    }
    RadioCounters c;
    if (!parse_counter_line(lines[0], "tx_info",
                            {{"tx_queue_sz", &c.tx.queue_size},
                             {"in_flight", &c.tx.in_flight},
                             {"dropped_invalid_frame",
                              &c.tx.dropped_invalid_frame},
                             {"dropped_tx_queue", &c.tx.dropped_queue},
                             {"dropped_wifi", &c.tx.dropped_wifi},
                             {"ether_pkt", &c.tx.ether_packets},
                             {"air_pkt", &c.tx.air_packets},
                             {"ts", &c.tx.timestamp_us}}) ||
        !parse_counter_line(lines[1], "rx_info",
                            {{"rx_queue_sz", &c.rx.queue_size},
                             {"dropped_filter_mismatched",
                              &c.rx.dropped_filter_mismatched},
                             {"dropped_rx_queue", &c.rx.dropped_queue},
                             {"dropped_no_peer", &c.rx.dropped_no_peer},
                             {"dropped_send_failed", &c.rx.dropped_send_failed},
                             {"ether_pkt", &c.rx.ether_packets},
                             {"air_pkt", &c.rx.air_packets},
                             {"ts", &c.rx.timestamp_us}}))
    {
        return false;
    }
    *out = c;
    return true;
}

bool parse_radio_caps_line(const std::string& line, RadioFcsMode* out_mode)
{
    if (out_mode == nullptr)
    {
        return false;
    }
    std::string body = trim_copy(line);
    if (body.rfind("OK ", 0) == 0)
    {
        body = trim_copy(body.substr(3));
    }
    static const char k_prefix[] = "radio_caps_info";
    if (body.rfind(k_prefix, 0) != 0)
    {
        return false;
    }
    std::string rest = body.substr(sizeof(k_prefix) - 1);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
    {
        rest.erase(rest.begin());
    }
    std::vector<char> buf(rest.begin(), rest.end());
    buf.push_back('\0');
    char* save = nullptr;
    bool any = false;
    for (char* tok = strtok_r(buf.data(), " \t", &save); tok != nullptr;
         tok = strtok_r(nullptr, " \t", &save))
    {
        RadioFcsMode parsed = RadioFcsMode::unknown;
        if (parse_fcs_token(tok, &parsed))
        {
            *out_mode = parsed;
            any = true;
        }
    }
    return any && *out_mode != RadioFcsMode::unknown;
}

bool resolve_radio_caps_mplane(const MplaneResult& r, RadioFcsMode* out_mode,
                               bool* legacy_enosys, std::string* error)
{
    if (out_mode == nullptr)
    {
        return false;
    }
    if (legacy_enosys != nullptr)
    {
        *legacy_enosys = false;
    }
    if (r.ok)
    {
        std::string line = r.payload;
        if (line.empty() && !r.body_lines.empty())
        {
            line = r.body_lines.front();
        }
        if (!parse_radio_caps_line(line, out_mode))
        {
            if (error != nullptr)
            {
                *error = "invalid radio_caps_info reply";
            }
            return false;
        }
        return true;
    }
    const std::string err = trim_copy(r.error);
    if (err == "ENOSYS" || err.find("ENOSYS") != std::string::npos)
    {
        *out_mode = RadioFcsMode::actual;
        if (legacy_enosys != nullptr)
        {
            *legacy_enosys = true;
        }
        return true;
    }
    if (error != nullptr)
    {
        *error = err.empty() ? "radio_caps_info failed" : err;
    }
    return false;
}

const char* radio_fcs_mode_name(RadioFcsMode mode)
{
    switch (mode)
    {
        case RadioFcsMode::signal:
            return "SIGNAL";
        case RadioFcsMode::actual:
            return "ACTUAL";
        default:
            return "UNKNOWN";
    }
}

std::string format_radio_caps_ok_line(RadioFcsMode mode)
{
    return std::string("radio_caps_info fcs=") + radio_fcs_mode_name(mode);
}

bool radio_phy_matches_desired(const RadioPhyView& actual, uint8_t channel,
                               int8_t power_dbm, const std::string& modulation)
{
    const std::string want = phy_canonical_name(modulation);
    const std::string have = phy_canonical_name(actual.modulation);
    if (want.empty() || have.empty())
    {
        return false;
    }
    return static_cast<uint16_t>(channel) == actual.channel &&
           static_cast<int>(power_dbm) == actual.tx_power && want == have;
}

}  // namespace winject::core
