#include "console/MplaneCorrelation.h"

#include <gtest/gtest.h>

using namespace winject;

TEST(MplaneErrStringTest, RadioNokPassThrough)
{
    EXPECT_EQ(mplane_err_string("radio_tx -> NOK EINVAL"), "EINVAL");
    EXPECT_EQ(mplane_err_string("x -> nok ETIMEDOUT"), "ETIMEDOUT");
}

TEST(MplaneErrStringTest, LocalErrors)
{
    EXPECT_EQ(mplane_err_string("cancelled"), "ECANCELED");
    EXPECT_EQ(mplane_err_string("timeout"), "ETIMEDOUT");
    EXPECT_EQ(mplane_err_string("ENOSYS"), "ENOSYS");
    EXPECT_EQ(mplane_err_string(""), "EIO");
    EXPECT_EQ(mplane_err_string("send failed"), "EIO");
}
