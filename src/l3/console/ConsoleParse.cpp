#include "console/ConsoleParse.h"

#include "mplane/MplaneErrno.h"

#include <arpa/inet.h>
#include <climits>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/socket.h>

namespace winject::core
{

using winject::FecType;
using winject::RadioDevice;
using winject::RadioFcs;

bool console_parse_fec_type(const char* text, FecType* out)
{
    if (text == nullptr || out == nullptr)
    {
        return false;
    }
    if (strcmp(text, "NONE") == 0 || strcmp(text, "none") == 0)
    {
        *out = FecType::none;
        return true;
    }
    if (strcmp(text, "RS_BLOCK_ERASURE") == 0 || strcmp(text, "BLOCK") == 0)
    {
        *out = FecType::rs_block;
        return true;
    }
    return false;
}

bool console_format_fec_display(FecType type, int k, int n, char* out,
                                size_t out_len)
{
    if (out == nullptr || out_len == 0)
    {
        return false;
    }
    if (type == FecType::none)
    {
        snprintf(out, out_len, "none");
        return true;
    }
    snprintf(out, out_len, "block(%d,%d)", k, n);
    return true;
}

bool console_parse_duration_ms(const char* text, int* out_ms)
{
    if (text == nullptr || out_ms == nullptr || *text == '\0')
    {
        return false;
    }
    size_t n = strlen(text);
    unsigned long v = 0;
    if (n >= 2 && (text[n - 1] == 's' || text[n - 1] == 'S') &&
        (text[n - 2] == 'm' || text[n - 2] == 'M'))
    {
        char num[32];
        if (n - 2 >= sizeof(num))
        {
            return false;
        }
        memcpy(num, text, n - 2);
        num[n - 2] = '\0';
        if (!mplane_parse_u(num, &v) || v > static_cast<unsigned long>(INT_MAX))
        {
            return false;
        }
        *out_ms = static_cast<int>(v);
        return true;
    }
    if (!mplane_parse_u(text, &v) || v > static_cast<unsigned long>(INT_MAX))
    {
        return false;
    }
    *out_ms = static_cast<int>(v);
    return true;
}

bool console_parse_id_list(const char* text, std::vector<uint8_t>* out)
{
    if (text == nullptr || out == nullptr)
    {
        return false;
    }
    out->clear();
    if (*text == '\0')
    {
        return true;
    }
    const char* p = text;
    while (*p != '\0')
    {
        char* end = nullptr;
        errno = 0;
        const unsigned long v = strtoul(p, &end, 10);
        if (errno != 0 || end == p || v > 255)
        {
            return false;
        }
        out->push_back(static_cast<uint8_t>(v));
        if (*end == '\0')
        {
            return true;
        }
        if (*end != ',')
        {
            return false;
        }
        p = end + 1;
        if (*p == '\0')
        {
            return false;
        }
    }
    return true;
}

bool console_parse_string_list(const char* text, std::vector<std::string>* out)
{
    if (text == nullptr || out == nullptr)
    {
        return false;
    }
    out->clear();
    if (*text == '\0')
    {
        return true;
    }
    const char* p = text;
    while (*p != '\0')
    {
        const char* comma = strchr(p, ',');
        const size_t len =
            comma != nullptr ? static_cast<size_t>(comma - p) : strlen(p);
        if (len == 0)
        {
            return false;
        }
        out->emplace_back(p, len);
        if (comma == nullptr)
        {
            return true;
        }
        p = comma + 1;
        if (*p == '\0')
        {
            return false;
        }
    }
    return true;
}

void console_trim_line(char* line)
{
    if (line == nullptr)
    {
        return;
    }
    size_t n = strlen(line);
    while (n > 0)
    {
        const char c = line[n - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
        {
            break;
        }
        line[--n] = '\0';
    }
}

bool console_cmd_is(const char* cmd, const char* full, const char* abbrev,
                    const char* abbrev2)
{
    if (cmd == nullptr)
    {
        return false;
    }
    if (full != nullptr && strcmp(cmd, full) == 0)
    {
        return true;
    }
    if (abbrev != nullptr && strcmp(cmd, abbrev) == 0)
    {
        return true;
    }
    return abbrev2 != nullptr && strcmp(cmd, abbrev2) == 0;
}

bool console_parse_radio_fcs(const char* text, RadioFcs* out)
{
    if (text == nullptr || out == nullptr)
    {
        return false;
    }
    std::string v;
    for (const char* p = text; *p != '\0'; ++p)
    {
        v.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    if (v == "auto")
    {
        *out = RadioFcs::auto_detect;
        return true;
    }
    if (v == "signal")
    {
        *out = RadioFcs::signal;
        return true;
    }
    if (v == "actual")
    {
        *out = RadioFcs::actual;
        return true;
    }
    return false;
}

const char* console_format_radio_fcs(RadioFcs fcs)
{
    switch (fcs)
    {
        case RadioFcs::signal:
            return "SIGNAL";
        case RadioFcs::actual:
            return "ACTUAL";
        case RadioFcs::auto_detect:
        default:
            return "AUTO";
    }
}

bool console_parse_radio_device_args(char* save, RadioDeviceUpdate* out,
                                     std::string* err)
{
    if (out == nullptr)
    {
        return false;
    }
    bool have_id = false;
    bool seen_mplane = false;
    bool seen_dplane = false;
    bool seen_fcs = false;
    for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
         a = strtok_r(nullptr, " \t", &save))
    {
        const char* value = nullptr;
        if (mplane_parse_kv(a, "id=", &value))
        {
            if (have_id)
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            uint8_t id = 0;
            if (!mplane_parse_u8(value, &id))
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            out->id = id;
            have_id = true;
            continue;
        }
        if (mplane_parse_kv(a, "mplane=", &value))
        {
            if (seen_mplane || value == nullptr || *value == '\0' ||
                strcmp(value, "-") == 0)
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            sockaddr_in addr = {};
            if (!mplane_parse_ipv4_port(value, &addr))
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            out->have_mplane = true;
            out->mplane = addr;
            seen_mplane = true;
            continue;
        }
        if (mplane_parse_kv(a, "dplane=", &value))
        {
            if (seen_dplane || value == nullptr || *value == '\0' ||
                strcmp(value, "-") == 0)
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            sockaddr_in addr = {};
            if (!mplane_parse_ipv4_port(value, &addr))
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            out->have_dplane = true;
            out->dplane = addr;
            seen_dplane = true;
            continue;
        }
        if (mplane_parse_kv(a, "fcs=", &value))
        {
            if (seen_fcs || value == nullptr || *value == '\0')
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            RadioFcs fcs = RadioFcs::auto_detect;
            if (!console_parse_radio_fcs(value, &fcs))
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            out->have_fcs = true;
            out->fcs = fcs;
            seen_fcs = true;
            continue;
        }
        if (err != nullptr)
        {
            *err = k_einval;
        }
        return false;
    }
    if (!have_id)
    {
        if (err != nullptr)
        {
            *err = k_einval;
        }
        return false;
    }
    return true;
}

std::string console_format_radio_device(uint8_t id, const RadioDevice& device)
{
    std::string s = "radio_device id=" + std::to_string(id);
    s += " mplane=";
    s += mplane_format_ipv4_port(device.mplane);
    s += " dplane=";
    s += mplane_format_ipv4_port(device.dplane);
    s += " fcs=";
    s += console_format_radio_fcs(device.fcs);
    return s;
}

}  // namespace winject::core
