#include "endpoint/DatagramAir.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

using namespace winject;
namespace
{
class DatagramAirTest : public ::testing::Test
{
protected:
    DatagramAir::FecTimer timer;
    DatagramAir air;
    UpstreamConfig cfg;
    std::vector<std::vector<uint8_t>> received;
    void SetUp() override
    {
        cfg.fec_type = FecType::RsBlockErasure;
        cfg.fec_k = 2;
        cfg.fec_n = 3;
        cfg.fec_timeout_ms = 0;
        ASSERT_TRUE(start());
    }
    bool start()
    {
        return air.start(
            {},
            [&](const uint8_t* p, size_t n)
            {
                received.emplace_back(p, p + n);
            },
            &timer, {}, cfg, "test");
    }
    std::vector<std::vector<uint8_t>> drain()
    {
        std::vector<std::vector<uint8_t>> shards;
        while (air.has_tx())
        {
            bool fec = false;
            auto p = air.pull_tx(k_stream_payload_max, &fec);
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
    air.tx_pending_stats(&packets, &bytes);
    EXPECT_EQ(packets, 1024u);
    EXPECT_LE(bytes, 1024u * k_stream_payload_max);
    EXPECT_EQ(air.tx_queue_drops(), 15000u - 1024u);
}

TEST_F(DatagramAirTest, TimeoutChangeFlushesOldBlockAndAppliesNewDeadline)
{
    std::string error;
    ASSERT_TRUE(air.set_fec_timeout_ms(1000, &error));
    const uint8_t p = 1;
    air.accept_app_datagram(&p, 1);
    ASSERT_FALSE(air.has_tx());
    ASSERT_TRUE(air.set_fec_timeout_ms(1, &error));
    EXPECT_EQ(drain().size(), 2u);  // old partial block survived the change
    air.accept_app_datagram(&p, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    timer.schedule();
    EXPECT_EQ(drain().size(), 2u);
    EXPECT_EQ(air.fec_timeout_ms(), 1);
}

TEST_F(DatagramAirTest, ZeroTimeoutSurvivesRuntimeKnChange)
{
    std::string error;
    ASSERT_TRUE(air.set_fec(FecType::RsBlockErasure, 3, 4, &error));
    uint8_t p = 1;
    air.accept_app_datagram(&p, 1);
    timer.schedule(DatagramAir::FecTimer::current_time_us() + 1000000);
    EXPECT_FALSE(air.has_tx());
    air.announce_down();
    EXPECT_EQ(drain().size(), 2u);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(280));
    timer.schedule();
    EXPECT_EQ(received, (std::vector<std::vector<uint8_t>>{{2}}));
    EXPECT_EQ(air.fec_decode_fail(), 1u);
}

TEST_F(DatagramAirTest, TxReconfigurationPreservesPartialRxBlock)
{
    RsBlockErasure enc;
    ASSERT_TRUE(enc.init(2, 3, 20));
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(enc.encode_block({{1}, {2}}, 100, &shards));
    receive(shards[1]);
    std::string error;
    ASSERT_TRUE(air.set_fec(FecType::none, 0, 0, &error));
    ASSERT_TRUE(air.set_fec_timeout_ms(5, &error));
    receive(shards[2]);
    EXPECT_EQ(received, (std::vector<std::vector<uint8_t>>{{1}, {2}}));
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
    air.announce_down();
    for (const auto& shard : drain())
    {
        receive(shard);
    }
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[1], telemetry);
}

TEST_F(DatagramAirTest, InvalidReconfigureDoesNotFlushOrDisablePendingBlock)
{
    uint8_t p = 1;
    air.accept_app_datagram(&p, 1);
    std::string error;
    EXPECT_FALSE(air.set_fec(FecType::RsBlockErasure, 3, 3, &error));
    EXPECT_FALSE(air.has_tx());
    air.accept_app_datagram(&p, 1);
    EXPECT_EQ(drain().size(), 3u);
}

TEST_F(DatagramAirTest, ReopenClearsPendingTxAndDisablesOldFec)
{
    uint8_t p = 1;
    air.accept_app_datagram(&p, 1);
    cfg.fec_type = FecType::none;
    ASSERT_TRUE(start());
    air.accept_app_datagram(&p, 1);
    bool fec = true;
    EXPECT_EQ(air.pull_tx(k_stream_payload_max, &fec).size(), 1u);
    EXPECT_FALSE(fec);
    EXPECT_FALSE(air.has_tx());
}

TEST(DatagramAirLifetimeTest, ExtractedTimerCallbackIsSafeAfterDestroy)
{
    DatagramAir::FecTimer timer;
    auto air = std::make_unique<DatagramAir>();
    UpstreamConfig cfg;
    ASSERT_TRUE(air->start({}, {}, &timer, {}, cfg, "test"));
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

TEST_F(DatagramAirTest, ConcurrentInputAndRuntimeChanges)
{
    std::thread producer(
        [&]()
        {
            uint8_t p = 1;
            for (int i = 0; i < 2000; ++i)
            {
                air.accept_app_datagram(&p, 1);
            }
        });
    for (int i = 0; i < 100; ++i)
    {
        std::string error;
        EXPECT_TRUE(air.set_fec(FecType::RsBlockErasure, 2 + i % 3, 5, &error));
        EXPECT_TRUE(air.set_fec_timeout_ms(i % 2, &error));
    }
    producer.join();
    air.announce_down();
    uint64_t packets = 0;
    air.tx_pending_stats(&packets, nullptr);
    EXPECT_LE(packets, 1024u);
}
