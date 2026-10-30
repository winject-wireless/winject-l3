#include "ConsoleTestHelpers.h"
#include "mplane/MplaneErrno.h"
#include "utils/Version.h"

#include "WinjectBuildVersion.h"

#include <gtest/gtest.h>
#include <vector>

using namespace winject;
using namespace winject::test;

namespace
{

ControlOps handlers_with_list_upstream()
{
    ControlOps h = stub_console_handlers();
    h.list_upstream = [](const std::vector<uint8_t>&,
                         std::vector<UpstreamView>* rows, std::string*) -> bool
    {
        rows->clear();
        return true;
    };
    return h;
}

}  // namespace

TEST(ConsoleServiceSocketTest, UntaggedPing)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(stub_console_handlers(), &err));

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "ping", &reply));
    EXPECT_STREQ(reply.c_str(), "pong\n");
}

TEST(ConsoleServiceSocketTest, TaggedPingLuAndBogus)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(handlers_with_list_upstream(), &err));

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:7 ping", &reply));
    EXPECT_STREQ(reply.c_str(), "OK:7 pong\n");

    ASSERT_TRUE(harness.exchange(client, "cmd:7 lu", &reply));
    EXPECT_EQ(reply.rfind("OK:7 N=", 0), 0u);

    ASSERT_TRUE(harness.exchange(client, "cmd:7 bogus", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK:7 ENOSYS\n");
}

TEST(ConsoleServiceSocketTest, SameClientIdDifferentSenders)
{
    std::vector<ControlReply> pending_a;
    std::vector<ControlReply> pending_b;
    ControlOps h = stub_console_handlers();
    h.radio_info = [&](ControlReply cr)
    {
        if (pending_a.empty())
        {
            pending_a.push_back(cr);
            return;
        }
        pending_b.push_back(cr);
    };

    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(h, &err));

    bfc::socket client_a(bfc::create_udp4());
    bfc::socket client_b(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client_a, "cmd:5 ri"));
    ASSERT_TRUE(harness.wait_for([&]() { return pending_a.size() >= 1u; }));
    ASSERT_TRUE(harness.send_line(client_b, "cmd:5 ri"));
    ASSERT_TRUE(harness.wait_for([&]() { return pending_b.size() >= 1u; }));

    ASSERT_TRUE(
        harness.on_reactor([&]() { pending_b[0].send_text("OK from B"); }));
    std::string reply_b;
    ASSERT_TRUE(harness.recv_line(client_b.fd(), &reply_b));
    EXPECT_STREQ(reply_b.c_str(), "OK:5 from B\n");

    ASSERT_TRUE(
        harness.on_reactor([&]() { pending_a[0].send_text("OK from A"); }));
    std::string reply_a;
    ASSERT_TRUE(harness.recv_line(client_a.fd(), &reply_a));
    EXPECT_STREQ(reply_a.c_str(), "OK:5 from A\n");
}

TEST(ConsoleServiceSocketTest, OutOfOrderCompletionSameSender)
{
    std::vector<ControlReply> pending;
    ControlOps h = stub_console_handlers();
    h.radio_stats = [&](ControlReply cr)
    {
        pending.push_back(cr);
    };

    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(h, &err));

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "cmd:1 rs"));
    ASSERT_TRUE(harness.wait_for([&]() { return pending.size() >= 1u; }));
    ASSERT_TRUE(harness.send_line(client, "cmd:2 rs"));
    ASSERT_TRUE(harness.wait_for([&]() { return pending.size() >= 2u; }));

    ASSERT_TRUE(
        harness.on_reactor([&]() { pending[1].send_text("N=0 T=0\n"); }));
    std::string reply2;
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply2));
    EXPECT_EQ(reply2.rfind("OK:2 N=0 T=0", 0), 0u);

    ASSERT_TRUE(
        harness.on_reactor([&]() { pending[0].send_text("N=1 T=0\n"); }));
    std::string reply1;
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply1));
    EXPECT_EQ(reply1.rfind("OK:1 N=1 T=0", 0), 0u);
}

TEST(ConsoleServiceSocketTest, RetransmitWhileInFlight)
{
    int radio_info_calls = 0;
    ControlReply pending;
    ControlOps h = stub_console_handlers();
    h.radio_info = [&](ControlReply cr)
    {
        ++radio_info_calls;
        pending = cr;
    };

    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(h, &err));

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "cmd:3 ri"));
    ASSERT_TRUE(harness.wait_for([&]() { return radio_info_calls >= 1; }));
    ASSERT_TRUE(harness.send_line(client, "cmd:3 ri"));
    ASSERT_TRUE(harness.on_reactor([&]() { EXPECT_EQ(radio_info_calls, 1); }));

    ASSERT_TRUE(harness.on_reactor([&]() { pending.send_text("OK once"); }));
    std::string reply;
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply));
    EXPECT_STREQ(reply.c_str(), "OK:3 once\n");

    ASSERT_TRUE(harness.send_line(client, "cmd:3 ri"));
    ASSERT_TRUE(harness.wait_for([&]() { return radio_info_calls >= 2; }));
    ASSERT_TRUE(harness.on_reactor([&]() { EXPECT_EQ(radio_info_calls, 2); }));
    ASSERT_TRUE(harness.on_reactor([&]() { pending.send_text("OK twice"); }));
    ASSERT_TRUE(harness.recv_line(client.fd(), &reply));
    EXPECT_STREQ(reply.c_str(), "OK:3 twice\n");
}

TEST(ConsoleServiceSocketTest, TableFullReturnsBusy)
{
    std::vector<ControlReply> pending;
    ControlOps h = stub_console_handlers();
    h.radio_info = [&](ControlReply cr)
    {
        pending.push_back(cr);
    };

    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(h, &err));

    bfc::socket client(bfc::create_udp4());
    for (int i = 0; i < 256; ++i)
    {
        ASSERT_TRUE(harness.send_line(client, "ri"));
    }
    ASSERT_TRUE(harness.wait_for([&]() { return pending.size() >= 256u; }));

    ASSERT_TRUE(harness.send_line(client, "cmd:7 ri"));
    std::string busy;
    ASSERT_TRUE(harness.recv_line(client.fd(), &busy));
    EXPECT_STREQ(busy.c_str(), "NOK:7 EBUSY\n");
}

TEST(ConsoleServiceSocketTest, StopDropsLateReply)
{
    ControlReply pending;
    ControlOps h = stub_console_handlers();
    h.radio_info = [&](ControlReply cr)
    {
        pending = cr;
    };

    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(h, &err));

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "cmd:1 ri"));
    harness.stop();
    pending.send_text("OK late");

    std::string reply;
    EXPECT_FALSE(harness.recv_line(client.fd(), &reply, 50));
}

TEST(ConsoleServiceSocketTest, ResetAndInvalidArgs)
{
    int reset_calls = 0;
    ControlReply pending;
    ControlOps h = stub_console_handlers();
    h.radio_reset = [&](ControlReply cr)
    {
        ++reset_calls;
        pending = cr;
    };

    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(h, &err));

    bfc::socket client(bfc::create_udp4());
    ASSERT_TRUE(harness.send_line(client, "reset"));
    ASSERT_TRUE(harness.wait_for([&]() { return reset_calls >= 1; }));
    ASSERT_TRUE(harness.on_reactor([&]() { pending.send_text(""); }));
    std::string ok;
    ASSERT_TRUE(harness.recv_line(client.fd(), &ok));
    EXPECT_STREQ(ok.c_str(), "OK\n");

    ASSERT_TRUE(harness.exchange(client, "reset id=5", &ok));
    EXPECT_STREQ(ok.c_str(), "NOK EINVAL\n");

    ASSERT_TRUE(harness.send_line(client, "cmd:4 reset"));
    ASSERT_TRUE(harness.send_line(client, "cmd:4 reset"));
    ASSERT_TRUE(harness.wait_for([&]() { return reset_calls >= 2; }));
    // Second datagram is a retransmit.
    ASSERT_TRUE(harness.on_reactor([&]() { EXPECT_EQ(reset_calls, 2); }));
    ASSERT_TRUE(harness.on_reactor([&]() { pending.send_text(""); }));
    ASSERT_TRUE(harness.recv_line(client.fd(), &ok));
    EXPECT_STREQ(ok.c_str(), "OK:4\n");
}

TEST(ConsoleServiceSocketTest, SilentEmptyAndComment)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(stub_console_handlers(), &err));

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
    ASSERT_TRUE(harness.start(stub_console_handlers(), &err));

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
    ASSERT_TRUE(harness.start(stub_console_handlers(), &err));

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

TEST(ConsoleServiceSocketTest, RadioMplaneForwardsLine)
{
    ConsoleServiceHarness harness;
    std::string err;
    ControlOps h = stub_console_handlers();
    std::string seen;
    h.radio_mplane = [&seen](const std::string& line, ControlReply reply)
    {
        seen = line;
        reply.send_text("radio_tx_cal group=dsss default=false points=8:-28.00,84:15.60");
    };
    ASSERT_TRUE(harness.start(std::move(h), &err)) << err;

    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "cmd:3 rmp rtc group=dsss  points=8:-28,84:15.6",
                                 &reply));
    EXPECT_STREQ(reply.c_str(),
                 "OK:3 radio_tx_cal group=dsss default=false points=8:-28.00,84:15.60\n");
    EXPECT_EQ(seen, "rtc group=dsss  points=8:-28,84:15.6");

    ASSERT_TRUE(harness.exchange(client, "radio_mplane \tradio_tx_cal", &reply));
    EXPECT_EQ(seen, "radio_tx_cal");

    ASSERT_TRUE(harness.exchange(client, "radio_mplane", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK EINVAL\n");
    ASSERT_TRUE(harness.exchange(client, "rmp   ", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK EINVAL\n");
}

TEST(ConsoleServiceSocketTest, RadioMplaneWithoutHandlerIsEnosys)
{
    ConsoleServiceHarness harness;
    std::string err;
    ASSERT_TRUE(harness.start(stub_console_handlers(), &err)) << err;
    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "rmp radio_tx_cal", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK ENOSYS\n");
}

TEST(ConsoleServiceSocketTest, RadioTxNegativePower)
{
    ConsoleServiceHarness harness;
    std::string err;
    ControlOps h = stub_console_handlers();
    int power = 0;
    h.radio_tx = [&power](const RadioUpdate& patch, ControlReply reply)
    {
        power = patch.tx_power;
        reply.send_text("radio_tx channel=1 tx_power=-10");
    };
    ASSERT_TRUE(harness.start(std::move(h), &err)) << err;
    bfc::socket client(bfc::create_udp4());
    std::string reply;
    ASSERT_TRUE(harness.exchange(client, "rt tx_power=-10", &reply));
    EXPECT_EQ(power, -10);
    ASSERT_TRUE(harness.exchange(client, "rt tx_power=-200", &reply));
    EXPECT_STREQ(reply.c_str(), "NOK EINVAL\n");
}
