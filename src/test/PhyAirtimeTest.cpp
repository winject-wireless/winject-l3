#include "radio/PhyAirtime.h"

#include <gtest/gtest.h>

using winject::phy_mode_from_name;
using winject::phy_txtime_us;
using winject::PhyMode;

namespace
{
PhyMode mode(const char* name, uint8_t channel = 6)
{
    PhyMode m;
    EXPECT_TRUE(phy_mode_from_name(name, channel, &m));
    return m;
}
}  // namespace

TEST(PhyAirtimeTest, SpotValuesFromReviewTable)
{
    EXPECT_EQ(phy_txtime_us(mode("DSS_1M_L"), 1504), 12224u);
    EXPECT_EQ(phy_txtime_us(mode("CCK_11M_S"), 1504), 1190u);
    EXPECT_EQ(phy_txtime_us(mode("OFDM_6M"), 1504), 2038u);
    EXPECT_EQ(phy_txtime_us(mode("OFDM_54M"), 1504), 250u);
    EXPECT_EQ(phy_txtime_us(mode("OFDM_MCS7_LGI"), 1504), 230u);
    EXPECT_EQ(phy_txtime_us(mode("OFDM_MCS7_SGI"), 1504), 214u);
}

TEST(PhyAirtimeTest, FiveGhzOmitsSignalExtension)
{
    PhyMode m24;
    PhyMode m5;
    ASSERT_TRUE(phy_mode_from_name("OFDM_24M", 6, &m24));
    ASSERT_TRUE(phy_mode_from_name("OFDM_24M", 36, &m5));
    EXPECT_EQ(phy_txtime_us(m24, 1504) - phy_txtime_us(m5, 1504), 6u);
}

TEST(PhyAirtimeTest, EveryTableNameResolves)
{
    const char* names[] = {
        "DSS_1M_L",      "DSS_2M_S",      "DSS_2M_L", "CCK_5M_L", "CCK_5M_S",
        "CCK_11M_L",     "CCK_11M_S",     "OFDM_6M",  "OFDM_9M",  "OFDM_12M",
        "OFDM_18M",      "OFDM_24M",      "OFDM_36M", "OFDM_48M", "OFDM_54M",
        "OFDM_MCS0_LGI", "OFDM_MCS7_SGI",
    };
    for (const char* n : names)
    {
        PhyMode m;
        EXPECT_TRUE(phy_mode_from_name(n, 1, &m));
        EXPECT_GT(phy_txtime_us(m, 1504), 0u);
    }
}
