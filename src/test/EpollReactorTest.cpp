#include <bfcext/epoll_reactor.hpp>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>

TEST(EpollReactorTest, SubMillisecondTimerChain)
{
    bfcext::epoll_reactor<std::function<void()>> reactor;
    const auto t0 = std::chrono::steady_clock::now();
    int fired = 0;
    for (int i = 0; i < 100; ++i)
    {
        const int64_t delay_us = 250 * static_cast<int64_t>(i + 1);
        reactor.get_timer().wait_us(delay_us,
                                    [&]()
                                    {
                                        ++fired;
                                    });
    }
    reactor.get_timer().wait_us(250 * 100 + 500,
                                [&]()
                                {
                                    reactor.stop();
                                });
    reactor.run();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT_EQ(fired, 100);
    EXPECT_LT(elapsed.count(), 60);
}

TEST(EpollReactorTest, WaitMsNotEarly)
{
    bfcext::epoll_reactor<std::function<void()>> reactor;
    const auto t0 = std::chrono::steady_clock::now();
    reactor.get_timer().wait_ms(10,
                                [&]()
                                {
                                    reactor.stop();
                                });
    reactor.run();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT_GE(elapsed.count(), 10);
}
