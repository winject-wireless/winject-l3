// The public API (include/winject_l3) against a real l3 and a FakeRadio,
// including the threading rules of aidocs/public-api.md: any thread may call
// l3, user code never runs on l3's thread except on_queue, and
// an l3 that stops cancels what is pending.

#include "ConsoleTestHelpers.h"
#include "L3Core.h"
#include "L3Impl.h"
#include "radio/RadioMplaneParse.h"
#include "winject_l3/Console.h"
#include "winject_l3/L3.h"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <thread>

using namespace winject::core;
using namespace winject::core::test;
namespace api = winject;

namespace
{

TEST(L3ApiTest, DetachedHandlesReturnEsrch)
{
    api::L3 none;
    EXPECT_FALSE(none.running());
    EXPECT_EQ(none.upstreams().list().status.error, "ESRCH");
    EXPECT_EQ(none.radios().get().status.error, "ESRCH");
    api::Upstream up;
    EXPECT_EQ(up.spec().status.error, "ESRCH");
    EXPECT_FALSE(up.valid());
    api::Radio radio;
    EXPECT_EQ(radio.info().status.error, "ESRCH");
    std::string async_error;
    radio.stats(
        [&](api::Result<api::RadioStats> r)
        {
            async_error = r.status.error;
        });
    EXPECT_EQ(async_error, "ESRCH");
    EXPECT_EQ(none.post([](const api::L3&) {}).error, "ESRCH");
    EXPECT_EQ(api::l3::console::execute(none, "lu"), "NOK ESRCH");
    // The process's l3 is not running in this test binary.
    EXPECT_FALSE(api::L3::instance().running());
    EXPECT_EQ(api::L3::instance().metrics().status.error, "ESRCH");
}

TEST(L3ApiTest, BlankAndOverlongLinesMatchTheUdpConsole)
{
    api::L3 none;
    EXPECT_EQ(api::l3::console::execute(none, "   "), "");
    EXPECT_EQ(api::l3::console::execute(none, "# comment"), "");
    EXPECT_EQ(api::l3::console::execute(none, std::string(600, 'x')),
              "NOK EINVAL");
    bool called = false;
    api::l3::console::execute(none, "  ",
                              [&](const std::string&)
                              {
                                  called = true;
                              });
    EXPECT_FALSE(called);
}

TEST(L3ApiTest, StopBeforeRunDoesNotLoseTheStopRequest)
{
    FakeRadio radio;
    auto core = api::l3::detail::L3Core::create();
    core->request_stop();
    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(api::l3::detail::run_l3(core, config_for(radio), {}, {}), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - before,
              std::chrono::seconds(1));
    // The request is spent: the core can run again.
    EXPECT_FALSE(core->stop_requested.load());
}

TEST(L3ApiTest, InvalidConfigReturnsOne)
{
    FakeRadio radio;
    Config cfg = config_for(radio);
    cfg.domain = 0;
    auto core = api::l3::detail::L3Core::create();
    EXPECT_EQ(api::l3::detail::run_l3(core, cfg, {}, {}), 1);
}

TEST(L3ApiTest, ThreadSetupRunsBeforeEachThread)
{
    FakeRadio radio;
    auto core = api::l3::detail::L3Core::create();
    std::vector<api::L3::Thread> seen;
    std::thread::id setup_thread;
    auto running = std::async(std::launch::async,
                              [&]()
                              {
                                  return api::l3::detail::run_l3(
                                      core, config_for(radio),
                                      [&](const api::L3&)
                                      {
                                          core->request_stop();
                                      },
                                      [&](api::L3::Thread t)
                                      {
                                          seen.push_back(t);
                                          setup_thread =
                                              std::this_thread::get_id();
                                      });
                              });
    ASSERT_EQ(running.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(running.get(), 0);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], api::L3::Thread::upstream);
    EXPECT_EQ(seen[1], api::L3::Thread::l3);
}

TEST(L3ApiTest, ApplyBuildsTheConfigAndHandlesWork)
{
    FakeRadio radio;
    TestL3 t;
    std::thread::id started_thread;
    ASSERT_TRUE(t.start(config_for(radio),
                        [&](const api::L3&)
                        {
                            started_thread = std::this_thread::get_id();
                        }));
    ASSERT_TRUE(t.radio_up());

    auto list = t.l3.upstreams().list();
    ASSERT_TRUE(list.ok()) << list.status.error;
    ASSERT_EQ(list.value.size(), 1u);
    api::Upstream up = list.value[0];
    EXPECT_EQ(up.id(), 4);
    EXPECT_TRUE(up.valid());
    auto spec = up.spec();
    ASSERT_TRUE(spec.ok());
    EXPECT_EQ(spec.value.bus_tx, 1);
    EXPECT_EQ(spec.value.tx, "127.0.0.1:9");
    EXPECT_EQ(spec.value.quanta, 256u);

    EXPECT_TRUE(up.set_quanta(512).ok());
    EXPECT_EQ(up.quanta().value, 512u);
    api::UpstreamPatch empty;
    EXPECT_EQ(up.update(empty).status.error, "EINVAL");
    api::Fec fec;
    fec.type = api::FecType::rs_block;
    fec.k = 2;
    fec.n = 3;
    EXPECT_TRUE(up.set_fec(fec).ok());
    EXPECT_EQ(up.fec().value.n, 3);
    fec.n = 99;
    EXPECT_EQ(up.set_fec(fec).error, "EINVAL");

    EXPECT_EQ(t.l3.upstreams().remove(9).error, "ENOENT");
    EXPECT_EQ(t.l3.upstreams().get(9).status.error, "ENOENT");

    auto r = t.l3.radios().get();
    ASSERT_TRUE(r.ok());
    api::Radio rad = r.value;
    EXPECT_EQ(rad.domain().value, 0x1234);
    auto info = rad.info();
    ASSERT_TRUE(info.ok()) << info.status.error;
    EXPECT_EQ(info.value.phy.channel, 1);
    EXPECT_EQ(info.value.phy.modulation, "OFDM_24M");
    ASSERT_TRUE(info.value.rssi.has_value());
    EXPECT_EQ(*info.value.rssi, -40);
    ASSERT_TRUE(info.value.version.has_value());
    EXPECT_EQ(info.value.version->x, api::L3::version().x);

    auto caps = rad.caps();
    ASSERT_TRUE(caps.ok());
    EXPECT_EQ(caps.value.fcs, api::RadioFcs::signal);

    auto set = rad.set_channel(6);
    ASSERT_TRUE(set.ok()) << set.status.error;
    EXPECT_EQ(set.value.phy.channel, 6);
    EXPECT_EQ(rad.phy().value.channel, 6);
    EXPECT_EQ(rad.set_modulation("bogus").status.error, "EINVAL");

    auto stats = rad.stats();
    ASSERT_TRUE(stats.ok()) << stats.status.error;
    EXPECT_EQ(stats.value.tx.air_packets, 0u);

    auto metrics = t.l3.metrics({"radio_proto_ok"});
    ASSERT_TRUE(metrics.ok());
    ASSERT_EQ(metrics.value.size(), 1u);
    EXPECT_EQ(metrics.value[0].value, "1");

    EXPECT_EQ(t.l3.radios().create(api::RadioSpec{}).status.error, "EEXIST");
    EXPECT_EQ(t.l3.radios().add(rad).error, "EALREADY");
    EXPECT_EQ(t.l3.radios().remove().error, "EBUSY");
    EXPECT_EQ(t.stop(), 0);
    EXPECT_FALSE(t.l3.running());
    EXPECT_EQ(up.spec().status.error, "ESRCH");
}

// Any thread may call l3: started, Done and post run on the callback
// thread, and blocking calls (radio ones too) work there.
TEST(L3ApiTest, BlockingCallsWorkFromStartedDoneAndPost)
{
    FakeRadio radio;
    TestL3 t;
    std::string from_started;
    std::thread::id callback_thread;
    ASSERT_TRUE(t.start(config_for(radio),
                        [&](const api::L3& l3)
                        {
                            callback_thread = std::this_thread::get_id();
                            auto list = l3.upstreams().list();
                            from_started = list.ok() ? "ok" : list.status.error;
                        }));
    EXPECT_EQ(from_started, "ok");
    ASSERT_TRUE(t.radio_up());
    api::Radio rad = t.l3.radios().get().value;

    std::promise<std::string> got;
    auto result = got.get_future();
    rad.info(
        [&](api::Result<api::RadioInfo> r)
        {
            EXPECT_EQ(std::this_thread::get_id(), callback_thread);
            EXPECT_TRUE(r.ok()) << r.status.error;
            // A blocking radio call from inside a Done.
            auto inner = rad.info(std::chrono::milliseconds(2000));
            std::string text = inner.ok() ? "ok" : inner.status.error;
            text += " " + api::l3::console::execute(t.l3, "ping");
            got.set_value(text);
        });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(result.get(), "ok pong\n");

    std::promise<std::string> posted;
    auto posted_result = posted.get_future();
    ASSERT_TRUE(
        t.l3.post(
                [&](const api::L3& l3)
                {
                    EXPECT_EQ(std::this_thread::get_id(), callback_thread);
                    auto stats = l3.radios().get().value.stats();
                    posted.set_value(stats.ok() ? "ok" : stats.status.error);
                })
            .ok());
    ASSERT_EQ(posted_result.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(posted_result.get(), "ok");
}

// on_queue runs on l3's thread mid-add: an l3 call from it is EDEADLK.
TEST(L3ApiTest, OnQueueRunsOnTheL3ThreadAndCallsAreEdeadlk)
{
    FakeRadio radio;
    TestL3 t;
    ASSERT_TRUE(t.start(config_for(radio)));
    std::string inner;
    std::shared_ptr<UpstreamQueue> queue;
    api::UpstreamSpec spec;
    spec.id = 7;
    spec.bus_tx = 5;
    spec.bus_rx = 5;
    spec.transport = api::Transport::queue;
    const api::L3 l3 = t.l3;
    spec.on_queue = [&](std::shared_ptr<UpstreamQueue> q)
    {
        queue = std::move(q);
        inner = l3.upstreams().list().status.error;
    };
    auto added = t.l3.upstreams().add(spec);
    ASSERT_TRUE(added.ok()) << added.status.error;
    EXPECT_TRUE(queue != nullptr);
    EXPECT_EQ(inner, "EDEADLK");

    // An l3-owned queue upstream without on_queue is invalid.
    spec.id = 8;
    spec.bus_tx = 6;
    spec.bus_rx = 6;
    spec.on_queue = nullptr;
    EXPECT_EQ(t.l3.upstreams().add(spec).status.error, "EINVAL");
}

// ETIMEDOUT leaves the outcome unknown; a request l3 had not started
// is skipped.
TEST(L3ApiTest, TimedOutRequestThatHasNotStartedIsSkipped)
{
    FakeRadio radio;
    TestL3 t;
    ASSERT_TRUE(t.start(config_for(radio)));
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> holding{false};
    api::UpstreamSpec spec;
    spec.id = 7;
    spec.bus_tx = 5;
    spec.bus_rx = 5;
    spec.transport = api::Transport::queue;
    spec.on_queue = [&, released](std::shared_ptr<UpstreamQueue>)
    {
        // Holds l3's thread.
        holding = true;
        released.wait();
    };
    auto adding = std::async(std::launch::async,
                             [&]()
                             {
                                 return t.l3.upstreams().add(spec);
                             });
    ASSERT_TRUE(wait_until(
        [&]()
        {
            return holding.load();
        }));
    std::atomic<bool> ran{false};
    const api::Status timed_out = api::l3::detail::call_status(
        t.core,
        [&](api::l3::detail::L3Impl&)
        {
            ran = true;
            return api::Status{};
        },
        std::chrono::milliseconds(20));
    EXPECT_EQ(timed_out.error, "ETIMEDOUT");
    release.set_value();
    ASSERT_TRUE(adding.get().ok());
    // A later call runs after the skipped one would have.
    EXPECT_TRUE(t.l3.upstreams().get(7).ok());
    EXPECT_FALSE(ran.load());
}

// Requests pending when l3 stops get ECANCELED at once, not after
// their timeout.
TEST(L3ApiTest, PendingRequestsAreCanceledWhenL3Stops)
{
    FakeRadio radio;
    TestL3 t;
    ASSERT_TRUE(t.start(config_for(radio)));
    ASSERT_TRUE(t.radio_up());
    radio.hold("radio_tx_info");
    api::Radio rad = t.l3.radios().get().value;

    std::promise<std::string> async_done;
    auto async_result = async_done.get_future();
    rad.info(
        [&](api::Result<api::RadioInfo> r)
        {
            async_done.set_value(r.status.error);
        });
    auto blocking =
        std::async(std::launch::async,
                   [&]()
                   {
                       return rad.info(std::chrono::seconds(10)).status.error;
                   });
    ASSERT_TRUE(radio.wait_held("radio_tx_info", 2));
    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(t.stop(), 0);
    ASSERT_EQ(async_result.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(async_result.get(), "ECANCELED");
    ASSERT_EQ(blocking.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(blocking.get(), "ECANCELED");
    EXPECT_LT(std::chrono::steady_clock::now() - before,
              std::chrono::seconds(2));
}

TEST(L3ApiTest, SetPhyWhileProgrammingIsEnodev)
{
    FakeRadio radio;
    radio.hold("radio_tx ");  // the startup programming's PHY
    TestL3 t;
    ASSERT_TRUE(t.start(config_for(radio)));
    ASSERT_TRUE(radio.wait_held("radio_tx ", 1));
    api::Radio rad = t.l3.radios().get().value;
    EXPECT_EQ(rad.state().value, api::RadioState::connecting);
    // The m-plane is up but the radio is not programmed: nothing is sent or
    // stored, or the startup radio_tx would undo it.
    EXPECT_EQ(rad.set_channel(6).status.error, "ENODEV");
    EXPECT_EQ(rad.phy().value.channel, 1);
    EXPECT_EQ(radio.seen("radio_tx channel=6"), 0u);

    radio.answer("radio_tx ", 0, true,
                 "radio_tx channel=1 tx_power=20 modulation=OFDM_24M cca=false");
    ASSERT_TRUE(t.radio_up());
}

TEST(L3ApiTest, RadioLifecycle)
{
    FakeRadio radio;
    TestL3 t;
    ASSERT_TRUE(t.start(config_for(radio)));
    ASSERT_TRUE(t.radio_up());
    api::RadioManager radios = t.l3.radios();
    api::Radio rad = radios.get().value;
    const api::RadioSpec spec = rad.spec().value;
    EXPECT_EQ(spec.domain, 0x1234);
    EXPECT_EQ(spec.phy.modulation, "OFDM_24M");

    ASSERT_TRUE(t.l3.upstreams().remove(4).ok());
    ASSERT_TRUE(radios.remove().ok());
    EXPECT_EQ(radios.get().status.error, "ENOENT");
    EXPECT_FALSE(rad.valid());
    EXPECT_EQ(rad.state().status.error, "ENOENT");
    api::UpstreamSpec up;
    up.id = 1;
    up.bus_tx = 1;
    up.tx = "127.0.0.1:9";
    EXPECT_EQ(t.l3.upstreams().add(up).status.error, "ENODEV");

    api::RadioSpec bad = spec;
    bad.domain = 0;
    EXPECT_EQ(radios.create(bad).status.error, "EINVAL");
    bad = spec;
    bad.phy.channel = 14;  // OFDM is not allowed on 14
    EXPECT_EQ(radios.create(bad).status.error, "EINVAL");

    auto created = radios.create(spec);
    ASSERT_TRUE(created.ok()) << created.status.error;
    EXPECT_EQ(created.value.state().value, api::RadioState::stopped);
    // Stopped: a PHY change is stored only.
    auto stored = created.value.set_power(10);
    ASSERT_TRUE(stored.ok()) << stored.status.error;
    EXPECT_TRUE(stored.value.text.empty());
    EXPECT_EQ(created.value.phy().value.power_dbm, 10);
    // Normalized dBm: negative is valid, outside -30..30 is not.
    EXPECT_TRUE(created.value.set_power(-10).ok());
    EXPECT_EQ(created.value.set_power(31).status.error, "EINVAL");
    ASSERT_TRUE(created.value.set_power(10).ok());
    EXPECT_EQ(created.value.info().status.error, "ENODEV");
    ASSERT_TRUE(t.l3.upstreams().add(up).ok());
    ASSERT_TRUE(radios.add(created.value).ok());
    ASSERT_TRUE(t.radio_up());
    EXPECT_GE(radio.seen("radio_tx channel=1 tx_power=10"), 1u);
}

TEST(L3ApiTest, SetDevice)
{
    FakeRadio radio;
    TestL3 t;
    ASSERT_TRUE(t.start(config_for(radio)));
    ASSERT_TRUE(t.radio_up());
    api::Radio rad = t.l3.radios().get().value;
    api::RadioDevice d = rad.device().value;
    EXPECT_EQ(d.fcs, api::RadioFcs::signal);
    api::RadioDevice cleared = d;
    cleared.mplane = {};
    EXPECT_EQ(rad.set_device(cleared).error, "EINVAL");
    api::RadioDevice queue = d;
    queue.dplane_tx = "x";
    EXPECT_EQ(rad.set_device(queue).error, "EINVAL");

    // A new m-plane address reconnects.
    FakeRadio other;
    d.mplane = other.mplane();
    ASSERT_TRUE(rad.set_device(d).ok());
    EXPECT_TRUE(wait_until(
        [&]()
        {
            return other.seen("version") >= 1;
        },
        3000));
    EXPECT_TRUE(t.radio_up(3000));
}

TEST(L3ApiTest, StatsFormatRoundTripsTheRadioLines)
{
    const std::vector<std::string> lines = {
        "tx_info tx_queue_sz=3 in_flight=2 dropped_invalid_frame=4294967295 "
        "dropped_tx_queue=0 dropped_wifi=1 ether_pkt=100 air_pkt=99 "
        "ts=123456789",
        "rx_info rx_queue_sz=1 dropped_filter_mismatched=14 dropped_rx_queue=0 "
        "dropped_no_peer=2 dropped_send_failed=0 ether_pkt=7 air_pkt=8 ts=5"};
    RadioCounters c;
    ASSERT_TRUE(parse_radio_counters(lines, &c));
    api::RadioStats s;
    s.tx.queue_size = c.tx.queue_size;
    s.tx.in_flight = c.tx.in_flight;
    s.tx.dropped_invalid_frame = c.tx.dropped_invalid_frame;
    s.tx.dropped_queue = c.tx.dropped_queue;
    s.tx.dropped_wifi = c.tx.dropped_wifi;
    s.tx.ether_packets = c.tx.ether_packets;
    s.tx.air_packets = c.tx.air_packets;
    s.tx.timestamp_us = c.tx.timestamp_us;
    s.rx.queue_size = c.rx.queue_size;
    s.rx.dropped_filter_mismatched = c.rx.dropped_filter_mismatched;
    s.rx.dropped_queue = c.rx.dropped_queue;
    s.rx.dropped_no_peer = c.rx.dropped_no_peer;
    s.rx.dropped_send_failed = c.rx.dropped_send_failed;
    s.rx.ether_packets = c.rx.ether_packets;
    s.rx.air_packets = c.rx.air_packets;
    s.rx.timestamp_us = c.rx.timestamp_us;
    EXPECT_EQ(api::l3::console::format(s), lines[0] + "\n" + lines[1]);
}

TEST(L3ApiTest, ServiceStartErrors)
{
    FakeRadio radio;
    TestL3 t;
    ASSERT_TRUE(t.start(config_for(radio)));
    api::l3::console::Service service;
    EXPECT_EQ(service.start(t.l3, "127.0.0.1:1", "no-such-reactor").error,
              "ENOENT");
    EXPECT_EQ(service.start(t.l3, "nonsense", "no-such-reactor").error,
              "EINVAL");
    ConsoleReactor reactor;
    const uint16_t port = reserve_free_udp_port();
    const std::string at = "127.0.0.1:" + std::to_string(port);
    ASSERT_TRUE(service.start(t.l3, at, reactor.name).ok());
    EXPECT_EQ(service.start(t.l3, at, reactor.name).error, "EALREADY");
    std::string on_thread;
    ASSERT_TRUE(reactor.on_reactor(
        [&]()
        {
            on_thread = service.stop().error;
        }));
    EXPECT_EQ(on_thread, "EDEADLK");
    EXPECT_TRUE(service.stop().ok());
}

}  // namespace
