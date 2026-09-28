#ifndef WINJECT_MANAGER_RADIO_DEFS_H_
#define WINJECT_MANAGER_RADIO_DEFS_H_

#include <stddef.h>
#include <stdint.h>

// Raw 802.11 data MPDU limits (radio UDP data plane = full frame bytes).
#define WIFI_HDR_LEN 24
#define WIFI_RADIO_INJECT_MAX 1500
#define WIFI_PAYLOAD_MAX (WIFI_RADIO_INJECT_MAX - WIFI_HDR_LEN)
#define WIFI_PDU_SLOTS 5

#define WIFI_BSSID_PREFIX 0xCA, 0xFE, 0xBA, 0xBE

#endif  // WINJECT_MANAGER_RADIO_DEFS_H_
