#include <bfcext/epoll_reactor.hpp>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

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

namespace
{
using test_reactor_t = bfcext::epoll_reactor<std::function<void()>>;

int make_readable_eventfd()
{
    const int fd = eventfd(0, EFD_NONBLOCK);
    uint64_t one = 1;
    auto res [[maybe_unused]] = write(fd, &one, sizeof(one));
    return fd;
}
}  // namespace

// Two fds become readable in the same epoll batch. Whichever callback runs
// first removes and closes the other one (as App::console_remove_upstream
// does with an upstream socket). The removed fd's callback must not run
// afterwards: its owner may already be destroyed.
TEST(EpollReactorTest, RemReadFromCallbackSuppressesPendingEvent)
{
    test_reactor_t reactor;
    int fds[2] = {make_readable_eventfd(), make_readable_eventfd()};
    ASSERT_GE(fds[0], 0);
    ASSERT_GE(fds[1], 0);
    bool removed[2] = {false, false};
    int calls_after_removal = 0;
    int calls = 0;

    for (int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(reactor.add_read_rdy(
            fds[i],
            [&, i]()
            {
                if (removed[i])
                {
                    ++calls_after_removal;
                    return;
                }
                ++calls;
                const int other = 1 - i;
                if (!removed[other])
                {
                    reactor.rem_read_rdy(fds[other]);
                    close(fds[other]);
                    removed[other] = true;
                }
                uint64_t v;
                auto res [[maybe_unused]] = read(fds[i], &v, sizeof(v));
            }));
    }
    reactor.get_timer().wait_ms(20,
                                [&]()
                                {
                                    reactor.stop();
                                });
    reactor.run();

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(calls_after_removal, 0);
    for (int i = 0; i < 2; ++i)
    {
        if (!removed[i])
        {
            reactor.rem_read_rdy(fds[i]);
            close(fds[i]);
        }
    }
}

// Removing an fd, closing it and registering a new fd that reuses the same
// number from one callback must leave the new registration armed.
TEST(EpollReactorTest, RemThenAddSameFdNumberStaysArmed)
{
    test_reactor_t reactor;
    int trigger = make_readable_eventfd();
    int victim = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(trigger, 0);
    ASSERT_GE(victim, 0);
    ASSERT_TRUE(reactor.add_read_rdy(victim, []() {}));

    int replacement = -1;
    bool replacement_fired = false;
    ASSERT_TRUE(reactor.add_read_rdy(
        trigger,
        [&]()
        {
            uint64_t v;
            auto res [[maybe_unused]] = read(trigger, &v, sizeof(v));
            if (replacement != -1)
            {
                return;
            }
            reactor.rem_read_rdy(victim);
            close(victim);
            replacement = eventfd(0, EFD_NONBLOCK);
            ASSERT_EQ(replacement, victim);
            ASSERT_TRUE(reactor.add_read_rdy(replacement,
                                             [&]()
                                             {
                                                 uint64_t r;
                                                 if (read(replacement, &r,
                                                          sizeof(r)) > 0)
                                                 {
                                                     replacement_fired = true;
                                                     reactor.stop();
                                                 }
                                             }));
            // Make the replacement readable only after the reactor has run
            // its deferred work for this iteration.
            reactor.get_timer().wait_ms(5,
                                        [&]()
                                        {
                                            uint64_t one = 1;
                                            auto w [[maybe_unused]] = write(
                                                replacement, &one, sizeof(one));
                                        });
        }));
    reactor.get_timer().wait_ms(200,
                                [&]()
                                {
                                    reactor.stop();
                                });
    reactor.run();

    EXPECT_TRUE(replacement_fired);
    reactor.rem_read_rdy(trigger);
    close(trigger);
    if (replacement != -1)
    {
        reactor.rem_read_rdy(replacement);
        close(replacement);
    }
}
