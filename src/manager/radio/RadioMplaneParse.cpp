#include "radio/RadioMplaneParse.h"

#include "Config.h"
#include "console/ConsoleParse.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

namespace winject
{

bool parse_radio_tx_line(const std::string& line, ManagerRadioView* out)
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
        if (console_parse_kv(tok, "channel=", &value) &&
            console_parse_u(value, &v))
        {
            out->channel = static_cast<uint16_t>(v);
            continue;
        }
        if (console_parse_kv(tok, "tx_power=", &value) &&
            console_parse_u(value, &v))
        {
            out->tx_power = static_cast<int>(v);
            continue;
        }
        if (console_parse_kv(tok, "modulation=", &value))
        {
            out->modulation = value;
            continue;
        }
        if (console_parse_kv(tok, "cca=", &value))
        {
            out->cca =
                (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
            out->cca_valid = true;
        }
    }
    return true;
}

bool parse_radio_info_body(const std::string& body, ManagerRadioView* out)
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
    if (!console_parse_kv(tok, "fcs=", &value) || value == nullptr)
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

bool radio_phy_matches_desired(const ManagerRadioView& actual, uint8_t channel,
                               int8_t power_dbm, const std::string& modulation)
{
    const std::string want = Config::canonical_modulation(modulation);
    const std::string have = Config::canonical_modulation(actual.modulation);
    if (want.empty() || have.empty())
    {
        return false;
    }
    return static_cast<uint16_t>(channel) == actual.channel &&
           static_cast<int>(power_dbm) == actual.tx_power && want == have;
}

}  // namespace winject
