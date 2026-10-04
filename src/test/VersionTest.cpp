#include "utils/Version.h"
#include "WinjectBuildVersion.h"

#include <gtest/gtest.h>

using namespace winject;

TEST(VersionTest, ParseOk)
{
    WinjectVersion v;
    EXPECT_TRUE(parse_winject_version("v1.0.3", &v));
    EXPECT_EQ(v.x, 1);
    EXPECT_EQ(v.y, 0);
    EXPECT_EQ(v.z, 3);
}

TEST(VersionTest, ParseRejectsBad)
{
    WinjectVersion v;
    EXPECT_FALSE(parse_winject_version("1.0.3", &v));
    EXPECT_FALSE(parse_winject_version("v1.0", &v));
    EXPECT_FALSE(parse_winject_version("v1.0.3x", &v));
    EXPECT_FALSE(parse_winject_version("v256.0.0", &v));
}

TEST(VersionTest, ProtocolCompatible)
{
    WinjectVersion a{1, 0, 0};
    WinjectVersion b{1, 0, 99};
    WinjectVersion c{2, 0, 0};
    EXPECT_TRUE(protocol_compatible(a, b));
    EXPECT_FALSE(protocol_compatible(a, c));
}

TEST(VersionTest, OwnVersion)
{
    const WinjectVersion v = own_version();
    EXPECT_EQ(v.x, WINJECT_VERSION_MAJOR);
    EXPECT_EQ(v.y, WINJECT_VERSION_MINOR);
    EXPECT_EQ(v.z, WINJECT_VERSION_PATCH);
}
