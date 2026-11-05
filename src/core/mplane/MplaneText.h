#ifndef WINJECT_CORE_MPLANE_MPLANE_TEXT_H_
#define WINJECT_CORE_MPLANE_MPLANE_TEXT_H_

#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <sys/socket.h>

namespace winject::core
{

// key=value token helpers shared by the radio m-plane client and the l3
// console. Keys match case-insensitively.
bool mplane_parse_kv(const char* text, const char* key, const char** value);
bool mplane_parse_u(const char* text, unsigned long* out);
// Signed decimal (tx_power in dBm can be negative).
bool mplane_parse_i(const char* text, long* out);
// Parses 0..255; rejects values > UINT8_MAX.
bool mplane_parse_u8(const char* text, uint8_t* out);
bool mplane_parse_ipv4_port(const char* text, sockaddr_in* out);
std::string mplane_format_ipv4_port(const sockaddr_in& addr);

}  // namespace winject::core

#endif  // WINJECT_CORE_MPLANE_MPLANE_TEXT_H_
