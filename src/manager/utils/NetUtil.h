#ifndef WINJECT_MANAGER_NET_UTIL_H_
#define WINJECT_MANAGER_NET_UTIL_H_

#include "frames/LCSequence.h"

#include <bfc/socket.hpp>
#include <netinet/in.h>
#include <stdint.h>
#include <string>

namespace winject
{

constexpr size_t k_wifi_payload_max = 1476;
constexpr size_t k_lc_sequence_len = LCSequence::k_len;
constexpr size_t k_stream_payload_max = k_wifi_payload_max - k_lc_sequence_len;
static_assert(k_stream_payload_max + k_lc_sequence_len == k_wifi_payload_max,
              "LCSequence must fit in one wifi payload");

bool parse_host_port(const std::string& text, sockaddr_in* out);
bool parse_host(const std::string& text, in_addr* out);
bool parse_bus(const std::string& text, uint8_t* bus);
bool parse_domain(const std::string& text, uint16_t* domain);
std::string bus_to_string(uint8_t bus);
std::string domain_to_string(uint16_t domain);
std::string ipv4_to_string(in_addr addr);

}  // namespace winject

#endif  // WINJECT_MANAGER_NET_UTIL_H_
