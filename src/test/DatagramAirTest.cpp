#include "endpoint/DatagramAir.h"
#include "frames/LCHeader.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace winject;
namespace
{
bfc::shared_sized_buffer pull_air_sdu(const std::shared_ptr<BoundedLossyQueue>& queue,
                                      size_t max, bool* fec)
{
    auto item = queue->pop(max);
    if (!item)
    {
        return {};
    }
    const auto& slot = *item;
    *fec = LCHeader::read_is_fec(slot.u8());
    return slot.subview(k_slot_prefix_len, slot.size() - k_slot_prefix_len);
}
class DatagramAirTest : public ::testing::Test
{
protected:
    std::shared_ptr<DatagramAir::Reactor> cv =
        std::make_shared<DatagramAir::Reactor>();
    DatagramAir::FecTimer& timer = cv->get_timer();
    std::shared_ptr<BoundedLossyQueue> tx_queue =
        std::make_shared<BoundedLossyQueue>();
    DatagramAir air;
    UpstreamConfig cfg;
    UpstreamStats tx_stats;
    std::mutex tx_stats_mu;
    std::vector<std::vector<uint8_t>> received;
    void SetUp() override
    {
        cfg.fec_type = FecType::RsBlockErasure;
        cfg.fec_k = 2;
        cfg.fec_n = 3;
        cfg.fec_timeout_ms = 0;
        cfg.bus_tx = 7;
        ASSERT_TRUE(start());
    }
    bool start()
    {
        while (tx_queue->pop())
        {
        }
        tx_stats = UpstreamStats{};
        return air.start(
            tx_queue,
            [&](const uint8_t* p, size_t n)
            {
                received.emplace_back(p, p + n);
            },
            cv, cfg, cfg.bus_tx, &tx_stats, &tx_stats_mu);
    }
    std::vector<std::vector<uint8_t>> drain()
    {
        std::vector<std::vector<uint8_t>> shards;
        while (tx_queue->has_data())
        {
            bool fec = false;
            auto p = pull_air_sdu(tx_queue, k_wifi_payload_max, &fec);
            EXPECT_TRUE(fec);
            if (p.empty())
            {
                break;
            }
            shards.emplace_back(
                reinterpret_cast<const uint8_t*>(p.data()),
                reinterpret_cast<const uint8_t*>(p.data()) + p.size());
        }
        return shards;
    }
    void receive(const std::vector<uint8_t>& p, bool fec = true)
    {
        air.on_radio_rx(bfc::shared_sized_buffer::copy_from(p.data(), p.size()),
                        fec);
    }
};
}  // namespace

TEST_F(DatagramAirTest, EncodedBacklogIsBoundedAndDropsCounted)
{
    uint8_t p = 1;
    for (int i = 0; i < 10000; ++i)
    {
        air.accept_app_datagram(&p, 1);
    }
    uint64_t packets = 0, bytes = 0;
    tx_queue->pending_stats(&packets, &bytes);
    EXPECT_EQ(packets, 1024u);
    EXPECT_LE(bytes, 1024u * k_wifi_payload_max);
    EXPECT_EQ(air.tx_queue_drops(), 15000u - 1024u);
}

TEST_F(DatagramAirTest, TxTimeoutFlushesPartialBlockOnReactorTimer)
{
    cfg.fec_timeout_ms = 1;
    ASSERT_TRUE(start());
    const uint8_t p = 1;
    air.accept_app_datagram(&p, 1);
    EXPECT_FALSE(tx_queue->has_data());
    int64_t deadline_us = 0;
    ASSERT_TRUE(timer.get_next_deadline_us(deadline_us));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    timer.schedule();
    EXPECT_EQ(drain().size(), 2u);
    EXPECT_FALSE(timer.get_next_deadline_us(deadline_us));
}

TEST_F(DatagramAirTest, ZeroTimeoutWaitsForFullBlockWithoutTimer)
{
    uint8_t p = 1;
    air.accept_app_datagram(&p, 1);
    int64_t deadline_us = 0;
    EXPECT_FALSE(timer.get_next_deadline_us(deadline_us));
    EXPECT_FALSE(tx_queue->has_data());
    air.accept_app_datagram(&p, 1);
    EXPECT_EQ(drain().size(), 3u);
}

TEST_F(DatagramAirTest, EarlySystematicDeliveryIsNotRepeatedAfterRecovery)
{
    RsBlockErasure enc;
    ASSERT_TRUE(enc.init(2, 3, 20));
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(enc.encode_block({{1}, {2}}, 100, &shards));
    receive(shards[0]);
    EXPECT_EQ(received, (std::vector<std::vector<uint8_t>>{{1}}));
    receive(shards[2]);
    EXPECT_EQ(received, (std::vector<std::vector<uint8_t>>{{1}, {2}}));
    receive(shards[1]);
    EXPECT_EQ(received.size(), 2u);
}

TEST_F(DatagramAirTest, RxGiveUpSalvagesDataWithoutFurtherArrivals)
{
    RsBlockErasure enc;
    ASSERT_TRUE(enc.init(3, 4, 20));
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(enc.encode_block({{1}, {2}, {3}}, 100, &shards));
    receive(shards[1]);  // head missing, no immediate output
    EXPECT_TRUE(received.empty());
    // One timer at the block deadline (spread cap + margin), not a poll.
    int64_t deadline_us = 0;
    ASSERT_TRUE(timer.get_next_deadline_us(deadline_us));
    const int64_t wait_us = deadline_us - DatagramAir::FecTimer::current_time_us();
    EXPECT_GT(wait_us, 100000);
    EXPECT_LE(wait_us, (RsBlockErasure::k_spread_cap_ms +
                        RsBlockErasure::k_deadline_margin_min_ms) * 1000);
    timer.schedule(deadline_us - 50000);
    EXPECT_TRUE(received.empty());
    std::this_thread::sleep_for(std::chrono::microseconds(
        deadline_us - DatagramAir::FecTimer::current_time_us() + 1000));
    timer.schedule();
    EXPECT_EQ(received, (std::vector<std::vector<uint8_t>>{{2}}));
    EXPECT_EQ(air.fec_decode_fail(), 1u);
    EXPECT_FALSE(timer.get_next_deadline_us(deadline_us));
}

TEST_F(DatagramAirTest, CompleteRxBlockLeavesNoTimer)
{
    RsBlockErasure enc;
    ASSERT_TRUE(enc.init(2, 3, 20));
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(enc.encode_block({{1}, {2}}, 100, &shards));
    receive(shards[0]);
    receive(shards[1]);
    EXPECT_EQ(received, (std::vector<std::vector<uint8_t>>{{1}, {2}}));
    int64_t deadline_us = 0;
    EXPECT_FALSE(timer.get_next_deadline_us(deadline_us));
}

TEST_F(DatagramAirTest, PayloadAndTelemetryStayOpaque)
{
    // vstreamer v2 outer header (data=0, fec=0) plus link-report prefix.
    // The LC flag alone determines whether winject runs its decoder.
    std::vector<uint8_t> telemetry(68, 0);
    telemetry[2] = 2u << 3;
    telemetry[4] = 'V';
    telemetry[5] = 'L';
    telemetry[6] = 2;
    receive(telemetry, false);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], telemetry);
    air.accept_app_datagram(telemetry.data(), telemetry.size());
    air.accept_app_datagram(telemetry.data(), telemetry.size());  // k = 2
    for (const auto& shard : drain())
    {
        receive(shard);
    }
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[1], telemetry);
    EXPECT_EQ(received[2], telemetry);
}

TEST_F(DatagramAirTest, ReopenClearsPendingTxAndDisablesOldFec)
{
    uint8_t p = 1;
    air.accept_app_datagram(&p, 1);
    cfg.fec_type = FecType::none;
    ASSERT_TRUE(start());
    air.accept_app_datagram(&p, 1);
    bool fec = true;
    EXPECT_EQ(pull_air_sdu(tx_queue, k_wifi_payload_max, &fec).size(), 1u);
    EXPECT_FALSE(fec);
    EXPECT_FALSE(tx_queue->has_data());
}

TEST(DatagramAirLifetimeTest, ExtractedTimerCallbackIsSafeAfterDestroy)
{
    std::shared_ptr<DatagramAir::Reactor> cv =
        std::make_shared<DatagramAir::Reactor>();
    DatagramAir::FecTimer& timer = cv->get_timer();
    auto air = std::make_unique<DatagramAir>();
    UpstreamConfig cfg;
    cfg.bus_tx = 1;
    UpstreamStats stats;
    std::mutex stats_mu;
    ASSERT_TRUE(air->start(std::make_shared<BoundedLossyQueue>(),
                           [](const uint8_t*, size_t) {}, cv, cfg, cfg.bus_tx,
                           &stats, &stats_mu));
    timer.wait_us(0,
                  [&]()
                  {
                      air.reset();
                  });
    RsBlockErasure enc;
    ASSERT_TRUE(enc.init(2, 3, 20));
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(enc.encode_block({{1}, {2}}, 0, &shards));
    air->on_radio_rx(
        bfc::shared_sized_buffer::copy_from(shards[1].data(), shards[1].size()),
        true);
    // schedule extracts both callbacks before invoking the first one.
    timer.schedule(DatagramAir::FecTimer::current_time_us() + 1000000);
    EXPECT_FALSE(air);
}
