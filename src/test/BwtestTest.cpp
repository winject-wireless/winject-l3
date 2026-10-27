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
              "snd_pkt 0\nsnd_byt 0\nsnd_drop 0\nrcv_pkt 0\nrcv_byt 0\n");
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
              "snd_pkt 50\nsnd_byt 5000\nsnd_drop 0\nrcv_pkt 0\nrcv_byt 0\n");
}
