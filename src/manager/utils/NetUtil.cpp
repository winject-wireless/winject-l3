#include "utils/NetUtil.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace winject
{

bool parse_host(const std::string& text, in_addr* out)
{
    if (out == nullptr || text.empty())
    {
        return false;
    }
    if (inet_pton(AF_INET, text.c_str(), out) == 1)
    {
        return true;
    }
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(text.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
    {
        return false;
    }
    const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    *out = sin->sin_addr;
    freeaddrinfo(res);
    return out->s_addr != 0;
}

bool parse_host_port(const std::string& text, sockaddr_in* out)
{
    if (out == nullptr)
    {
        return false;
    }
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
    {
        return false;
    }
    const std::string host = text.substr(0, colon);
    const std::string port_s = text.substr(colon + 1);
    char* end = nullptr;
    const long port = std::strtol(port_s.c_str(), &end, 10);
    if (end == port_s.c_str() || *end != '\0' || port <= 0 || port > 65535)
    {
        return false;
    }
    in_addr addr = {};
    if (!parse_host(host, &addr))
    {
        return false;
    }
    *out = {};
    out->sin_family = AF_INET;
    out->sin_addr = addr;
    out->sin_port = htons(static_cast<uint16_t>(port));
    return true;
}

bool parse_bus(const std::string& text, uint8_t* bus)
{
    if (bus == nullptr || text.empty())
    {
        return false;
    }
    const char* p = text.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
    }
    const size_t n = strlen(p);
    if (n < 1 || n > 2)
    {
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(p, &end, 16);
    if (end != p + n || v < 0 || v > 255)
    {
        return false;
    }
    *bus = static_cast<uint8_t>(v);
    return true;
}

bool parse_domain(const std::string& text, uint16_t* domain)
{
    if (domain == nullptr || text.empty())
    {
        return false;
    }
    const char* p = text.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 16);
    if (end == p || *end != '\0' || v < 1 || v > 65535)
    {
        return false;
    }
    *domain = static_cast<uint16_t>(v);
    return true;
}

std::string bus_to_string(uint8_t bus)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%x", bus);
    return buf;
}

std::string domain_to_string(uint16_t domain)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%x", domain);
    return buf;
}

std::string ipv4_to_string(in_addr addr)
{
    char host[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr, host, sizeof(host));
    return host;
}

}  // namespace winject
