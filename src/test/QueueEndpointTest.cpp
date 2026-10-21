#include "endpoint/QueueEndpoint.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>
#include <chrono>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace winject;

namespace
{

using AppQueue = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
using AppQueueRegistry =
    bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

class IoRun
{
public:
    explicit IoRun(IOReactor& io)
        : io_(io),
          thread_(
              [&io]()
              {
                  io.run();
              })
    {
    }

    ~IoRun()
    {
        io_.stop();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    IoRun(const IoRun&) = delete;
    IoRun& operator=(const IoRun&) = delete;

private:
    IOReactor& io_;
    std::thread thread_;
};

class QueueEndpointTest : public ::testing::Test
{
protected:
    std::shared_ptr<AppQueue> in_queue = std::make_shared<AppQueue>();
    std::shared_ptr<AppQueue> out_queue = std::make_shared<AppQueue>();
    std::shared_ptr<bfc::cv_reactor<>> reactor =
        std::make_shared<bfc::cv_reactor<>>();
    IOReactor io;
    UpstreamConfig cfg;
    std::thread reactor_thread;

    void SetUp() override
    {
        in_queue->set_capacity(64);
        out_queue->set_capacity(64);
        ASSERT_TRUE(AppQueueRegistry::get_instance().add("qep-in", in_queue));
        ASSERT_TRUE(AppQueueRegistry::get_instance().add("qep-out", out_queue));
        ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add("qep-reactor",
                                                                 reactor));
        cfg.mode = UpstreamMode::queue;
        cfg.endpoint.rx = "qep-in";
        cfg.endpoint.tx = "qep-out";
        cfg.reactor = "qep-reactor";
        cfg.fec_type = FecType::none;

        auto started = std::make_shared<std::promise<void>>();
        auto ready = started->get_future();
        reactor->wake_up(
            [started]()
            {
                started->set_value();
            });
        reactor_thread = std::thread(
            [this]()
            {
                reactor->run();
            });
        ASSERT_EQ(ready.wait_for(std::chrono::seconds(2)),
                  std::future_status::ready);
    }

    void TearDown() override
    {
        reactor->stop();
        if (reactor_thread.joinable())
        {
            reactor_thread.join();
        }
        bfc::cv_reactor_registry::get_instance().rem("qep-reactor");
        AppQueueRegistry::get_instance().rem("qep-in");
        AppQueueRegistry::get_instance().rem("qep-out");
    }

    void push_in(const std::vector<uint8_t>& bytes)
    {
        (void)in_queue->push(
            bfc::shared_sized_buffer::copy_from(bytes.data(), bytes.size()));
        reactor->wake_up();
    }

    template <typename Pred>
    bool wait_until(Pred pred)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pred();
    }
};

}  // namespace

TEST_F(QueueEndpointTest, RawDatagramBecomesAirSduAndWakesOnce)
{
    std::atomic<int> wakes{0};
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg,
                              [&]()
                              {
                                  wakes.fetch_add(1);
                              }));
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    push_in(payload);
    push_in(payload);
    ASSERT_TRUE(wait_until(
        [&]()
        {
            return endpoint.app_tx_packets() == 2;
        }));
    EXPECT_EQ(wakes.load(), 1);
    EXPECT_EQ(endpoint.app_tx_bytes(), payload.size() * 2);

    int pulled = 0;
    for (;;)
    {
        bool is_fec = true;
        const bfc::sized_buffer sdu =
            endpoint.pull_tx(k_stream_payload_max, &is_fec);
        if (sdu.empty())
        {
            break;
        }
        EXPECT_FALSE(is_fec);
        ASSERT_EQ(sdu.size(), payload.size());
        EXPECT_EQ(0, memcmp(sdu.data(), payload.data(), payload.size()));
        ++pulled;
    }
    EXPECT_EQ(pulled, 2);
    EXPECT_EQ(endpoint.fec_air_tx_packets(), 0u);
    endpoint.close();
}

TEST_F(QueueEndpointTest, RadioRxIsDeliveredOnTheOutQueue)
{
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg));
    const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    endpoint.on_radio_rx(
        bfc::shared_sized_buffer::copy_from(payload.data(), payload.size()),
        false);
    auto got = out_queue->pop();
    ASSERT_TRUE(got);
    ASSERT_EQ(got->size(), payload.size());
    EXPECT_EQ(0, memcmp(got->u8(), payload.data(), payload.size()));
    EXPECT_EQ(endpoint.app_rx_packets(), 1u);
    EXPECT_EQ(endpoint.queue_out_drops(), 0u);
    endpoint.close();
}

TEST_F(QueueEndpointTest, OutQueueDropsOldestWhenFull)
{
    out_queue->set_capacity(1);
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg));
    const std::vector<uint8_t> first = {1};
    const std::vector<uint8_t> second = {2};
    endpoint.on_radio_rx(
        bfc::shared_sized_buffer::copy_from(first.data(), first.size()), false);
    endpoint.on_radio_rx(
        bfc::shared_sized_buffer::copy_from(second.data(), second.size()),
        false);
    EXPECT_EQ(endpoint.queue_out_drops(), 1u);
    auto got = out_queue->pop();
    ASSERT_TRUE(got);
    ASSERT_EQ(got->size(), 1u);
    EXPECT_EQ(got->u8()[0], 2);
    endpoint.close();
}

TEST_F(QueueEndpointTest, FecShardsAreTagged)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    cfg.fec_timeout_ms = 0;
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg));
    const std::vector<uint8_t> payload(50, 0xF1);
    push_in(payload);
    push_in(payload);
    ASSERT_TRUE(wait_until(
        [&]()
        {
            return endpoint.has_tx();
        }));
    int pulled = 0;
    for (;;)
    {
        bool is_fec = false;
        const bfc::sized_buffer sdu =
            endpoint.pull_tx(k_stream_payload_max, &is_fec);
        if (sdu.empty())
        {
            break;
        }
        EXPECT_TRUE(is_fec);
        ++pulled;
    }
    EXPECT_EQ(pulled, 3);
    EXPECT_EQ(endpoint.fec_air_tx_packets(), 3u);
    endpoint.close();
}

TEST_F(QueueEndpointTest, PartialBlockFlushesOnManagerReactor)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    cfg.fec_timeout_ms = 20;
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg));
    {
        IoRun run(io);
        push_in(std::vector<uint8_t>(8, 0x11));
        ASSERT_TRUE(wait_until(
            [&]()
            {
                return endpoint.has_tx();
            }));
    }
    bool is_fec = false;
    const bfc::sized_buffer sdu =
        endpoint.pull_tx(k_stream_payload_max, &is_fec);
    EXPECT_FALSE(sdu.empty());
    EXPECT_TRUE(is_fec);
    endpoint.close();
}

TEST_F(QueueEndpointTest, DropsOversizeAppDatagram)
{
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg));
    push_in(std::vector<uint8_t>(k_stream_payload_max + 1, 0xAB));
    ASSERT_TRUE(wait_until(
        [&]()
        {
            return endpoint.app_rx_oversize_pkt() == 1;
        }));
    EXPECT_FALSE(endpoint.has_tx());
    EXPECT_EQ(endpoint.app_tx_packets(), 1u);
    endpoint.close();
}

TEST_F(QueueEndpointTest, DrainsDatagramsQueuedBeforeOpen)
{
    const std::vector<uint8_t> payload = {9, 8, 7};
    (void)in_queue->push(
        bfc::shared_sized_buffer::copy_from(payload.data(), payload.size()));
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg));
    ASSERT_TRUE(wait_until(
        [&]()
        {
            return endpoint.has_tx();
        }));
    bool is_fec = true;
    const bfc::sized_buffer sdu =
        endpoint.pull_tx(k_stream_payload_max, &is_fec);
    ASSERT_EQ(sdu.size(), payload.size());
    EXPECT_FALSE(is_fec);
    EXPECT_EQ(0, memcmp(sdu.data(), payload.data(), payload.size()));
    endpoint.close();
}

TEST_F(QueueEndpointTest, OpenRejectsMissingAndAliasedQueues)
{
    QueueEndpoint endpoint;
    cfg.endpoint.rx = "missing";
    EXPECT_FALSE(endpoint.open(io, cfg));

    cfg.endpoint.rx = "qep-in";
    cfg.reactor = "missing";
    EXPECT_FALSE(endpoint.open(io, cfg));

    cfg.reactor = "qep-reactor";
    cfg.endpoint.tx = "qep-in";
    EXPECT_FALSE(endpoint.open(io, cfg));

    auto alias = std::make_shared<AppQueue>();
    alias->set_capacity(4);
    ASSERT_TRUE(AppQueueRegistry::get_instance().add("qep-alias-a", alias));
    ASSERT_TRUE(AppQueueRegistry::get_instance().add("qep-alias-b", alias));
    cfg.endpoint.rx = "qep-alias-a";
    cfg.endpoint.tx = "qep-alias-b";
    EXPECT_FALSE(endpoint.open(io, cfg));
    AppQueueRegistry::get_instance().rem("qep-alias-a");
    AppQueueRegistry::get_instance().rem("qep-alias-b");

    cfg.endpoint.rx = "qep-in";
    cfg.endpoint.tx = "qep-out";
    in_queue->set_capacity(0);
    EXPECT_FALSE(endpoint.open(io, cfg));
}

TEST_F(QueueEndpointTest, CloseDetachesBeforeDestroy)
{
    std::atomic<int> wakes{0};
    {
        QueueEndpoint endpoint;
        ASSERT_TRUE(endpoint.open(io, cfg,
                                  [&]()
                                  {
                                      wakes.fetch_add(1);
                                  }));
        endpoint.close();
        push_in(std::vector<uint8_t>{1, 2, 3});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(endpoint.app_tx_packets(), 0u);
        EXPECT_EQ(wakes.load(), 0);
    }
    push_in(std::vector<uint8_t>{4, 5, 6});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(QueueEndpointTest, FecAirSlotDecodesToTheOutQueue)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    cfg.fec_timeout_ms = 0;
    QueueEndpoint endpoint;
    ASSERT_TRUE(endpoint.open(io, cfg));
    const std::vector<uint8_t> payload(40, 0x5A);
    push_in(payload);
    push_in(payload);
    ASSERT_TRUE(wait_until(
        [&]()
        {
            return endpoint.has_tx();
        }));
    std::vector<bfc::sized_buffer> shards;
    for (;;)
    {
        bool is_fec = false;
        auto sdu = endpoint.pull_tx(k_stream_payload_max, &is_fec);
        if (sdu.empty())
        {
            break;
        }
        ASSERT_TRUE(is_fec);
        shards.push_back(std::move(sdu));
    }
    ASSERT_EQ(shards.size(), 3u);
    endpoint.on_radio_rx(
        bfc::shared_sized_buffer::copy_from(shards[2].data(), shards[2].size()),
        true);
    endpoint.on_radio_rx(
        bfc::shared_sized_buffer::copy_from(shards[1].data(), shards[1].size()),
        true);
    int got = 0;
    for (;;)
    {
        auto pkt = out_queue->pop();
        if (!pkt)
        {
            break;
        }
        ASSERT_EQ(pkt->size(), payload.size());
        EXPECT_EQ(0, memcmp(pkt->u8(), payload.data(), payload.size()));
        ++got;
    }
    EXPECT_EQ(got, 2);
    EXPECT_GE(endpoint.fec_recovered(), 1u);
    endpoint.close();
}
