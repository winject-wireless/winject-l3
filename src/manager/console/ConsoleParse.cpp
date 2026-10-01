#include "console/ConsoleParse.h"

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

}  // namespace winject
