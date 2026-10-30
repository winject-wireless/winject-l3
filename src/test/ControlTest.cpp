#include "App.h"
#include "ConsoleTestHelpers.h"
#include "Control.h"
#include "ControlNode.h"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <thread>

using namespace winject;
using winject_l3::Control;
using winject_l3::RadioReply;

namespace
{

// A reactor on its own thread with a Control attached to stub ops, as App
// does it.
class NodeHarness
{
public:
    explicit NodeHarness(ControlOps ops)
    {
        control.impl()->attach(&reactor, std::move(ops));
        // Keep the loop alive until stop().
        reactor.get_timer().wait_ms(10000, [] {});
        worker = std::thread(
            [this]()
            {
                reactor.run();
                control.impl()->detach();
            });
        on_reactor([] {});
    }

    ~NodeHarness()
    {
        stop();
    }

    void stop()
    {
        if (worker.joinable())
        {
            reactor.stop();
            worker.join();
        }
    }

    void on_reactor(const std::function<void()>& fn)
    {
        std::promise<void> done;
        auto ran = done.get_future();
        reactor.wake_up(
            [&]()
            {
                fn();
                done.set_value();
            });
        ran.wait();
    }

    IOReactor reactor;
    Control control;
    std::thread worker;
};

ControlOps list_ops()
{
    ControlOps ops = test::stub_console_handlers();
    ops.list_upstream = [](const std::vector<uint8_t>& ids,
                           std::vector<UpstreamView>* out, std::string*)
    {
        UpstreamView v;
        v.id = ids.empty() ? 7 : ids[0];
        v.bus_tx = 1;
        v.bus_rx = 2;
        out->push_back(v);
        return true;
    };
    ops.add_upstream = [](const UpstreamView&, std::string* error)
    {
        *error = "EEXIST";
        return false;
    };
    return ops;
}

TEST(ControlTest, DetachedCallsReturnEsrch)
{
    Control c;
    EXPECT_FALSE(c.running());
    EXPECT_EQ(c.list_upstreams().status.error, "ESRCH");
    EXPECT_EQ(c.remove_upstream(1).error, "ESRCH");
    EXPECT_EQ(c.radio_info().status.error, "ESRCH");
    EXPECT_EQ(c.command("lu"), "NOK ESRCH");
    std::string async_error;
    c.radio_stats(
        [&](const RadioReply& r)
        {
            async_error = r.status.error;
        });
    EXPECT_EQ(async_error, "ESRCH");
}

TEST(ControlTest, BlankAndOverlongLinesMatchTheUdpConsole)
{
    Control c;
    EXPECT_EQ(c.command("   "), "");
    EXPECT_EQ(c.command("# comment"), "");
    EXPECT_EQ(c.command(std::string(600, 'x')), "NOK EINVAL");
}

TEST(ControlTest, SyncCallsRunOnTheNodeThread)
{
    std::thread::id op_thread;
    ControlOps ops = list_ops();
    ops.remove_upstream = [&](uint8_t, std::string*)
    {
        op_thread = std::this_thread::get_id();
        return true;
    };
    NodeHarness node(std::move(ops));
    EXPECT_TRUE(node.control.running());

    const auto rows = node.control.list_upstreams({3});
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value.size(), 1u);
    EXPECT_EQ(rows.value[0].id, 3);

    EXPECT_EQ(node.control.add_upstream(UpstreamView{}).error, "EEXIST");
    EXPECT_TRUE(node.control.remove_upstream(3).ok());
    EXPECT_EQ(op_thread, node.worker.get_id());
}

TEST(ControlTest, CommandUsesTheTextConsole)
{
    NodeHarness node(list_ops());
    const std::string reply = node.control.command("lu");
    EXPECT_EQ(reply.rfind("N=1", 0), 0u) << reply;
    EXPECT_NE(reply.find("id=7"), std::string::npos) << reply;
    EXPECT_EQ(node.control.command("bogus"), "NOK ENOSYS");
    EXPECT_EQ(node.control.command("ping"), "pong\n");
}

TEST(ControlTest, RadioForwardRepliesLater)
{
    ControlReply pending;
    ControlOps ops = test::stub_console_handlers();
    ops.radio_info = [&](ControlReply cr)
    {
        pending = cr;
    };
    NodeHarness node(std::move(ops));

    std::promise<RadioReply> got;
    auto reply = got.get_future();
    node.control.radio_info(
        [&](const RadioReply& r)
        {
            got.set_value(r);
        });
    node.on_reactor(
        [&]()
        {
            ASSERT_TRUE(pending.send_text);
            pending.send_text("channel=6 tx_power=14");
            // A second answer is ignored.
            pending.send_nok("EIO");
        });
    ASSERT_EQ(reply.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    const RadioReply r = reply.get();
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body, "channel=6 tx_power=14");
}

TEST(ControlTest, BlockingRadioCallTimesOut)
{
    ControlReply pending;
    ControlOps ops = test::stub_console_handlers();
    ops.radio_stats = [&](ControlReply cr)
    {
        pending = cr;
    };
    NodeHarness node(std::move(ops));
    const RadioReply r =
        node.control.radio_stats(std::chrono::milliseconds(20));
    EXPECT_EQ(r.status.error, "ETIMEDOUT");
    // The late answer goes nowhere.
    node.on_reactor(
        [&]()
        {
            pending.send_text("late");
        });
}

TEST(ControlTest, UnansweredRadioForwardIsCanceledWhenTheNodeDrops)
{
    std::string error;
    {
        ControlReply pending;
        ControlOps ops = test::stub_console_handlers();
        ops.radio_reset = [&](ControlReply cr)
        {
            pending = cr;
        };
        NodeHarness node(std::move(ops));
        node.control.radio_reset(
            [&](const RadioReply& r)
            {
                error = r.status.error;
            });
        node.on_reactor([] {});
        node.stop();
        EXPECT_TRUE(error.empty());
        EXPECT_FALSE(node.control.running());
        pending = {};
    }
    EXPECT_EQ(error, "ECANCELED");
}

TEST(ControlTest, BlockingCallFromTheNodeThreadIsRefused)
{
    NodeHarness node(list_ops());
    RadioReply r;
    std::string text;
    bool listed = false;
    node.on_reactor(
        [&]()
        {
            r = node.control.radio_info();
            text = node.control.command("lu");
            // Synchronous ops run inline instead.
            listed = node.control.list_upstreams().ok();
        });
    EXPECT_EQ(r.status.error, "EDEADLK");
    EXPECT_EQ(text, "NOK EDEADLK");
    EXPECT_TRUE(listed);
}

TEST(ControlTest, AppAttachesWhileRunning)
{
    sockaddr_in radio{};
    radio.sin_family = AF_INET;
    radio.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    radio.sin_port = htons(test::reserve_free_udp_port());
    Config cfg;
    cfg.domain = 0x1234;
    cfg.modulation = "OFDM_24M";
    cfg.radio_device.dplane = radio;
    cfg.radio_device.mplane = radio;
    cfg.radio_device.fcs = RadioFcsConfig::signal;
    UpstreamConfig up;
    up.index = 4;
    up.mode = UpstreamMode::udp_client;
    up.endpoint.tx = "127.0.0.1:9";
    up.bus_tx = 1;
    up.bus_rx = 2;
    cfg.upstreams.push_back(up);

    Control c;
    App app;
    app.set_control(c);
    ASSERT_TRUE(app.load(cfg));
    auto result = std::async(std::launch::async,
                             [&]()
                             {
                                 return app.run();
                             });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!c.running() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(c.running());

    auto rows = c.list_upstreams();
    ASSERT_TRUE(rows.ok()) << rows.status.error;
    ASSERT_EQ(rows.value.size(), 1u);
    EXPECT_EQ(rows.value[0].id, 4);

    UpstreamUpdate patch;
    patch.id = 4;
    patch.have_quanta = true;
    patch.quanta = 512;
    auto updated = c.update_upstream(patch);
    ASSERT_TRUE(updated.ok()) << updated.status.error;
    EXPECT_EQ(updated.value.quanta, 512u);

    EXPECT_EQ(c.remove_upstream(9).error, "ENOENT");
    // No radio answers on this port.
    EXPECT_EQ(c.radio_info().status.error, "ENODEV");
    auto device = c.radio_device();
    ASSERT_TRUE(device.ok());
    EXPECT_EQ(device.value.device.mplane.sin_port, radio.sin_port);

    app.stop();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(result.get(), 0);
    EXPECT_FALSE(c.running());
    EXPECT_EQ(c.list_upstreams().status.error, "ESRCH");
}

}  // namespace
