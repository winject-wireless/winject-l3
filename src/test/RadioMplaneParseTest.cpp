#include "console/ConsoleClient.h"
#include "radio/RadioMplaneParse.h"

#include <gtest/gtest.h>

using namespace winject;

TEST(RadioMplaneParseTest, ParseInfoBody)
{
    const std::string body =
        "radio_tx channel=6 tx_power=18 modulation=OFDM_24M\n"
        "radio_rx rssi=-42\n";
    ManagerRadioView view;
    EXPECT_TRUE(parse_radio_info_body(body, &view));
    EXPECT_EQ(view.channel, 6u);
    EXPECT_EQ(view.tx_power, 18);
    EXPECT_EQ(view.modulation, "OFDM_24M");
    EXPECT_TRUE(view.rssi_valid);
    EXPECT_EQ(view.rssi, -42);
}

TEST(RadioMplaneParseTest, ParseCca)
{
    ManagerRadioView view;
    EXPECT_TRUE(parse_radio_tx_line(
        "radio_tx channel=1 cca=false modulation=DSS_1M_L", &view));
    EXPECT_TRUE(view.cca_valid);
    EXPECT_FALSE(view.cca);
}

TEST(RadioMplaneParseTest, ParseRadioCapsSignalAndActual)
{
    RadioFcsMode mode = RadioFcsMode::unknown;
    EXPECT_TRUE(parse_radio_caps_line("radio_caps_info fcs=SIGNAL", &mode));
    EXPECT_EQ(mode, RadioFcsMode::signal);
    EXPECT_TRUE(parse_radio_caps_line("OK radio_caps_info fcs=ACTUAL", &mode));
    EXPECT_EQ(mode, RadioFcsMode::actual);
}

TEST(RadioMplaneParseTest, ParseRadioCapsExtraKeysIgnored)
{
    RadioFcsMode mode = RadioFcsMode::unknown;
    EXPECT_TRUE(
        parse_radio_caps_line("radio_caps_info fcs=SIGNAL foo=bar", &mode));
    EXPECT_EQ(mode, RadioFcsMode::signal);
}

TEST(RadioMplaneParseTest, ParseRadioCapsRejectsBadFcs)
{
    RadioFcsMode mode = RadioFcsMode::unknown;
    EXPECT_FALSE(parse_radio_caps_line("radio_caps_info fcs=BOGUS", &mode));
}

TEST(RadioMplaneParseTest, ResolveEnosysAssumesActual)
{
    MplaneResult r;
    r.ok = false;
    r.error = "ENOSYS";
    RadioFcsMode mode = RadioFcsMode::unknown;
    bool legacy = false;
    EXPECT_TRUE(resolve_radio_caps_mplane(r, &mode, &legacy, nullptr));
    EXPECT_TRUE(legacy);
    EXPECT_EQ(mode, RadioFcsMode::actual);
}

TEST(RadioMplaneParseTest, PhyMatchesDesired)
{
    ManagerRadioView view;
    view.channel = 6;
    view.tx_power = 18;
    view.modulation = "OFDM_24M";
    EXPECT_TRUE(radio_phy_matches_desired(view, 6, 18, "OFDM_24M"));
    EXPECT_FALSE(radio_phy_matches_desired(view, 7, 18, "OFDM_24M"));
}
