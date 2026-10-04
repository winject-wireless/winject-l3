#ifndef WINJECT_MANAGER_CONSOLE_MPLANE_CORRELATION_H_
#define WINJECT_MANAGER_CONSOLE_MPLANE_CORRELATION_H_

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace winject
{

// cmd:<u8> <line> on TX; OK:<u8> / NOK:<u8> on RX (radio m-plane and manager
// m-plane server).
std::string format_mplane_cmd(uint8_t req_id, const std::string& line);

// Returns false if there is no cmd: prefix (*body is the full line). When true,
// *malformed is set if the prefix is present but invalid.
bool parse_mplane_cmd(const char* line, uint8_t* id, const char** body,
                      bool* malformed);

std::string format_correlated_reply(uint8_t id, const std::string& text);

bool parse_correlated_reply(const std::string& line, uint8_t* req_id, bool* ok,
                            std::string* payload);

bool mplane_nok_token(const std::string& err, std::string* token);

std::string mplane_err_string(const std::string& err);

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_MPLANE_CORRELATION_H_
