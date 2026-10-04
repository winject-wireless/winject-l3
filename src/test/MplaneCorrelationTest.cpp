#include "console/MplaneCorrelation.h"

#include <gtest/gtest.h>

using namespace winject;

TEST(MplaneCorrelationTest, FormatCorrelatedReply)
{
    EXPECT_EQ(format_correlated_reply(7, "OK"), "OK:7");
    EXPECT_EQ(format_correlated_reply(7, "OK\n"), "OK:7\n");
    EXPECT_EQ(format_correlated_reply(7, "OK upstream id=1"),
              "OK:7 upstream id=1");
    EXPECT_EQ(format_correlated_reply(7, "NOK EINVAL"), "NOK:7 EINVAL");
    EXPECT_EQ(format_correlated_reply(7, "pong"), "OK:7 pong");
    EXPECT_EQ(format_correlated_reply(0, "pong"), "OK:0 pong");
    EXPECT_EQ(format_correlated_reply(255, "pong"), "OK:255 pong");
    const std::string multi = "N=2 T=1\nupstream id=1\n";
    const std::string tagged = format_correlated_reply(7, multi);
    EXPECT_EQ(tagged.rfind("OK:7 N=2 T=1", 0), 0u);
    EXPECT_NE(tagged.find("\nupstream id=1"), std::string::npos);
}

TEST(MplaneCorrelationTest, ParseMplaneCmd)
{
    uint8_t id = 0;
    const char* body = nullptr;
    bool malformed = false;
    EXPECT_FALSE(parse_mplane_cmd("ping", &id, &body, &malformed));
    EXPECT_FALSE(malformed);
    EXPECT_STREQ(body, "ping");

    EXPECT_TRUE(parse_mplane_cmd("cmd:7 ping", &id, &body, &malformed));
    EXPECT_FALSE(malformed);
    EXPECT_EQ(id, 7);
    EXPECT_STREQ(body, "ping");

    EXPECT_TRUE(parse_mplane_cmd("cmd:256 x", &id, &body, &malformed));
    EXPECT_TRUE(malformed);

    EXPECT_TRUE(parse_mplane_cmd("cmd:5", &id, &body, &malformed));
    EXPECT_TRUE(malformed);

    EXPECT_TRUE(parse_mplane_cmd("cmd:0 reset", &id, &body, &malformed));
    EXPECT_FALSE(malformed);
    EXPECT_EQ(id, 0);
    EXPECT_STREQ(body, "reset");
}

TEST(MplaneCorrelationTest, RoundTripWithFormatMplaneCmd)
{
    const std::string wire = format_mplane_cmd(9, "radio_stats");
    uint8_t id = 0;
    const char* body = nullptr;
    bool malformed = false;
    EXPECT_TRUE(parse_mplane_cmd(wire.c_str(), &id, &body, &malformed));
    EXPECT_FALSE(malformed);
    EXPECT_EQ(id, 9);
    EXPECT_STREQ(body, "radio_stats\n");
}
