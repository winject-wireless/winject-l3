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
        : reactor_(reactor),
          thread_(
              [&reactor]()
              {
                  reactor.run();
              })
    {
    }

    ~CvRun()
    {
        reactor_.stop();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    CvRun(const CvRun&) = delete;
    CvRun& operator=(const CvRun&) = delete;

private:
    bfc::cv_reactor<>& reactor_;
    std::thread thread_;
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
    ASSERT_TRUE(DplaneQueueRegistry::get_instance().add("wq-inject", inject));
    ASSERT_TRUE(DplaneQueueRegistry::get_instance().add("wq-forward", forward));
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
    bind.inject_tx = "wq-inject";
    bind.forward_rx = "wq-forward";
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
    ASSERT_TRUE(DplaneQueueRegistry::get_instance().add("wq2-inject", inject));
    ASSERT_TRUE(
        DplaneQueueRegistry::get_instance().add("wq2-forward", forward));
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
    bind.inject_tx = "wq2-inject";
    bind.forward_rx = "wq2-forward";
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
