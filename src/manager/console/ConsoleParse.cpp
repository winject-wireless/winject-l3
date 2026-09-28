#include "console/ConsoleParse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
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
    if (strcmp(text, "RS_BLOCK_ERASURE") == 0)
    {
        *out = FecType::RsBlockErasure;
        return true;
    }
    return false;
}

const char* console_fec_type_name(FecType type)
{
    switch (type)
    {
        case FecType::none:
            return "NONE";
        case FecType::RsBlockErasure:
            return "RS_BLOCK_ERASURE";
    }
    return "NONE";
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

bool console_cmd_is(const char* cmd, const char* full, const char* abbrev)
{
    return cmd != nullptr && ((full != nullptr && strcmp(cmd, full) == 0) ||
                              (abbrev != nullptr && strcmp(cmd, abbrev) == 0));
}

}  // namespace winject
