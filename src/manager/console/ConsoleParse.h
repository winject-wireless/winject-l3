#ifndef WINJECT_MANAGER_CONSOLE_PARSE_H_
#define WINJECT_MANAGER_CONSOLE_PARSE_H_

#include "Config.h"

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
// manager.md response form: "none" or "block(k,n)" (no fec= prefix).
bool console_format_fec_display(FecType type, int k, int n, char* out,
                                size_t out_len);
bool console_parse_duration_ms(const char* text, int* out_ms);
bool console_parse_id_list(const char* text, std::vector<uint8_t>* out);
bool console_parse_string_list(const char* text, std::vector<std::string>* out);
void console_trim_line(char* line);
bool console_cmd_is(const char* cmd, const char* full, const char* abbrev,
                    const char* abbrev2 = nullptr);

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_PARSE_H_
