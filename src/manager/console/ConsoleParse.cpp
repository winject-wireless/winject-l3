#include "console/ConsoleParse.h"

#include "console/MplaneErrno.h"

#include <arpa/inet.h>
#include <climits>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>

namespace winject
{

bool console_parse_kv(const char* text, const char* key, const char** value)
{
    if (text == nullptr || key == nullptr || value == nullptr)
    {
        return false;
    }
    const size_t n = strlen(key);
    if (strncasecmp(text, key, n) != 0)
    {
        return false;
    }
    *value = text + n;
    return true;
}

bool console_parse_u(const char* text, unsigned long* out)
{
    if (text == nullptr || out == nullptr || *text == '\0')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long v = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
    {
        return false;
    }
    *out = v;
    return true;
}

bool console_parse_u8(const char* text, uint8_t* out)
{
    unsigned long v = 0;
    if (!console_parse_u(text, &v) || v > 255u)
    {
        return false;
    }
    *out = static_cast<uint8_t>(v);
    return true;
}

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
        *out = FecType::RsBlockErasure;
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
        if (!console_parse_u(num, &v) ||
            v > static_cast<unsigned long>(INT_MAX))
        {
            return false;
        }
        *out_ms = static_cast<int>(v);
        return true;
    }
    if (!console_parse_u(text, &v) || v > static_cast<unsigned long>(INT_MAX))
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

bool console_parse_ipv4_port(const char* text, sockaddr_in* out)
{
    if (text == nullptr || out == nullptr || *text == '\0')
    {
        return false;
    }
    const char* colon = strchr(text, ':');
    if (colon == nullptr || colon == text || colon[1] == '\0')
    {
        return false;
    }
    std::string host(text, colon - text);
    const char* port_s = colon + 1;
    char* end = nullptr;
    errno = 0;
    const unsigned long port = strtoul(port_s, &end, 10);
    if (errno != 0 || end == port_s || *end != '\0' || port == 0 ||
        port > 65535)
    {
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        return false;
    }
    addr.sin_port = htons(static_cast<uint16_t>(port));
    *out = addr;
    return true;
}

std::string console_format_ipv4_port(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ':' + std::to_string(ntohs(addr.sin_port));
}

bool console_parse_radio_fcs(const char* text, RadioFcsConfig* out)
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
        *out = RadioFcsConfig::auto_detect;
        return true;
    }
    if (v == "signal")
    {
        *out = RadioFcsConfig::signal;
        return true;
    }
    if (v == "actual")
    {
        *out = RadioFcsConfig::actual;
        return true;
    }
    return false;
}

const char* console_format_radio_fcs(RadioFcsConfig fcs)
{
    switch (fcs)
    {
        case RadioFcsConfig::signal:
            return "SIGNAL";
        case RadioFcsConfig::actual:
            return "ACTUAL";
        case RadioFcsConfig::auto_detect:
        default:
            return "AUTO";
    }
}

bool console_parse_radio_device_args(char* save, ManagerRadioDeviceUpdate* out,
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
        if (console_parse_kv(a, "id=", &value))
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
            if (!console_parse_u8(value, &id))
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
        if (console_parse_kv(a, "mplane=", &value))
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
            if (!console_parse_ipv4_port(value, &addr))
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
        if (console_parse_kv(a, "dplane=", &value))
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
            if (!console_parse_ipv4_port(value, &addr))
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
        if (console_parse_kv(a, "fcs=", &value))
        {
            if (seen_fcs || value == nullptr || *value == '\0')
            {
                if (err != nullptr)
                {
                    *err = k_einval;
                }
                return false;
            }
            RadioFcsConfig fcs = RadioFcsConfig::auto_detect;
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

std::string console_format_radio_device(const ManagerRadioDeviceView& view)
{
    std::string s = "radio_device id=" + std::to_string(view.id);
    s += " mplane=";
    if (view.device.have_mplane)
    {
        s += console_format_ipv4_port(view.device.mplane);
    }
    else
    {
        s += '-';
    }
    s += " dplane=";
    if (view.device.have_dplane)
    {
        s += console_format_ipv4_port(view.device.dplane);
    }
    else
    {
        s += '-';
    }
    s += " fcs=";
    s += console_format_radio_fcs(view.device.fcs);
    return s;
}

}  // namespace winject
