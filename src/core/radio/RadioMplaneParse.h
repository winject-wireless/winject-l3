#ifndef WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_
#define WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_

#include "mplane/MplaneResult.h"
#include "radio/RadioDefs.h"
#include "radio/RadioPhyView.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace winject::core
{

// tx_info and rx_info counters (radio_stats). The ESP32, C6 and Realtek
// firmwares print the same keys:
//   tx_info tx_queue_sz= in_flight= dropped_invalid_frame= dropped_tx_queue=
//           dropped_wifi= ether_pkt= air_pkt= ts=
//   rx_info rx_queue_sz= dropped_filter_mismatched= dropped_rx_queue=
//           dropped_no_peer= dropped_send_failed= ether_pkt= air_pkt= ts=
struct RadioTxCounters
{
    uint64_t queue_size = 0;
    uint64_t in_flight = 0;
    uint64_t dropped_invalid_frame = 0;
    uint64_t dropped_queue = 0;
    uint64_t dropped_wifi = 0;
    uint64_t ether_packets = 0;
    uint64_t air_packets = 0;
    uint64_t timestamp_us = 0;
};
struct RadioRxCounters
{
    uint64_t queue_size = 0;
    uint64_t dropped_filter_mismatched = 0;
    uint64_t dropped_queue = 0;
    uint64_t dropped_no_peer = 0;
    uint64_t dropped_send_failed = 0;
    uint64_t ether_packets = 0;
    uint64_t air_packets = 0;
    uint64_t timestamp_us = 0;
};
struct RadioCounters
{
    RadioTxCounters tx;
    RadioRxCounters rx;
};
// Parses one tx_info and one rx_info line, in that order. Every key must be
// present; unknown keys are ignored. False on anything else.
bool parse_radio_counters(const std::vector<std::string>& lines,
                          RadioCounters* out);

bool parse_radio_tx_line(const std::string& line, RadioPhyView* out);
bool parse_radio_info_body(const std::string& body, RadioPhyView* out);

// Parses OK payload: "radio_caps_info fcs=SIGNAL|ACTUAL" (optional "OK "
// prefix).
bool parse_radio_caps_line(const std::string& line, RadioFcsMode* out_mode);

// Maps a radio_caps_info m-plane result to the FCS mode to apply.
// On NOK ENOSYS sets *legacy_enosys and returns actual.
bool resolve_radio_caps_mplane(const MplaneResult& r, RadioFcsMode* out_mode,
                               bool* legacy_enosys, std::string* error);

const char* radio_fcs_mode_name(RadioFcsMode mode);
std::string format_radio_caps_ok_line(RadioFcsMode mode);

bool radio_phy_matches_desired(const RadioPhyView& actual, uint8_t channel,
                               int8_t power_dbm, const std::string& modulation);

}  // namespace winject::core

#endif  // WINJECT_MANAGER_RADIO_RADIO_MPLANE_PARSE_H_
