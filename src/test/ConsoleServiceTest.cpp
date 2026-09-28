#include "console/ConsoleParse.h"

#include <gtest/gtest.h>
#include <string.h>

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
    EXPECT_FALSE(console_parse_fec_type("bogus", &type));
}

TEST(ConsoleParseTest, FecTypeName)
{
    EXPECT_STREQ(console_fec_type_name(FecType::none), "NONE");
    EXPECT_STREQ(console_fec_type_name(FecType::RsBlockErasure),
                 "RS_BLOCK_ERASURE");
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
    EXPECT_FALSE(console_cmd_is("ping", "help", "?"));
    EXPECT_FALSE(console_cmd_is(nullptr, "help", "?"));
}
