#include "radio/RadioMplaneParse.h"

#include "console/ConsoleParse.h"
#include "Config.h"

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
        if (console_parse_kv(tok, "channel=", &value) && console_parse_u(value, &v))
        {
            out->channel = static_cast<uint16_t>(v);
            continue;
        }
        if (console_parse_kv(tok, "tx_power=", &value) && console_parse_u(value, &v))
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
            out->cca = (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
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

bool radio_phy_matches_desired(const ManagerRadioView& actual,
                               uint8_t channel, int8_t power_dbm,
                               const std::string& modulation)
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
