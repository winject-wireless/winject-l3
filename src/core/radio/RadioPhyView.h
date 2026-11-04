#ifndef WINJECT_CORE_RADIO_RADIO_PHY_VIEW_H_
#define WINJECT_CORE_RADIO_RADIO_PHY_VIEW_H_

#include <stdint.h>
#include <string>

namespace winject::core
{

// The PHY a radio reports (radio_info / radio_tx reply).
struct RadioPhyView
{
    uint16_t channel = 0;
    int tx_power = 0;
    std::string modulation;
    int8_t rssi = 0;
    bool rssi_valid = false;
    bool cca = false;
    bool cca_valid = false;
};

}  // namespace winject::core

#endif  // WINJECT_CORE_RADIO_RADIO_PHY_VIEW_H_
