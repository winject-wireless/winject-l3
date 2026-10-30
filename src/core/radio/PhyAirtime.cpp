#include "radio/PhyAirtime.h"

#include <cctype>
#include <string>

namespace winject
{

namespace
{

struct PhyTableEntry
{
    const char* name;
    PhyFamily family;
    uint32_t rate_kbps;
    uint16_t n_dbps;
    bool short_preamble;
    bool short_gi;
};

static constexpr PhyTableEntry k_phy_table[] = {
    {"DSS_1M_L", PhyFamily::dsss, 1000, 0, false, false},
    {"DSS_2M_S", PhyFamily::dsss, 2000, 0, true, false},
    {"DSS_2M_L", PhyFamily::dsss, 2000, 0, false, false},
    {"CCK_5M_L", PhyFamily::dsss, 5500, 0, false, false},
    {"CCK_5M_S", PhyFamily::dsss, 5500, 0, true, false},
    {"CCK_11M_L", PhyFamily::dsss, 11000, 0, false, false},
    {"CCK_11M_S", PhyFamily::dsss, 11000, 0, true, false},
    {"OFDM_6M", PhyFamily::ofdm, 6000, 24, false, false},
    {"OFDM_9M", PhyFamily::ofdm, 9000, 36, false, false},
    {"OFDM_12M", PhyFamily::ofdm, 12000, 48, false, false},
    {"OFDM_18M", PhyFamily::ofdm, 18000, 72, false, false},
    {"OFDM_24M", PhyFamily::ofdm, 24000, 96, false, false},
    {"OFDM_36M", PhyFamily::ofdm, 36000, 144, false, false},
    {"OFDM_48M", PhyFamily::ofdm, 48000, 192, false, false},
    {"OFDM_54M", PhyFamily::ofdm, 54000, 216, false, false},
    {"OFDM_MCS0_LGI", PhyFamily::ht, 6500, 26, false, false},
    {"OFDM_MCS1_LGI", PhyFamily::ht, 13000, 52, false, false},
    {"OFDM_MCS2_LGI", PhyFamily::ht, 19500, 78, false, false},
    {"OFDM_MCS3_LGI", PhyFamily::ht, 26000, 104, false, false},
    {"OFDM_MCS4_LGI", PhyFamily::ht, 39000, 156, false, false},
    {"OFDM_MCS5_LGI", PhyFamily::ht, 52000, 208, false, false},
    {"OFDM_MCS6_LGI", PhyFamily::ht, 58500, 234, false, false},
    {"OFDM_MCS7_LGI", PhyFamily::ht, 65000, 260, false, false},
    {"OFDM_MCS0_SGI", PhyFamily::ht, 7200, 26, false, true},
    {"OFDM_MCS1_SGI", PhyFamily::ht, 14400, 52, false, true},
    {"OFDM_MCS2_SGI", PhyFamily::ht, 21700, 78, false, true},
    {"OFDM_MCS3_SGI", PhyFamily::ht, 28900, 104, false, true},
    {"OFDM_MCS4_SGI", PhyFamily::ht, 43300, 156, false, true},
    {"OFDM_MCS5_SGI", PhyFamily::ht, 57800, 208, false, true},
    {"OFDM_MCS6_SGI", PhyFamily::ht, 65000, 234, false, true},
    {"OFDM_MCS7_SGI", PhyFamily::ht, 72200, 260, false, true},
};

std::string to_upper(const std::string& modulation)
{
    std::string upper;
    upper.reserve(modulation.size());
    for (char c : modulation)
    {
        upper.push_back(
            static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return upper;
}

const PhyTableEntry* find_entry(const std::string& modulation)
{
    const std::string upper = to_upper(modulation);
    for (const auto& e : k_phy_table)
    {
        if (upper == e.name)
        {
            return &e;
        }
    }
    return nullptr;
}

bool is_band_2g4(uint8_t channel)
{
    return channel > 0 && channel <= 14;
}

uint32_t ceil_div_u64(uint64_t num, uint64_t den)
{
    return static_cast<uint32_t>((num + den - 1) / den);
}

}  // namespace

bool phy_mode_from_name(const std::string& modulation, uint8_t channel,
                        PhyMode* out)
{
    if (out == nullptr)
    {
        return false;
    }
    const PhyTableEntry* e = find_entry(modulation);
    if (e == nullptr)
    {
        return false;
    }
    out->family = e->family;
    out->rate_kbps = e->rate_kbps;
    out->n_dbps = e->n_dbps;
    out->short_preamble = e->short_preamble;
    out->short_gi = e->short_gi;
    out->band_2g4 = is_band_2g4(channel);
    return true;
}

uint32_t phy_nominal_kbps(const std::string& modulation)
{
    const PhyTableEntry* e = find_entry(modulation);
    return e != nullptr ? e->rate_kbps : 0;
}

std::string phy_canonical_name(const std::string& modulation)
{
    const PhyTableEntry* e = find_entry(modulation);
    return e != nullptr ? e->name : "";
}

uint32_t phy_txtime_us(const PhyMode& m, size_t psdu_bytes)
{
    const uint64_t l = psdu_bytes;
    if (m.family == PhyFamily::dsss)
    {
        const uint32_t preamble = m.short_preamble ? 96u : 192u;
        const uint32_t payload =
            ceil_div_u64(8ULL * l * 1000ULL, m.rate_kbps);
        return preamble + payload;
    }
    const uint32_t n_sym =
        ceil_div_u64(16ULL + 8ULL * l + 6ULL, m.n_dbps);
    const uint32_t se = m.band_2g4 ? 6u : 0u;
    if (m.family == PhyFamily::ofdm)
    {
        return 20u + 4u * n_sym + se;
    }
    if (m.short_gi)
    {
        const uint32_t sym_us = 4u * ceil_div_u64(9ULL * n_sym, 10ULL);
        return 36u + sym_us + se;
    }
    return 36u + 4u * n_sym + se;
}

uint32_t phy_default_gap_us(const PhyMode& m)
{
    if (m.family == PhyFamily::dsss)
    {
        return 360u;
    }
    return m.band_2g4 ? 96u : 102u;
}

}  // namespace winject
