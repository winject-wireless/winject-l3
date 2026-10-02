#include "console/ConsoleService.h"
#include "console/ManagerConsoleTypes.h"
#include "endpoint/UdpEndpoint.h"
#include "radio/WifiFcs.h"
#include "radio/WifiUdp.h"
#include "utils/IOReactor.h"

#include <arpa/inet.h>
#include <bfc/socket.hpp>
#include <chrono>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace winject
{
namespace
{

uint16_t reserve_free_udp_port()
{
    bfc::socket probe(bfc::create_udp4());
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (probe.bind(addr) < 0)
    {
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(probe.fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0)
    {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool run_reactor_with_watchdog(IOReactor& reactor,
                               const std::function<void()>& unblock)
{
    reactor.get_timer().wait_ms(50,
                                [&]()
                                {
                                    reactor.stop();
                                });
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    std::thread worker(
        [&]()
        {
            reactor.run();
            done.set_value();
        });
    const bool ok =
        finished.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!ok)
    {
        unblock();
    }
    worker.join();
    return ok;
}

ManagerConsoleHandlers stub_console_handlers()
{
    ManagerConsoleHandlers h;
    h.add_upstream = [](const ManagerUpstreamView&, std::string*) -> bool
    {
        return false;
    };
    h.remove_upstream = [](uint8_t, std::string*) -> bool
    {
        return false;
    };
    h.list_upstream = [](const std::vector<uint8_t>&,
                         std::vector<ManagerUpstreamView>*,
                         std::string*) -> bool
    {
        return false;
    };
    h.update_upstream = [](const ManagerUpstreamUpdate&, ManagerUpstreamView*,
                           std::string*) -> bool
    {
        return false;
    };
    h.list_upstream_rx_stat = [](const std::vector<uint8_t>&,
                                 std::vector<ManagerUpstreamRxStatView>*,
                                 std::string*) -> bool
    {
        return false;
    };
    h.list_upstream_tx_stat = [](const std::vector<uint8_t>&,
                                 std::vector<ManagerUpstreamTxStatView>*,
                                 std::string*) -> bool
    {
        return false;
    };
    h.get_metrics = [](const std::vector<std::string>&,
                       std::vector<ManagerMetricView>*, std::string*) -> bool
    {
        return false;
    };
    h.radio_info = [](ManagerConsoleReply) {};
    h.radio_caps_info = [](ManagerConsoleReply) {};
    h.radio_stats = [](ManagerConsoleReply) {};
    h.radio_tx = [](const ManagerRadioUpdate&, ManagerConsoleReply) {};
    h.radio_reset = [](uint8_t, ManagerConsoleReply) {};
    h.config_slot = [](uint8_t, ManagerConsoleReply) {};
    return h;
}

}  // namespace

TEST(ReactorDrainTest, UdpEndpointDrainDoesNotBlock)
{
    const uint16_t port = reserve_free_udp_port();
    ASSERT_NE(port, 0u);

    IOReactor reactor;
    UdpEndpoint endpoint;
    UpstreamConfig cfg;
    cfg.fec_type = FecType::none;
    cfg.endpoint.rx = "127.0.0.1:" + std::to_string(port);
    std::string err;
    ASSERT_TRUE(endpoint.open(reactor, cfg));

    bfc::socket client(bfc::create_udp4());
    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(port);
    std::vector<uint8_t> payload(100, 0xAB);
    const bfc::const_buffer_view view(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    ASSERT_EQ(client.send(view, 0, reinterpret_cast<const sockaddr*>(&dest),
                          sizeof(dest)),
              static_cast<ssize_t>(payload.size()));

    const bool ok = run_reactor_with_watchdog(
        reactor,
        [&]()
        {
            client.send(view, 0, reinterpret_cast<const sockaddr*>(&dest),
                        sizeof(dest));
        });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(endpoint.has_tx());
    EXPECT_EQ(endpoint.get_tx_size(), payload.size());
    endpoint.close();
}

TEST(ReactorDrainTest, UdpEndpointDrainIsBounded)
{
    const uint16_t port = reserve_free_udp_port();
    ASSERT_NE(port, 0u);

    IOReactor reactor;
    UdpEndpoint endpoint;
    UpstreamConfig cfg;
    cfg.fec_type = FecType::none;
    cfg.endpoint.rx = "127.0.0.1:" + std::to_string(port);
    ASSERT_TRUE(endpoint.open(reactor, cfg));

    bfc::socket client(bfc::create_udp4());
    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(port);
    const std::vector<uint8_t> one_byte = {0x01};
    const bfc::const_buffer_view one_view(
        reinterpret_cast<const std::byte*>(one_byte.data()), one_byte.size());
    for (int i = 0; i < 20; ++i)
    {
        ASSERT_EQ(
            client.send(one_view, 0, reinterpret_cast<const sockaddr*>(&dest),
                        sizeof(dest)),
            1);
    }

    const bool ok = run_reactor_with_watchdog(
        reactor,
        [&]()
        {
            client.send(one_view, 0, reinterpret_cast<const sockaddr*>(&dest),
                        sizeof(dest));
        });
    EXPECT_TRUE(ok);
    uint64_t pkt = 0;
    uint64_t byt = 0;
    endpoint.tx_pending_stats(&pkt, &byt);
    EXPECT_EQ(pkt, 20u);
    EXPECT_EQ(byt, 20u);
    endpoint.close();
}

TEST(ReactorDrainTest, ConsoleServiceDrainDoesNotBlock)
{
    const uint16_t in_port = reserve_free_udp_port();
    const uint16_t out_port = reserve_free_udp_port();
    ASSERT_NE(in_port, 0u);
    ASSERT_NE(out_port, 0u);

    IOReactor reactor;
    ConsoleService service;
    sockaddr_in console_in = {};
    console_in.sin_family = AF_INET;
    console_in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    console_in.sin_port = htons(in_port);
    sockaddr_in console_out = {};
    console_out.sin_family = AF_INET;
    console_out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    console_out.sin_port = htons(out_port);

    std::string err;
    ASSERT_TRUE(service.start(reactor, console_in, console_out,
                              stub_console_handlers(), &err));

    bfc::socket reply_sock(bfc::create_udp4());
    sockaddr_in reply_bind = console_out;
    ASSERT_EQ(reply_sock.bind(reply_bind), 0);

    bfc::socket client(bfc::create_udp4());
    const char ping[] = "ping\n";
    const bfc::const_buffer_view ping_view(
        reinterpret_cast<const std::byte*>(ping), sizeof(ping) - 1);
    ASSERT_GT(client.send(ping_view, 0,
                          reinterpret_cast<const sockaddr*>(&console_in),
                          sizeof(console_in)),
              0);

    const bool ok = run_reactor_with_watchdog(
        reactor,
        [&]()
        {
            client.send(ping_view, 0,
                        reinterpret_cast<const sockaddr*>(&console_in),
                        sizeof(console_in));
        });
    EXPECT_TRUE(ok);

    char buf[64] = {};
    sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    const ssize_t n =
        ::recvfrom(reply_sock.fd(), buf, sizeof(buf), MSG_DONTWAIT,
                   reinterpret_cast<sockaddr*>(&from), &from_len);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    EXPECT_STREQ(buf, "pong\n");
    service.stop();
}

TEST(ReactorDrainTest, WifiUdpDrainDoesNotBlock)
{
    const uint16_t fwd_port = reserve_free_udp_port();
    ASSERT_NE(fwd_port, 0u);

    bfc::socket radio_sock(bfc::create_udp4());
    sockaddr_in radio_bind = {};
    radio_bind.sin_family = AF_INET;
    radio_bind.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    radio_bind.sin_port = htons(fwd_port);
    ASSERT_EQ(radio_sock.bind(radio_bind), 0);

    IOReactor reactor;
    WifiUdp wifi;
    sockaddr_in inject = {};
    inject.sin_family = AF_INET;
    inject.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    inject.sin_port = htons(19000);

    int rx_calls = 0;
    const auto on_rx = [&](bfcext::shared_sized_buffer)
    {
        ++rx_calls;
    };

    ASSERT_TRUE(wifi.open(reactor, inject, fwd_port, on_rx));
    wifi.set_fcs_mode(winject::RadioFcsMode::actual);

    uint8_t reg_buf[8] = {};
    sockaddr_in manager_addr = {};
    socklen_t manager_len = sizeof(manager_addr);
    const ssize_t reg_n =
        ::recvfrom(radio_sock.fd(), reg_buf, sizeof(reg_buf), 0,
                   reinterpret_cast<sockaddr*>(&manager_addr), &manager_len);
    ASSERT_EQ(reg_n, 1);

    std::vector<uint8_t> mpdu(28, 0);
    mpdu[0] = 0x08;
    mpdu[1] = 0x01;
    uint8_t fcs[4];
    winject::wifi_fcs_store(mpdu.data(), mpdu.size(), fcs);
    mpdu.insert(mpdu.end(), fcs, fcs + 4);

    const bfc::const_buffer_view frame_view(
        reinterpret_cast<const std::byte*>(mpdu.data()), mpdu.size());
    ASSERT_EQ(radio_sock.send(frame_view, 0,
                              reinterpret_cast<const sockaddr*>(&manager_addr),
                              manager_len),
              static_cast<ssize_t>(mpdu.size()));

    const bool ok = run_reactor_with_watchdog(
        reactor,
        [&]()
        {
            radio_sock.send(frame_view, 0,
                            reinterpret_cast<const sockaddr*>(&manager_addr),
                            manager_len);
        });
    EXPECT_TRUE(ok);
    EXPECT_EQ(rx_calls, 1);
    wifi.close();
}

}  // namespace winject
