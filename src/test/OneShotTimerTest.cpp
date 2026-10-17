#include "ReactorTestHelpers.h"
#include "utils/OneShotTimer.h"

#include <chrono>
#include <gtest/gtest.h>
#include <vector>

namespace
{
using winject::IOReactor;
using winject::OneShotTimer;
using winject::test::run_reactor_with_watchdog;

TEST(OneShotTimerTest, FiresOnce)
{
    IOReactor reactor;
    OneShotTimer timer;
    timer.bind(&reactor);
    int fired = 0;
    timer.arm(std::chrono::milliseconds(1),
              [&]()
              {
                  fired++;
              });
    EXPECT_TRUE(timer.armed());
    ASSERT_TRUE(run_reactor_with_watchdog(reactor,
                                          [&]()
                                          {
                                              reactor.stop();
                                          }));
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(timer.armed());
}

TEST(OneShotTimerTest, ArmReplacesPrevious)
{
    IOReactor reactor;
    OneShotTimer timer;
    timer.bind(&reactor);
    std::vector<int> fired;
    timer.arm(std::chrono::milliseconds(1),
              [&]()
              {
                  fired.push_back(1);
              });
    timer.arm(std::chrono::milliseconds(2),
              [&]()
              {
                  fired.push_back(2);
              });
    ASSERT_TRUE(run_reactor_with_watchdog(reactor,
                                          [&]()
                                          {
                                              reactor.stop();
                                          }));
    EXPECT_EQ(fired, std::vector<int>{2});
}

// bfc extracts every due timer before running any; cancelling one from an
// earlier callback in the same pass must still keep it from running.
TEST(OneShotTimerTest, CancelWhileDueDoesNotFire)
{
    IOReactor reactor;
    OneShotTimer timer;
    timer.bind(&reactor);
    bool fired = false;
    reactor.get_timer().wait_us(0,
                                [&]()
                                {
                                    timer.cancel();
                                });
    timer.arm(std::chrono::microseconds(0),
              [&]()
              {
                  fired = true;
              });
    ASSERT_TRUE(run_reactor_with_watchdog(reactor,
                                          [&]()
                                          {
                                              reactor.stop();
                                          }));
    EXPECT_FALSE(fired);
}

TEST(OneShotTimerTest, UnboundDoesNothing)
{
    OneShotTimer timer;
    timer.arm(std::chrono::milliseconds(1), []() {});
    EXPECT_FALSE(timer.armed());
}

}  // namespace
