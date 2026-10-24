#include "App.h"
#include "ConsoleTestHelpers.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"

#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/socket.hpp>
#include <future>
#include <gtest/gtest.h>
#include <poll.h>
#include <thread>

using namespace winject;
namespace
{
Config config_for(const sockaddr_in& radio)
{
    Config cfg;
    cfg.domain = 0x1234;
    cfg.modulation = "OFDM_24M";
    cfg.radio_device.dplane = radio;
    cfg.radio_device.mplane = radio;
    cfg.radio_device.fcs = RadioFcsConfig::signal;
    return cfg;
}

TEST(AppShutdownTest, StopBeforeRunDoesNotLoseTheStopRequest)
{
    sockaddr_in radio{};
    radio.sin_family = AF_INET;
    radio.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    radio.sin_port = htons(test::reserve_free_udp_port());
    Config cfg = config_for(radio);
    UpstreamConfig up;
    up.mode = UpstreamMode::udp_client;
    up.endpoint.tx = "127.0.0.1:9";
    up.bus_tx = 1;
    up.bus_rx = 2;
    cfg.upstreams.push_back(up);
    App app;
    ASSERT_TRUE(app.load(cfg));
    app.stop();
    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(app.run(), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - before,
              std::chrono::seconds(1));
}

TEST(AppShutdownTest, StopDropsPartialFecUsingSharedReactor)
{
    using Queue = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
    using Registry =
        bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;
    auto in = std::make_shared<Queue>();
    auto out = std::make_shared<Queue>();
    auto cv = std::make_shared<bfc::cv_reactor<>>();
    in->set_capacity(8);
    out->set_capacity(8);
    ASSERT_TRUE(Registry::get_instance().add("drain-in", in));
    ASSERT_TRUE(Registry::get_instance().add("drain-out", out));
    ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add("drain-cv", cv));
    bfc::socket radio(bfc::create_udp4());
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(radio.bind(addr), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(radio.fd(), reinterpret_cast<sockaddr*>(&addr), &len),
              0);
    Config cfg = config_for(addr);
    UpstreamConfig up;
    up.mode = UpstreamMode::queue;
    up.bus_tx = 1;
    up.bus_rx = 2;
    up.endpoint = {"drain-in", "drain-out"};
    up.reactor = "drain-cv";
    up.fec_type = FecType::RsBlockErasure;
    up.fec_k = 2;
    up.fec_n = 3;
    up.fec_timeout_ms = 0;
    cfg.upstreams.push_back(up);
    {
        App app;
        ASSERT_TRUE(app.load(cfg));
        uint8_t p = 7;
        in->push(bfc::shared_sized_buffer::copy_from(&p, 1));
        std::thread cv_thread(
            [&]()
            {
                cv->run();
            });
        // Constructing the upstream adds its handlers and wakes the CV reactor;
        // wait for input consumption rather than relying on thread startup
        // timing.
        auto result = std::async(std::launch::async,
                                 [&]()
                                 {
                                     return app.run();
                                 });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (in->has_data() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool did_consume = !in->has_data();
        // Fence the current callback to ensure encoding finished before stop.
        std::promise<void> fence;
        auto fenced = fence.get_future();
        cv->wake_up(
            [&]()
            {
                fence.set_value();
            });
        const bool completed = fenced.wait_for(std::chrono::seconds(2)) ==
                               std::future_status::ready;
        app.stop();
        const auto finished = result.wait_for(std::chrono::seconds(2));
        if (finished != std::future_status::ready)
        {
            app.stop();
        }
        EXPECT_TRUE(did_consume);
        EXPECT_TRUE(completed);
        EXPECT_EQ(finished, std::future_status::ready);
        EXPECT_EQ(result.get(), 0);
        cv->stop();
        cv_thread.join();
        size_t shards = 0;
        uint8_t buffer[2048];
        for (;;)
        {
            const auto n =
                ::recv(radio.fd(), buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n <= 0)
            {
                break;
            }
            // Ignore one-byte forward registration and m-plane requests.
            Mpdu mpdu(buffer, static_cast<size_t>(n));
            if (!mpdu.rescan() || !mpdu.is_valid_winject_frame())
            {
                continue;
            }
            for (uint8_t slot = 0; slot < WIFI_PDU_SLOTS; ++slot)
            {
                const auto pdu =
                    static_cast<const Mpdu&>(mpdu).get_slot_payload(slot);
                if (pdu.empty())
                {
                    continue;
                }
                EXPECT_TRUE(LCHeader::read_is_fec(
                    reinterpret_cast<const uint8_t*>(pdu.data())));
                ++shards;
            }
        }
        EXPECT_EQ(shards, 0u);
    }
    bfc::cv_reactor_registry::get_instance().rem("drain-cv");
    Registry::get_instance().rem("drain-in");
    Registry::get_instance().rem("drain-out");
}
}  // namespace

namespace
{
class AppUpstreamReplacementTest : public ::testing::TestWithParam<bool>
{
};

TEST_P(AppUpstreamReplacementTest,
       FecUpdateReconstructsEndpointAndKeepsForwarding)
{
    using Queue = ReactiveLossyQueue;
    using Registry =
        bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;
    auto input = std::make_shared<Queue>();
    auto output = std::make_shared<Queue>();
    auto cv = std::make_shared<Upstream::Reactor>();
    input->set_capacity(16);
    output->set_capacity(16);
    ASSERT_TRUE(Registry::get_instance().add("replace-in", input));
    ASSERT_TRUE(Registry::get_instance().add("replace-out", output));
    ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add("replace-cv", cv));
    struct Cleanup
    {
        std::shared_ptr<Upstream::Reactor> cv;
        std::thread thread;
        ~Cleanup()
        {
            cv->stop();
            thread.join();
            Registry::get_instance().rem("replace-in");
            Registry::get_instance().rem("replace-out");
            bfc::cv_reactor_registry::get_instance().rem("replace-cv");
        }
    } cleanup{cv, std::thread(
                      [cv]
                      {
                          cv->run();
                      })};
    std::promise<void> started;
    auto ready = started.get_future();
    cv->wake_up(
        [&started]
        {
            started.set_value();
        });
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);

    bfc::socket radio(bfc::create_udp4());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(radio.bind(address), 0);
    socklen_t len = sizeof(address);
    ASSERT_EQ(
        getsockname(radio.fd(), reinterpret_cast<sockaddr*>(&address), &len),
        0);
    auto config = config_for(address);
    auto console_port = test::reserve_free_udp_port();
    auto input_port = test::reserve_free_udp_port();
    config.console_in = "127.0.0.1:" + std::to_string(console_port);
    UpstreamConfig upstream;
    upstream.bus_tx = 1;
    upstream.bus_rx = 2;
    upstream.fec_type = FecType::RsBlockErasure;
    upstream.fec_k = 3;
    upstream.fec_n = 4;
    upstream.fec_timeout_ms = 0;
    if (GetParam())
    {
        upstream.mode = UpstreamMode::queue;
        upstream.endpoint = {"replace-in", "replace-out"};
        upstream.reactor = "replace-cv";
    }
    else
    {
        upstream.mode = UpstreamMode::udp_server;
        upstream.endpoint.rx = "127.0.0.1:" + std::to_string(input_port);
    }
    config.upstreams.push_back(upstream);
    App app;
    ASSERT_TRUE(app.load(config));
    auto running = std::async(std::launch::async,
                              [&]
                              {
                                  return app.run();
                              });
    struct Stop
    {
        App& app;
        ~Stop()
        {
            app.stop();
        }
    } stop{app};
    bfc::socket client(bfc::create_udp4());
    auto exchange = [&](const std::string& command)
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dest.sin_port = htons(console_port);
        std::string reply;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            ::sendto(client.fd(), command.data(), command.size(), 0,
                     reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
            pollfd fd{client.fd(), POLLIN, 0};
            if (::poll(&fd, 1, 50) != 1)
            {
                continue;
            }
            char buffer[4096];
            const auto n =
                ::recv(client.fd(), buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n > 0)
            {
                reply.assign(buffer, size_t(n));
                break;
            }
        }
        return reply;
    };
    ASSERT_EQ(exchange("cmd:1 ping").find("OK:1"), 0u);
    auto send_input = [&]
    {
        const uint8_t value = 7;
        if (GetParam())
        {
            input->push(bfc::shared_sized_buffer::copy_from(&value, 1));
        }
        else
        {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            dest.sin_port = htons(input_port);
            EXPECT_EQ(
                ::sendto(client.fd(), &value, 1, 0,
                         reinterpret_cast<sockaddr*>(&dest), sizeof(dest)),
                1);
        }
    };
    auto wait_partial = [&]
    {
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            if (exchange("cmd:2 lut").find("tx_pending_pkt=1") !=
                std::string::npos)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    send_input();
    ASSERT_TRUE(wait_partial());
    if (GetParam())
    {
        // A host can temporarily remove a registry binding. Both replacement
        // and rollback then fail; the next valid update must recover the slot.
        Registry::get_instance().rem("replace-in");
        const auto unavailable = exchange("cmd:9 uu id=0 fec=BLOCK k=2 n=3");
        EXPECT_EQ(unavailable.find("NOK:9 EIO"), 0u) << unavailable;
        ASSERT_TRUE(Registry::get_instance().add("replace-in", input));
    }
    auto reply = exchange("cmd:3 uu id=0 fec=BLOCK k=2 n=3 quanta=512");
    ASSERT_EQ(reply.find("OK:3"), 0u) << reply;
    // Replacement discards the old partial block; the first new input remains
    // pending instead of completing the previous encoder's block.
    send_input();
    ASSERT_TRUE(wait_partial());
    reply = exchange("cmd:4 uu id=0 fec=BLOCK k=4 n=4");
    EXPECT_EQ(reply.find("NOK:4"), 0u) << reply;
    send_input();
    size_t shards = 0;
    for (int attempt = 0; attempt < 100 && shards < 3; ++attempt)
    {
        pollfd fd{radio.fd(), POLLIN, 0};
        if (::poll(&fd, 1, 10) != 1)
        {
            continue;
        }
        uint8_t buffer[2048];
        const auto n = ::recv(radio.fd(), buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n <= 0)
        {
            continue;
        }
        Mpdu mpdu(buffer, size_t(n));
        if (!mpdu.rescan() || !mpdu.is_valid_winject_frame())
        {
            continue;
        }
        for (uint8_t slot = 0; slot < WIFI_PDU_SLOTS; ++slot)
        {
            auto pdu = static_cast<const Mpdu&>(mpdu).get_slot_payload(slot);
            if (!pdu.empty())
            {
                EXPECT_TRUE(LCHeader::read_is_fec(
                    reinterpret_cast<const uint8_t*>(pdu.data())));
                ++shards;
            }
        }
    }
    EXPECT_EQ(shards, 3u);
    app.stop();
    ASSERT_EQ(running.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(running.get(), 0);
}
INSTANTIATE_TEST_SUITE_P(Transports, AppUpstreamReplacementTest,
                         ::testing::Bool());
}  // namespace
