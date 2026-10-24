#include "ConsoleTestHelpers.h"
#include "radio/RxEvent.h"
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
using test::reserve_free_udp_port;
using test::run_reactor_with_watchdog;
using test::stub_console_handlers;

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
    sockaddr_in dplane = {};
    dplane.sin_family = AF_INET;
    dplane.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dplane.sin_port = htons(fwd_port);

    int rx_calls = 0;
    const auto on_rx = [&](bfc::shared_sized_buffer)
    {
        ++rx_calls;
    };

    ASSERT_TRUE(wifi.open(reactor, on_rx));
    wifi.set_tx_dplane(dplane);
    wifi.post_rx_event(winject::EventCtrlRecvSocketChange{dplane});
    ASSERT_TRUE(run_reactor_with_watchdog(reactor, []() {}));
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
