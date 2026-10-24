#include "radio/RadioDefs.h"
#include "radio/RxEvent.h"
#include "radio/WifiFcs.h"
#include "radio/WifiQueue.h"
#include "utils/IOReactor.h"

#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace winject;

namespace
{

using DplaneQueue =
    bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
using DplaneQueueRegistry =
    bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

class CvRun
{
public:
    explicit CvRun(bfc::cv_reactor<>& reactor)
        : reactor(reactor),
          thread(
              [&reactor]()
              {
                  reactor.run();
              })
    {
    }

    ~CvRun()
    {
        reactor.stop();
        if (thread.joinable())
        {
            thread.join();
        }
    }

    CvRun(const CvRun&) = delete;
    CvRun& operator=(const CvRun&) = delete;

private:
    bfc::cv_reactor<>& reactor;
    std::thread thread;
};

bool run_io_brief(IOReactor& reactor)
{
    reactor.get_timer().wait_ms(50,
                                [&]()
                                {
                                    reactor.stop();
                                });
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    std::thread worker(
        [&]()
        {
            reactor.run();
            done.set_value();
        });
    const bool ok =
        finished.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    worker.join();
    return ok;
}

std::vector<uint8_t> minimal_mpdu()
{
    std::vector<uint8_t> mpdu(24, 0);
    return mpdu;
}

}  // namespace

TEST(WifiQueueTest, SignalModeAcceptsZeroTrailer)
{
    auto inject = std::make_shared<DplaneQueue>();
    auto forward = std::make_shared<DplaneQueue>();
    inject->set_capacity(16);
    forward->set_capacity(16);
    auto cv = std::make_shared<bfc::cv_reactor<>>();
    ASSERT_TRUE(DplaneQueueRegistry::get_instance().add("wq-wifi-tx", inject));
    ASSERT_TRUE(DplaneQueueRegistry::get_instance().add("wq-wifi-rx", forward));
    ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add("wq-reactor", cv));

    cv->add_read_rdy(
        *inject,
        [&]()
        {
            for (;;)
            {
                auto pkt = inject->pop();
                if (!pkt)
                {
                    break;
                }
                if (pkt->size() == 1)
                {
                    continue;
                }
                std::vector<uint8_t> frame(pkt->u8(), pkt->u8() + pkt->size());
                frame.resize(frame.size() + 4);
                forward->push(
                    bfc::shared_sized_buffer::copy_from(frame.data(),
                                                         frame.size()));
                cv->wake_up();
            }
        });

    CvRun cv_run(*cv);

    IOReactor io;
    std::vector<uint8_t> received;
    WifiQueue wifi;
    ASSERT_TRUE(wifi.open(io,
                          [&](bfc::shared_sized_buffer buf)
                          {
                              received.assign(buf.u8(), buf.u8() + buf.size());
                          }));
    WifiQueueDplaneBinding bind;
    bind.wifi_tx = "wq-wifi-tx";
    bind.wifi_rx = "wq-wifi-rx";
    bind.reactor = "wq-reactor";
    ASSERT_TRUE(wifi.bind_dplane(bind));
    wifi.set_fcs_mode(RadioFcsMode::signal);
    wifi.post_rx_event(EventCtrlRecvSocketChange{});

    const auto mpdu = minimal_mpdu();
    ASSERT_TRUE(wifi.send(mpdu.data(), mpdu.size()));

    ASSERT_TRUE(run_io_brief(io));
    ASSERT_EQ(received.size(), mpdu.size());
}

TEST(WifiQueueTest, RegisterForwardSendsOneByte)
{
    auto inject = std::make_shared<DplaneQueue>();
    auto forward = std::make_shared<DplaneQueue>();
    inject->set_capacity(16);
    forward->set_capacity(16);
    auto cv = std::make_shared<bfc::cv_reactor<>>();
    ASSERT_TRUE(DplaneQueueRegistry::get_instance().add("wq2-wifi-tx", inject));
    ASSERT_TRUE(
        DplaneQueueRegistry::get_instance().add("wq2-wifi-rx", forward));
    ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add("wq2-reactor",
                                                               cv));

    std::atomic<bool> saw_reg{false};
    cv->add_read_rdy(
        *inject,
        [&]()
        {
            auto pkt = inject->pop();
            if (pkt && pkt->size() == 1)
            {
                saw_reg = true;
            }
        });

    CvRun cv_run(*cv);

    IOReactor io;
    WifiQueue wifi;
    ASSERT_TRUE(wifi.open(io, [](bfc::shared_sized_buffer) {}));
    WifiQueueDplaneBinding bind;
    bind.wifi_tx = "wq2-wifi-tx";
    bind.wifi_rx = "wq2-wifi-rx";
    bind.reactor = "wq2-reactor";
    ASSERT_TRUE(wifi.bind_dplane(bind));
    wifi.set_fcs_mode(RadioFcsMode::signal);
    ASSERT_TRUE(wifi.register_forward());

    for (int i = 0; i < 50 && !saw_reg.load(); ++i)
    {
        cv->wake_up();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(saw_reg.load());
}

namespace
{

// Registers a d-plane pair and a CV reactor under unique names for one test.
struct Dplane
{
    std::string prefix;
    std::shared_ptr<DplaneQueue> inject = std::make_shared<DplaneQueue>();
    std::shared_ptr<DplaneQueue> forward = std::make_shared<DplaneQueue>();
    std::shared_ptr<bfc::cv_reactor<>> cv =
        std::make_shared<bfc::cv_reactor<>>();
    WifiQueueDplaneBinding binding;

    explicit Dplane(std::string name, size_t inject_capacity = 16)
        : prefix(std::move(name))
    {
        inject->set_capacity(inject_capacity);
        forward->set_capacity(16);
        binding.wifi_tx = prefix + "-tx";
        binding.wifi_rx = prefix + "-rx";
        binding.reactor = prefix + "-cv";
        EXPECT_TRUE(
            DplaneQueueRegistry::get_instance().add(binding.wifi_tx, inject));
        EXPECT_TRUE(
            DplaneQueueRegistry::get_instance().add(binding.wifi_rx, forward));
        EXPECT_TRUE(
            bfc::cv_reactor_registry::get_instance().add(binding.reactor, cv));
    }
    ~Dplane()
    {
        DplaneQueueRegistry::get_instance().rem(binding.wifi_tx);
        DplaneQueueRegistry::get_instance().rem(binding.wifi_rx);
        bfc::cv_reactor_registry::get_instance().rem(binding.reactor);
    }
    // A forward frame as the peer sends it (signal mode: zero trailer).
    void push_forward()
    {
        std::vector<uint8_t> frame(24 + 4, 0);
        forward->push(
            bfc::shared_sized_buffer::copy_from(frame.data(), frame.size()));
        cv->wake_up();
    }
};

}  // namespace

// close() waits out an rx callback running on the reactor, however long it
// runs, and no callback runs afterwards.
TEST(WifiQueueTest, CloseWaitsForInFlightRxCallback)
{
    Dplane dplane("wq-inflight");
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> calls{0};
    IOReactor io;
    WifiQueue wifi;
    ASSERT_TRUE(wifi.open(io,
                          [&](bfc::shared_sized_buffer)
                          {
                              if (calls.fetch_add(1) == 0)
                              {
                                  entered.set_value();
                                  released.wait();
                              }
                          }));
    ASSERT_TRUE(wifi.bind_dplane(dplane.binding));
    wifi.set_fcs_mode(RadioFcsMode::signal);
    CvRun cv_run(*dplane.cv);
    dplane.push_forward();
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    auto closed = std::async(std::launch::async,
                             [&]
                             {
                                 wifi.close();
                             });
    EXPECT_EQ(closed.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);
    release.set_value();
    ASSERT_EQ(closed.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    closed.get();
    dplane.push_forward();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(calls.load(), 1);
    EXPECT_TRUE(dplane.forward->has_data());  // left for the next consumer
}

// A reactor that is not running never blocks close().
TEST(WifiQueueTest, CloseWithStoppedReactorReturnsAtOnce)
{
    Dplane dplane("wq-stopped");
    IOReactor io;
    WifiQueue wifi;
    ASSERT_TRUE(wifi.open(io, [](bfc::shared_sized_buffer) {}));
    ASSERT_TRUE(wifi.bind_dplane(dplane.binding));
    const auto begin = std::chrono::steady_clock::now();
    wifi.close();
    EXPECT_LT(std::chrono::steady_clock::now() - begin,
              std::chrono::milliseconds(50));
}

TEST(WifiQueueTest, CloseFromInsideRxCallback)
{
    Dplane dplane("wq-reentrant");
    IOReactor io;
    WifiQueue wifi;
    std::atomic<int> calls{0};
    ASSERT_TRUE(wifi.open(io,
                          [&](bfc::shared_sized_buffer)
                          {
                              ++calls;
                              wifi.close();
                          }));
    ASSERT_TRUE(wifi.bind_dplane(dplane.binding));
    wifi.set_fcs_mode(RadioFcsMode::signal);
    dplane.push_forward();
    dplane.push_forward();
    CvRun cv_run(*dplane.cv);
    for (int i = 0; i < 100 && calls.load() == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(calls.load(), 1);  // the drain loop stops once closed
    EXPECT_FALSE(wifi.send(minimal_mpdu().data(), 24));
}

// An RX event posted to the IO reactor never reaches a destroyed WifiQueue.
TEST(WifiQueueTest, PostedRxEventAfterDestructionIsIgnored)
{
    Dplane dplane("wq-posted");
    IOReactor io;
    {
        WifiQueue wifi;
        ASSERT_TRUE(wifi.open(io, [](bfc::shared_sized_buffer) {}));
        ASSERT_TRUE(wifi.bind_dplane(dplane.binding));
        wifi.post_rx_event(EventCtrlRecvSocketChange{});
    }
    ASSERT_TRUE(run_io_brief(io));
    EXPECT_FALSE(dplane.inject->has_data());
}

TEST(WifiQueueTest, TxEvictionIsCounted)
{
    Dplane dplane("wq-evict", 1);
    IOReactor io;
    WifiQueue wifi;
    ASSERT_TRUE(wifi.open(io, [](bfc::shared_sized_buffer) {}));
    ASSERT_TRUE(wifi.bind_dplane(dplane.binding));
    const auto mpdu = minimal_mpdu();
    ASSERT_TRUE(wifi.send(mpdu.data(), mpdu.size()));
    ASSERT_TRUE(wifi.send(mpdu.data(), mpdu.size()));
    EXPECT_EQ(wifi.peek_counters().wifi_tx_drops, 1u);
    EXPECT_EQ(dplane.inject->size(), 1u);
}
