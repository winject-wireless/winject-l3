#ifndef WINJECT_MANAGER_CONSOLE_PARSE_H_
#define WINJECT_MANAGER_CONSOLE_PARSE_H_

#include "Config.h"
#include "console/ManagerConsoleTypes.h"

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace winject
{

bool console_parse_kv(const char* text, const char* key, const char** value);
bool console_parse_u(const char* text, unsigned long* out);
// Parses 0..255; rejects values > UINT8_MAX.
bool console_parse_u8(const char* text, uint8_t* out);
bool console_parse_fec_type(const char* text, FecType* out);
// mplane.md response form: "none" or "block(k,n)" (no fec= prefix).
bool console_format_fec_display(FecType type, int k, int n, char* out,
                                size_t out_len);
bool console_parse_duration_ms(const char* text, int* out_ms);
bool console_parse_id_list(const char* text, std::vector<uint8_t>* out);
bool console_parse_string_list(const char* text, std::vector<std::string>* out);
void console_trim_line(char* line);
bool console_cmd_is(const char* cmd, const char* full, const char* abbrev,
                    const char* abbrev2 = nullptr);

bool console_parse_ipv4_port(const char* text, sockaddr_in* out);
std::string console_format_ipv4_port(const sockaddr_in& addr);
bool console_parse_radio_fcs(const char* text, RadioFcsConfig* out);
const char* console_format_radio_fcs(RadioFcsConfig fcs);
bool console_parse_radio_device_args(char* save, ManagerRadioDeviceUpdate* out,
                                     std::string* err);
std::string console_format_radio_device(const ManagerRadioDeviceView& view);

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_PARSE_H_
