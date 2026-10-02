#ifndef WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_
#define WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_

#include "console/ConsoleClient.h"
#include "console/ManagerConsoleTypes.h"
#include "radio/RadioDefs.h"

#include <string>

namespace winject
{

bool parse_radio_tx_line(const std::string& line, ManagerRadioView* out);
bool parse_radio_info_body(const std::string& body, ManagerRadioView* out);

// Parses OK payload: "radio_caps_info fcs=SIGNAL|ACTUAL" (optional "OK "
// prefix).
bool parse_radio_caps_line(const std::string& line, RadioFcsMode* out_mode);

// Maps a radio_caps_info m-plane result to the FCS mode to apply.
// On NOK ENOSYS sets *legacy_enosys and returns actual.
bool resolve_radio_caps_mplane(const MplaneResult& r, RadioFcsMode* out_mode,
                               bool* legacy_enosys, std::string* error);

const char* radio_fcs_mode_name(RadioFcsMode mode);
std::string format_radio_caps_ok_line(RadioFcsMode mode);

bool radio_phy_matches_desired(const ManagerRadioView& actual, uint8_t channel,
                               int8_t power_dbm, const std::string& modulation);

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_
