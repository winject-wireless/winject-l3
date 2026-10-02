#include "console/ConsoleParse.h"

#include <gtest/gtest.h>
#include <string.h>
#include <vector>

using namespace winject;

TEST(ConsoleParseTest, ParseKv)
{
    const char* value = nullptr;
    EXPECT_TRUE(
        console_parse_kv("max_rate_kbps=1000", "max_rate_kbps=", &value));
    ASSERT_NE(value, nullptr);
    EXPECT_STREQ(value, "1000");
    EXPECT_FALSE(console_parse_kv("other=1", "max_rate_kbps=", &value));
}

TEST(ConsoleParseTest, ParseU)
{
    unsigned long v = 0;
    EXPECT_TRUE(console_parse_u("42", &v));
    EXPECT_EQ(v, 42u);
    EXPECT_FALSE(console_parse_u("", &v));
    EXPECT_FALSE(console_parse_u("12abc", &v));
}

TEST(ConsoleParseTest, ParseFecType)
{
    FecType type = FecType::RsBlockErasure;
    EXPECT_TRUE(console_parse_fec_type("NONE", &type));
    EXPECT_EQ(type, FecType::none);
    EXPECT_TRUE(console_parse_fec_type("none", &type));
    EXPECT_EQ(type, FecType::none);
    EXPECT_TRUE(console_parse_fec_type("RS_BLOCK_ERASURE", &type));
    EXPECT_EQ(type, FecType::RsBlockErasure);
    EXPECT_TRUE(console_parse_fec_type("BLOCK", &type));
    EXPECT_EQ(type, FecType::RsBlockErasure);
    EXPECT_FALSE(console_parse_fec_type("bogus", &type));
}

TEST(ConsoleParseTest, ParseU8)
{
    uint8_t b = 0;
    EXPECT_TRUE(console_parse_u8("255", &b));
    EXPECT_EQ(b, 255);
    EXPECT_FALSE(console_parse_u8("256", &b));
}

TEST(ConsoleParseTest, ParseDurationMs)
{
    int ms = 0;
    EXPECT_TRUE(console_parse_duration_ms("50ms", &ms));
    EXPECT_EQ(ms, 50);
    EXPECT_TRUE(console_parse_duration_ms("20", &ms));
    EXPECT_EQ(ms, 20);
    EXPECT_FALSE(console_parse_duration_ms("ms", &ms));
}

TEST(ConsoleParseTest, ParseIdList)
{
    std::vector<uint8_t> ids;
    EXPECT_TRUE(console_parse_id_list("1,2,3", &ids));
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[2], 3);
    EXPECT_TRUE(console_parse_id_list("", &ids));
    EXPECT_TRUE(ids.empty());
    EXPECT_FALSE(console_parse_id_list("1,", &ids));
}

TEST(ConsoleParseTest, ParseStringList)
{
    std::vector<std::string> keys;
    EXPECT_TRUE(console_parse_string_list("rx,rate", &keys));
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "rx");
    EXPECT_EQ(keys[1], "rate");
    EXPECT_TRUE(console_parse_string_list("", &keys));
    EXPECT_TRUE(keys.empty());
    EXPECT_FALSE(console_parse_string_list("a,", &keys));
}

TEST(ConsoleParseTest, TrimLine)
{
    char line[] = "ping\r\n  ";
    console_trim_line(line);
    EXPECT_STREQ(line, "ping");
}

TEST(ConsoleParseTest, CmdIs)
{
    EXPECT_TRUE(console_cmd_is("help", "help", "?"));
    EXPECT_TRUE(console_cmd_is("?", "help", "?"));
    EXPECT_TRUE(console_cmd_is("h", "help", "?", "h"));
    EXPECT_FALSE(console_cmd_is("ping", "help", "?"));
    EXPECT_FALSE(console_cmd_is(nullptr, "help", "?"));
}

TEST(ConsoleParseTest, FecDisplay)
{
    char buf[32];
    EXPECT_TRUE(
        console_format_fec_display(FecType::none, 0, 0, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "none");
    EXPECT_TRUE(console_format_fec_display(FecType::RsBlockErasure, 10, 16, buf,
                                           sizeof(buf)));
    EXPECT_STREQ(buf, "block(10,16)");
}
