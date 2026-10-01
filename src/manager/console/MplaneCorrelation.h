#ifndef WINJECT_MANAGER_CONSOLE_MPLANE_CORRELATION_H_
#define WINJECT_MANAGER_CONSOLE_MPLANE_CORRELATION_H_

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace winject
{

// cmd:<u8> <line> on TX; OK:<u8> / NOK:<u8> on RX (see radio mplane.md).
std::string format_mplane_cmd(uint8_t req_id, const std::string& line);

bool parse_correlated_reply(const std::string& line, uint8_t* req_id, bool* ok,
                            std::string* payload);

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_MPLANE_CORRELATION_H_
