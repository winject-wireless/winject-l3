#include "mplane/MplaneText.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace winject
{

bool mplane_parse_kv(const char* text, const char* key, const char** value)
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

bool mplane_parse_u(const char* text, unsigned long* out)
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

bool mplane_parse_i(const char* text, long* out)
{
    if (text == nullptr || out == nullptr || *text == '\0')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long v = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
    {
        return false;
    }
    *out = v;
    return true;
}

bool mplane_parse_u8(const char* text, uint8_t* out)
{
    unsigned long v = 0;
    if (!mplane_parse_u(text, &v) || v > 255u)
    {
        return false;
    }
    *out = static_cast<uint8_t>(v);
    return true;
}

bool mplane_parse_ipv4_port(const char* text, sockaddr_in* out)
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

std::string mplane_format_ipv4_port(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ':' + std::to_string(ntohs(addr.sin_port));
}

}  // namespace winject
