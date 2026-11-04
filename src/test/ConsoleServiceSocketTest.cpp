// The UDP console against a real l3 and a FakeRadio: the wire as a client
// sees it (docs/mplane.md).

#include "ConsoleTestHelpers.h"
#include "WinjectBuildVersion.h"
#include "mplane/MplaneErrno.h"
#include "utils/Version.h"

#include <gtest/gtest.h>
#include <vector>

using namespace winject::core;
using namespace winject::core::test;

TEST(ConsoleServiceSocketTest, UntaggedPing)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "ping", &reply));
    EXPECT_STREQ(reply.c_str(), "pong\n");
}

TEST(ConsoleServiceSocketTest, TaggedPingLuAndBogus)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:7 ping", &reply));
    EXPECT_STREQ(reply.c_str(), "OK:7 pong\n");

    ASSERT_TRUE(harness.exchange(client, "cmd:7 lu", &reply));
    EXPECT_EQ(reply.rfind("OK:7 N=", 0), 0u);
    EXPECT_EQ(reply,
              "OK:7 N=1\nupstream id=4 txbus=1 rxbus=2 type=UDP rx= "
              "tx=127.0.0.1:9 fec=none fec_timeout=20ms quanta=256\n");

    ASSERT_TRUE(harness.exchange(client, "cmd:7 bogus", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK:7 ENOSYS\n");
}

TEST(ConsoleServiceSocketTest, SameClientIdDifferentSenders)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;
    harness.radio.hold("radio_tx_info");

    bfc::socket client_a(bfc::create_udp4());
    bfc::socket client_b(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client_a, "cmd:5 ri"));
    ASSERT_TRUE(harness.radio.wait_held("radio_tx_info", 1));
    ASSERT_TRUE(harness.send_line(client_b, "cmd:5 ri"));
    ASSERT_TRUE(harness.radio.wait_held("radio_tx_info", 2));

    harness.radio.answer("radio_tx_info", 1, true, "from B");
    std::string reply_b;
    ASSERT_TRUE(harness.recv_line(client_b.fd(), &reply_b));
    EXPECT_STREQ(reply_b.c_str(), "OK:5 from B\n");

    harness.radio.answer("radio_tx_info", 0, true, "from A");
    std::string reply_a;
    ASSERT_TRUE(harness.recv_line(client_a.fd(), &reply_a));
    EXPECT_STREQ(reply_a.c_str(), "OK:5 from A\n");
}

TEST(ConsoleServiceSocketTest, OutOfOrderCompletionSameSender)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;
    harness.radio.hold("tx_info");

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "cmd:1 rs"));
    ASSERT_TRUE(harness.radio.wait_held("tx_info", 1));
    ASSERT_TRUE(harness.send_line(client, "cmd:2 rs"));
    ASSERT_TRUE(harness.radio.wait_held("tx_info", 2));

    // radio_stats is tx_info then rx_info; rx_info answers at once.
    harness.radio.answer("tx_info", 1, true, FakeRadio::tx_info_line(2));
    std::string reply2;
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply2));
    EXPECT_EQ(reply2, "OK:2 " + FakeRadio::tx_info_line(2) + "\n" +
                          FakeRadio::rx_info_line(0) + "\n");

    harness.radio.answer("tx_info", 0, true, FakeRadio::tx_info_line(1));
    std::string reply1;
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply1));
    EXPECT_EQ(reply1, "OK:1 " + FakeRadio::tx_info_line(1) + "\n" +
                          FakeRadio::rx_info_line(0) + "\n");
}

TEST(ConsoleServiceSocketTest, RadioStatsThatDoNotParseAreEproto)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;
    harness.radio.hold("tx_info");

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "cmd:1 rs"));
    ASSERT_TRUE(harness.radio.wait_held("tx_info", 1));
    harness.radio.answer("tx_info", 0, true, "tx_info ether_pkt=1");
    std::string reply;
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply));
    EXPECT_STREQ(reply.c_str(), "NOK:1 EPROTO\n");
}

TEST(ConsoleServiceSocketTest, RetransmitWhileInFlight)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;
    harness.radio.hold("radio_tx_info");

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "cmd:3 ri"));
    ASSERT_TRUE(harness.radio.wait_held("radio_tx_info", 1));
    ASSERT_TRUE(harness.send_line(client, "cmd:3 ri"));
    // The retransmit is dropped: no second request reaches the radio.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(harness.radio.held_count("radio_tx_info"), 1u);

    harness.radio.answer("radio_tx_info", 0, true, "once");
    std::string reply;
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply));
    EXPECT_STREQ(reply.c_str(), "OK:3 once\n");

    ASSERT_TRUE(harness.send_line(client, "cmd:3 ri"));
    ASSERT_TRUE(harness.radio.wait_held("radio_tx_info", 2));
    harness.radio.answer("radio_tx_info", 1, true, "twice");
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply));
    EXPECT_STREQ(reply.c_str(), "OK:3 twice\n");
}

// The console's reactor never waits on l3: with l3's callback thread
// held, lines pile up as open routes until the table is full, and the
// console still answers.
TEST(ConsoleServiceSocketTest, TableFullReturnsBusy)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> holding{false};
    ASSERT_TRUE(harness.l3.l3
                    .post(
                        [released, &holding](const winject::L3&)
                        {
                            holding = true;
                            released.wait();
                        })
                    .ok());
    ASSERT_TRUE(wait_until(
        [&]()
        {
            return holding.load();
        }));

    bfc::socket client(bfc::create_udp4());
    for (int i = 0; i < 256; ++i)
    {
        ASSERT_TRUE(harness.send_line(client, "lu"));
    }
    ASSERT_TRUE(harness.send_line(client, "cmd:7 lu"));
    std::string busy;
    ASSERT_TRUE(harness.recv_line(client.fd(), &busy));
    EXPECT_STREQ(busy.c_str(), "NOK:7 EBUSY\n");

    release.set_value();
    for (int i = 0; i < 256; ++i)
    {
        std::string reply;
        ASSERT_TRUE(harness.recv_line(client.fd(), &reply)) << i;
        EXPECT_EQ(reply.rfind("N=1", 0), 0u) << reply;
    }
}

TEST(ConsoleServiceSocketTest, StopDropsLateReply)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;
    harness.radio.hold("radio_tx_info");

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "cmd:1 ri"));
    ASSERT_TRUE(harness.radio.wait_held("radio_tx_info", 1));
    harness.stop_console();
    harness.radio.answer("radio_tx_info", 0, true, "late");

    std::string reply;
    EXPECT_FALSE(harness.recv_line(client.fd(), &reply, 50));
}

TEST(ConsoleServiceSocketTest, ResetAndInvalidArgs)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;
    harness.radio.hold("reset");

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "reset"));
    ASSERT_TRUE(harness.radio.wait_held("reset", 1));
    harness.radio.answer("reset", 0, true, "");
    std::string ok;
    ASSERT_TRUE(harness.recv_line(client.fd(), &ok));
    EXPECT_STREQ(ok.c_str(), "OK\n");

    ASSERT_TRUE(harness.exchange(client, "reset id=5", &ok));
    EXPECT_STREQ(ok.c_str(), "NOK EINVAL\n");

    ASSERT_TRUE(harness.send_line(client, "cmd:4 reset"));
    ASSERT_TRUE(harness.send_line(client, "cmd:4 reset"));
    ASSERT_TRUE(harness.radio.wait_held("reset", 2));
    // Second datagram is a retransmit.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(harness.radio.held_count("reset"), 2u);
    harness.radio.answer("reset", 1, true, "");
    ASSERT_TRUE(harness.recv_line(client.fd(), &ok));
    EXPECT_STREQ(ok.c_str(), "OK:4\n");
}

TEST(ConsoleServiceSocketTest, SilentEmptyAndComment)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, ""));
    ASSERT_TRUE(harness.send_line(client, "# hi"));
    std::string reply;
    EXPECT_FALSE(harness.recv_line(client.fd(), &reply, 50));
}

TEST(ConsoleServiceSocketTest, MalformedCmdPrefix)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:300 ping", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK EINVAL\n");
}

// frozen: changing this breaks version discovery for every older and newer peer
TEST(ConsoleServiceSocketTest, VersionDiscoveryFrozen)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    char expected[128];
    snprintf(expected, sizeof(expected), "OK version ver=%s proto=1.0\n",
             WINJECT_VERSION_STRING);

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "version", &reply));
    EXPECT_STREQ(reply.c_str(), expected);

    snprintf(expected, sizeof(expected), "OK:7 version ver=%s proto=1.0\n",
             WINJECT_VERSION_STRING);
    ASSERT_TRUE(harness.exchange(client, "cmd:7 version", &reply));
    EXPECT_STREQ(reply.c_str(), expected);

    ASSERT_TRUE(harness.exchange(client, "version x=1", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK EINVAL\n");

    ASSERT_TRUE(harness.exchange(client, "bogus", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK ENOSYS\n");

    ASSERT_TRUE(harness.exchange(client, "cmd:7 bogus", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK:7 ENOSYS\n");
}

TEST(ConsoleServiceSocketTest, RadioCommandsOnTheWire)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:1 ri", &reply));
    EXPECT_EQ(reply,
              "OK:1 radio_tx channel=1 tx_power=20 modulation=OFDM_24M "
              "cca=false\nradio_rx rssi=-40\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:2 rci", &reply));
    EXPECT_EQ(reply, "OK:2 radio_caps_info fcs=SIGNAL\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:3 rt channel=6", &reply));
    EXPECT_EQ(reply,
              "OK:3 radio_tx channel=6 tx_power=20 modulation=OFDM_24M "
              "cca=false\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:4 rt tx_power=200", &reply));
    EXPECT_EQ(reply, "NOK:4 EINVAL\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:5 rt modulation=bogus", &reply));
    EXPECT_EQ(reply, "NOK:5 EINVAL\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:6 config slot=2", &reply));
    EXPECT_EQ(reply, "OK:6 slot=2\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:7 radio_domain", &reply));
    EXPECT_EQ(reply, "OK:7 radio_domain id=0 domain=1234\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:8 radio_domain id=1", &reply));
    EXPECT_EQ(reply, "NOK:8 EINVAL\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:9 rd id=0", &reply));
    EXPECT_EQ(reply.rfind("OK:9 radio_device id=0 mplane=127.0.0.1:", 0), 0u)
        << reply;
    EXPECT_NE(reply.find(" fcs=SIGNAL"), std::string::npos) << reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:10 rd id=1", &reply));
    EXPECT_EQ(reply, "NOK:10 EINVAL\n");
}

TEST(ConsoleServiceSocketTest, UpstreamCommandsOnTheWire)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    const std::string rx =
        "127.0.0.1:" + std::to_string(reserve_free_udp_port());
    const std::string au = "cmd:1 au id=6 txbus=3 rxbus=4 type=UDP rx=" + rx +
                           " tx=127.0.0.1:9 fec=BLOCK k=2 n=3 fec_timeout=10ms "
                           "quanta=128";
    ASSERT_TRUE(harness.exchange(client, au.c_str(), &reply));
    EXPECT_EQ(reply, "OK:1 upstream id=6 txbus=3 rxbus=4 type=UDP rx=" + rx +
                         " tx=127.0.0.1:9 fec=block(2,3) fec_timeout=10ms "
                         "quanta=128\n");
    // A port nothing can bind is EIO, as before.
    ASSERT_TRUE(harness.exchange(client,
                                 "cmd:2 au id=7 txbus=5 rxbus=5 type=UDP "
                                 "rx=127.0.0.1:0 tx=127.0.0.1:9 fec=NONE k=0 "
                                 "n=0 fec_timeout=10ms quanta=128",
                                 &reply));
    EXPECT_EQ(reply, "NOK:2 EIO\n");
    const std::string again =
        "cmd:2 au id=6 txbus=5 rxbus=5 type=UDP rx=" + rx +
        " tx=127.0.0.1:9 fec=NONE k=0 n=0 "
        "fec_timeout=10ms quanta=128";
    ASSERT_TRUE(harness.exchange(client, again.c_str(), &reply));
    EXPECT_EQ(reply, "NOK:2 EEXIST\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:3 uu id=6 k=3 n=5", &reply));
    EXPECT_EQ(reply.rfind("OK:3 upstream id=6 ", 0), 0u) << reply;
    EXPECT_NE(reply.find("fec=block(3,5) fec_timeout=10ms"), std::string::npos)
        << reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:4 uu id=6 quanta=512", &reply));
    EXPECT_NE(reply.find("fec=block(3,5)"), std::string::npos) << reply;
    EXPECT_NE(reply.find("quanta=512"), std::string::npos) << reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:5 uu id=6", &reply));
    EXPECT_EQ(reply, "NOK:5 EINVAL\n");
    ASSERT_TRUE(
        harness.exchange(client, "cmd:6 uu id=6 fec_timeout=0", &reply));
    EXPECT_EQ(reply, "NOK:6 EINVAL\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:7 uu id=9 quanta=1", &reply));
    EXPECT_EQ(reply, "NOK:7 ENOENT\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:8 lur ids=6", &reply));
    EXPECT_EQ(reply.rfind("OK:8 N=1 T=", 0), 0u) << reply;
    EXPECT_NE(reply.find("\nupstream_rx_stat id=6 rxbyt=0 rxpkt=0 "
                         "rx_oversize=0 rxgap=0 fec=block(3,5) fec_rec=0 "
                         "fec_lost=0 fec_rxbyt=0 fec_rxpkt=0 fec_rxgap=0 "
                         "rx_push_drop=0\n"),
              std::string::npos)
        << reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:9 lut", &reply));
    EXPECT_EQ(reply.rfind("OK:9 N=2 T=", 0), 0u) << reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:10 lur ids=9", &reply));
    EXPECT_EQ(reply, "NOK:10 ENOENT\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:11 ru id=6", &reply));
    EXPECT_EQ(reply, "OK:11\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:12 ru id=6", &reply));
    EXPECT_EQ(reply, "NOK:12 ENOENT\n");
    ASSERT_TRUE(
        harness.exchange(client, "cmd:13 gm keys=radio_proto_ok", &reply));
    EXPECT_EQ(reply.rfind("OK:13 N=1 T=", 0), 0u) << reply;
    EXPECT_NE(reply.find("\nradio_proto_ok=1\n"), std::string::npos) << reply;
}

TEST(ConsoleServiceSocketTest, RadioMplaneForwardsLine)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(
        client, "cmd:3 rmp radio_tx_cal group=dsss  points=8:-28,84:15.6",
        &reply));
    EXPECT_EQ(reply,
              "OK:3 radio_tx_cal group=dsss default=false "
              "points=8:-28.00,84:15.60\n");
    // The rest of the line goes to the radio unchanged.
    EXPECT_EQ(harness.radio.seen(
                  "radio_tx_cal group=dsss  points=8:-28,84:15.6"),
              1u);

    ASSERT_TRUE(harness.exchange(client, "radio_mplane \tradio_tx_cal", &reply));
    EXPECT_EQ(harness.radio.seen("radio_tx_cal"), 2u);

    // The radio's NOK comes back as is.
    ASSERT_TRUE(harness.exchange(client, "cmd:4 rmp bogus x=1", &reply));
    EXPECT_EQ(reply, "NOK:4 ENOSYS\n");

    ASSERT_TRUE(harness.exchange(client, "radio_mplane", &reply));
    EXPECT_EQ(reply, "NOK EINVAL\n");
    ASSERT_TRUE(harness.exchange(client, "rmp   ", &reply));
    EXPECT_EQ(reply, "NOK EINVAL\n");
}

TEST(ConsoleServiceSocketTest, RadioTxNegativePower)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "rt tx_power=-10", &reply));
    EXPECT_EQ(reply,
              "radio_tx channel=1 tx_power=-10 modulation=OFDM_24M "
              "cca=false\n");
    ASSERT_TRUE(harness.exchange(client, "rt tx_power=-200", &reply));
    EXPECT_EQ(reply, "NOK EINVAL\n");
}

TEST(ConsoleServiceSocketTest, TaggedCommentIsAnsweredAndFreesItsId)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(&err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:5 #note", &reply));
    EXPECT_EQ(reply, "NOK:5 ENOSYS\n");
    ASSERT_TRUE(harness.exchange(client, "cmd:6  # spaced", &reply));
    EXPECT_EQ(reply, "NOK:6 ENOSYS\n");
    // cmd:5 is not stuck as a retransmit.
    ASSERT_TRUE(harness.exchange(client, "cmd:5 ping", &reply));
    EXPECT_EQ(reply.rfind("OK:5", 0), 0u) << reply;
}
