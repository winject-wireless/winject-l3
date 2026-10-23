#include "App.h"
#include "ConsoleTestHelpers.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"

#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/socket.hpp>
#include <future>
#include <gtest/gtest.h>
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

TEST(AppShutdownTest, StopFlushesPartialFecUsingSharedReactor)
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
        // Opening the endpoint adds the handler and wakes the CV reactor; wait
        // for input consumption rather than relying on thread startup timing.
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
        EXPECT_EQ(shards, 2u);
    }
    bfc::cv_reactor_registry::get_instance().rem("drain-cv");
    Registry::get_instance().rem("drain-in");
    Registry::get_instance().rem("drain-out");
}
}  // namespace
