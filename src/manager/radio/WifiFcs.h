#ifndef WINJECT_MANAGER_RADIO_WIFI_FCS_H_
#define WINJECT_MANAGER_RADIO_WIFI_FCS_H_

#include <stddef.h>
#include <stdint.h>

namespace winject
{

// 802.11 FCS: CRC-32 over the MPDU, 4-byte trailer LSB first on the wire.
uint32_t wifi_fcs_compute(const uint8_t* mpdu, size_t len);
// frame = MPDU followed by its 4-byte FCS; len includes the FCS.
bool wifi_fcs_matches(const uint8_t* frame, size_t len);
void wifi_fcs_store(const uint8_t* mpdu, size_t len, uint8_t out[4]);

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_WIFI_FCS_H_
