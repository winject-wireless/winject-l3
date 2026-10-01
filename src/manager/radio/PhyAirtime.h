#ifndef WINJECT_MANAGER_RADIO_PHY_AIRTIME_H_
#define WINJECT_MANAGER_RADIO_PHY_AIRTIME_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace winject
{

enum class PhyFamily : uint8_t
{
    dsss,
    ofdm,
    ht,
};

struct PhyMode
{
    PhyFamily family = PhyFamily::ofdm;
    uint32_t rate_kbps = 0;
    uint16_t n_dbps = 0;
    bool short_preamble = false;
    bool short_gi = false;
    bool band_2g4 = true;
};

bool phy_mode_from_name(const std::string& modulation, uint8_t channel,
                        PhyMode* out);
uint32_t phy_nominal_kbps(const std::string& modulation);
std::string phy_canonical_name(const std::string& modulation);
uint32_t phy_txtime_us(const PhyMode& m, size_t psdu_bytes);
uint32_t phy_default_gap_us(const PhyMode& m);

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_PHY_AIRTIME_H_
