#ifndef WINJECT_L3_CONSOLE_CONSOLE_PARSE_H_
#define WINJECT_L3_CONSOLE_CONSOLE_PARSE_H_

#include "mplane/MplaneText.h"
#include "winject_l3/Radio.h"
#include "winject_l3/Upstream.h"

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace winject::core
{

// radio_device arguments: the fields the line sets (docs/mplane.md).
struct RadioDeviceUpdate
{
    uint8_t id = 0;
    bool have_mplane = false;
    sockaddr_in mplane{};
    bool have_dplane = false;
    sockaddr_in dplane{};
    bool have_fcs = false;
    winject::RadioFcs fcs = winject::RadioFcs::auto_detect;
};

bool console_parse_fec_type(const char* text, winject::FecType* out);
// mplane.md response form: "none" or "block(k,n)" (no fec= prefix).
bool console_format_fec_display(winject::FecType type, int k, int n, char* out,
                                size_t out_len);
bool console_parse_duration_ms(const char* text, int* out_ms);
bool console_parse_id_list(const char* text, std::vector<uint8_t>* out);
bool console_parse_string_list(const char* text, std::vector<std::string>* out);
void console_trim_line(char* line);
bool console_cmd_is(const char* cmd, const char* full, const char* abbrev,
                    const char* abbrev2 = nullptr);

bool console_parse_radio_fcs(const char* text, winject::RadioFcs* out);
const char* console_format_radio_fcs(winject::RadioFcs fcs);
bool console_parse_radio_device_args(char* save, RadioDeviceUpdate* out,
                                     std::string* err);
std::string console_format_radio_device(uint8_t id,
                                        const winject::RadioDevice& device);

}  // namespace winject::core

#endif  // WINJECT_L3_CONSOLE_CONSOLE_PARSE_H_
