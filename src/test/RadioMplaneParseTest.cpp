#include "mplane/RadioMplaneClient.h"
#include "radio/RadioMplaneParse.h"

#include <gtest/gtest.h>

using namespace winject::core;

TEST(RadioMplaneParseTest, ParseInfoBody)
{
    const std::string body =
        "radio_tx channel=6 tx_power=18 modulation=OFDM_24M\n"
        "radio_rx rssi=-42\n";
    RadioPhyView view;
    EXPECT_TRUE(parse_radio_info_body(body, &view));
    EXPECT_EQ(view.channel, 6u);
    EXPECT_EQ(view.tx_power, 18);
    EXPECT_EQ(view.modulation, "OFDM_24M");
    EXPECT_TRUE(view.rssi_valid);
    EXPECT_EQ(view.rssi, -42);
}

TEST(RadioMplaneParseTest, ParseCca)
{
    RadioPhyView view;
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
    RadioPhyView view;
    view.channel = 6;
    view.tx_power = 18;
    view.modulation = "OFDM_24M";
    EXPECT_TRUE(radio_phy_matches_desired(view, 6, 18, "OFDM_24M"));
    EXPECT_FALSE(radio_phy_matches_desired(view, 7, 18, "OFDM_24M"));
}

TEST(RadioMplaneParseTest, ParseNegativePowerAndAppliedKey)
{
    RadioPhyView view;
    EXPECT_TRUE(parse_radio_tx_line(
        "radio_tx channel=1 tx_power=-10 modulation=OFDM_24M cca=true tx_power_applied=-9",
        &view));
    EXPECT_EQ(view.tx_power, -10);
    EXPECT_EQ(view.modulation, "OFDM_24M");
}

TEST(RadioMplaneParseTest, CountersNeedEveryKey)
{
    const std::vector<std::string> ok = {
        "tx_info tx_queue_sz=3 in_flight=2 dropped_invalid_frame=4294967295 "
        "dropped_tx_queue=0 dropped_wifi=1 ether_pkt=100 air_pkt=99 "
        "ts=123456789",
        "rx_info rx_queue_sz=1 dropped_filter_mismatched=14 dropped_rx_queue=0 "
        "dropped_no_peer=2 dropped_send_failed=0 ether_pkt=7 air_pkt=8 ts=5"};
    winject::core::RadioCounters c;
    ASSERT_TRUE(winject::core::parse_radio_counters(ok, &c));
    EXPECT_EQ(c.tx.dropped_invalid_frame, 4294967295u);
    EXPECT_EQ(c.tx.timestamp_us, 123456789u);
    EXPECT_EQ(c.rx.dropped_filter_mismatched, 14u);
    EXPECT_EQ(c.rx.air_packets, 8u);

    // Unknown keys are ignored.
    std::vector<std::string> extra = ok;
    extra[0] += " new_counter=1";
    EXPECT_TRUE(winject::core::parse_radio_counters(extra, &c));

    std::vector<std::string> missing = ok;
    missing[1] = "rx_info rx_queue_sz=1 ether_pkt=7 air_pkt=8 ts=5";
    EXPECT_FALSE(winject::core::parse_radio_counters(missing, &c));
    std::vector<std::string> swapped = {ok[1], ok[0]};
    EXPECT_FALSE(winject::core::parse_radio_counters(swapped, &c));
    std::vector<std::string> bad = ok;
    bad[0].replace(bad[0].find("in_flight=2"), 11, "in_flight=x");
    EXPECT_FALSE(winject::core::parse_radio_counters(bad, &c));
    EXPECT_FALSE(winject::core::parse_radio_counters({ok[0]}, &c));
}
