#include "apps/bwtest/Bwtest.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace app_bwtest;
using winject::ReactiveLossyQueue;
using winject::UpstreamQueue;

namespace
{

class BwtestTest : public ::testing::Test
{
protected:
    void make(size_t ingress_capacity)
    {
        ingress = std::make_shared<ReactiveLossyQueue>();
        egress = std::make_shared<ReactiveLossyQueue>();
        ingress->set_capacity(ingress_capacity);
        egress->set_capacity(16);
        up = std::make_shared<UpstreamQueue>(ingress, egress);
        bw = std::make_unique<Bwtest>(up, 100);
    }
    void wait_done()
    {
        for (int i = 0; i < 2000 && bw->sending(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_FALSE(bw->sending());
    }
    size_t drain_ingress()
    {
        size_t n = 0;
        while (ingress->pop())
        {
            ++n;
        }
        return n;
    }

    std::shared_ptr<ReactiveLossyQueue> ingress;
    std::shared_ptr<ReactiveLossyQueue> egress;
    std::shared_ptr<UpstreamQueue> up;
    std::unique_ptr<Bwtest> bw;
};

}  // namespace

TEST_F(BwtestTest, SendsOfferedRateForDuration)
{
    make(1024);
    // 800 kbit/s for 100 ms = 10 kB = 100 datagrams of 100 B.
    ASSERT_TRUE(bw->start_send(800, 100));
    EXPECT_FALSE(bw->start_send(800, 100));  // busy
    wait_done();
    const Stats s = bw->stats();
    EXPECT_EQ(s.snd_pkt, 100u);
    EXPECT_EQ(s.snd_byt, 10000u);
    EXPECT_EQ(s.snd_drop, 0u);
    EXPECT_EQ(drain_ingress(), 100u);
    EXPECT_TRUE(bw->start_send(800, 10));  // finished sends free the slot
    wait_done();
}

TEST_F(BwtestTest, FullQueueAndDisconnectCountAsDrops)
{
    make(10);
    ASSERT_TRUE(bw->start_send(800, 100));
    wait_done();
    Stats s = bw->stats();
    EXPECT_EQ(s.snd_pkt, 100u);
    EXPECT_EQ(s.snd_drop, 90u);  // evicted by the undrained queue
    EXPECT_EQ(drain_ingress(), 10u);

    bw->clear();
    up->disconnect();
    ASSERT_TRUE(bw->start_send(800, 10));
    wait_done();
    s = bw->stats();
    EXPECT_EQ(s.snd_pkt, 10u);
    EXPECT_EQ(s.snd_drop, 10u);  // rejected
}

TEST_F(BwtestTest, CountsReceivedAndClears)
{
    make(16);
    egress->push(bfc::shared_sized_buffer::copy_from(
        reinterpret_cast<const uint8_t*>("abc"), 3));
    egress->push(bfc::shared_sized_buffer::copy_from(
        reinterpret_cast<const uint8_t*>("de"), 2));
    EXPECT_EQ(bw->poll_receive(), 2u);
    EXPECT_EQ(bw->stats().rcv_pkt, 2u);
    EXPECT_EQ(bw->stats().rcv_byt, 5u);
    EXPECT_EQ(bw->handle_command("clr\n"), "ok\n");
    EXPECT_EQ(bw->stats().rcv_pkt, 0u);
}

TEST_F(BwtestTest, ConsoleCommands)
{
    make(1024);
    EXPECT_EQ(bw->handle_command("stats"),
              "snd_pkt 0\nsnd_byt 0\nsnd_drop 0\nrcv_pkt 0\nrcv_byt 0\nlat_samples 0\nlat_invalid 0\nlat_last_us 0\nlat_min_us 0\nlat_avg_us 0\nlat_max_us 0\n");
    EXPECT_EQ(bw->handle_command("test_send 800"),
              "error usage: test_send <offer_kbps> <duration_ms>\n");
    EXPECT_EQ(bw->handle_command("test_send 0 10"),
              "error usage: test_send <offer_kbps> <duration_ms>\n");
    EXPECT_EQ(bw->handle_command("test_send 800 10 x"),
              "error usage: test_send <offer_kbps> <duration_ms>\n");
    EXPECT_EQ(bw->handle_command("nope"),
              "error unknown command (test_send, stats, clr)\n");
    EXPECT_EQ(bw->handle_command("test_send 800 50\n"), "ok\n");
    EXPECT_EQ(bw->handle_command("test_send 800 50"), "error busy\n");
    wait_done();
    EXPECT_EQ(bw->handle_command("stats"),
              "snd_pkt 50\nsnd_byt 5000\nsnd_drop 0\nrcv_pkt 0\nrcv_byt 0\nlat_samples 0\nlat_invalid 0\nlat_last_us 0\nlat_min_us 0\nlat_avg_us 0\nlat_max_us 0\n");
}

TEST_F(BwtestTest, AppCommands)
{
    make(16);
    std::string got;
    EXPECT_TRUE(bw->add_command("video_stop",
                                [&got](const std::string& args)
                                {
                                    got = args;
                                    return std::string("ok\n");
                                }));
    EXPECT_FALSE(bw->add_command("clr",
                                 [](const std::string&)
                                 {
                                     return std::string("x\n");
                                 }));
    EXPECT_EQ(bw->handle_command("video_stop now \r\n"), "ok\n");
    EXPECT_EQ(got, "now");
    EXPECT_EQ(bw->handle_command("video_stop"), "ok\n");
    EXPECT_EQ(got, "");
    EXPECT_EQ(bw->handle_command("clr"), "ok\n");
    EXPECT_EQ(bw->handle_command("nope"),
              "error unknown command (test_send, stats, clr, video_stop)\n");
}

TEST_F(BwtestTest, TimestampRoundTripAndInvalidPackets)
{
    make(16);
    int64_t now = 1000000;
    bw = std::make_unique<Bwtest>(up, 100, [&now] { return now; });
    ASSERT_TRUE(bw->start_send(800, 2));
    wait_done();
    auto first = ingress->pop();
    auto second = ingress->pop();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    egress->push(std::move(*first));
    now += 250;
    EXPECT_EQ(bw->poll_receive(), 1u);
    egress->push(std::move(*second));
    now += 500;
    EXPECT_EQ(bw->poll_receive(), 1u);
    const auto s = bw->stats();
    EXPECT_EQ(s.lat_samples, 2u);
    EXPECT_EQ(s.lat_last_us, 750u);
    EXPECT_EQ(s.lat_min_us, 250u);
    EXPECT_EQ(s.lat_avg_us, 500u);
    EXPECT_EQ(s.lat_max_us, 750u);
    EXPECT_NE(bw->handle_command("stats").find("lat_avg_us 500\n"), std::string::npos);
    bw->clear();
    EXPECT_EQ(bw->stats().lat_samples, 0u);
    EXPECT_EQ(bw->stats().lat_max_us, 0u);

    // Future timestamp and legacy/short data never become latency samples.
    now = 2000000;
    ASSERT_TRUE(bw->start_send(800, 1));
    wait_done();
    auto future = ingress->pop();
    ASSERT_TRUE(future);
    egress->push(std::move(*future));
    --now;
    egress->push(bfc::shared_sized_buffer::copy_from(
        reinterpret_cast<const uint8_t*>("old"), 3));
    bw->poll_receive();
    EXPECT_EQ(bw->stats().lat_invalid, 2u);
    EXPECT_EQ(bw->stats().lat_samples, 0u);
    EXPECT_EQ(bw->stats().rcv_pkt, 2u);
}

TEST_F(BwtestTest, RejectsPayloadTooSmallForTimestamp)
{
    make(16);
    Bwtest small(up, k_timestamp_header_bytes - 1);
    EXPECT_FALSE(small.start_send(800, 1));
}
