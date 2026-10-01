#ifndef WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_
#define WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_

#include "console/ManagerConsoleTypes.h"

#include <string>

namespace winject
{

bool parse_radio_tx_line(const std::string& line, ManagerRadioView* out);
bool parse_radio_info_body(const std::string& body, ManagerRadioView* out);

bool radio_phy_matches_desired(const ManagerRadioView& actual,
                               uint8_t channel, int8_t power_dbm,
                               const std::string& modulation);

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_
