#ifndef WINJECT_MANAGER_CONSOLE_PARSE_H_
#define WINJECT_MANAGER_CONSOLE_PARSE_H_

#include "Config.h"

#include <stddef.h>

namespace winject
{

bool console_parse_kv(const char* text, const char* key, const char** value);
bool console_parse_u(const char* text, unsigned long* out);
bool console_parse_fec_type(const char* text, FecType* out);
const char* console_fec_type_name(FecType type);
void console_trim_line(char* line);
bool console_cmd_is(const char* cmd, const char* full, const char* abbrev);

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_PARSE_H_
