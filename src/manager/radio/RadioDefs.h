#ifndef WINJECT_MANAGER_RADIO_DEFS_H_
#define WINJECT_MANAGER_RADIO_DEFS_H_

#include <stddef.h>
#include <stdint.h>

// Raw 802.11 data MPDU limits (radio UDP data plane = full frame bytes).
#define WIFI_HDR_LEN 24
// Largest MPDU per UDP inject datagram: one unfragmented IPv4/UDP packet on a
// 1500-byte MTU link (1500 - 20 IP - 8 UDP). Radios do not reassemble
// fragments.
#define WIFI_RADIO_INJECT_MAX 1472
// Largest MPDU accepted from a radio forward path (802.11 limit used today).
#define WIFI_RADIO_RX_MAX 1500
#define WIFI_PAYLOAD_MAX (WIFI_RADIO_INJECT_MAX - WIFI_HDR_LEN)
#define WIFI_RX_PAYLOAD_MAX (WIFI_RADIO_RX_MAX - WIFI_HDR_LEN)
#define WIFI_PDU_SLOTS 5

static_assert(WIFI_RADIO_INJECT_MAX + 28 <= 1500,
              "inject MPDU must fit one IPv4/UDP datagram");

#define WIFI_BSSID_PREFIX 0xCA, 0xFE, 0xBA, 0xBE

namespace winject
{

enum class RadioFcsMode : uint8_t
{
    unknown = 0,
    signal = 1,
    actual = 2,
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_DEFS_H_
