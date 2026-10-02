#include "radio/RadioDefs.h"
#include "radio/WifiFcs.h"
#include "radio/WifiUdp.h"
#include "utils/IOReactor.h"

#include <bfc/socket.hpp>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

using namespace winject;

namespace
{

uint16_t reserve_udp_port()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return 0;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return 0;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    const uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

void send_forward_frame(bfc::socket& radio_sock, const sockaddr* manager_addr,
                        socklen_t manager_len, std::vector<uint8_t> frame)
{
    const bfc::const_buffer_view view(
        reinterpret_cast<const std::byte*>(frame.data()), frame.size());
    ASSERT_EQ(radio_sock.send(view, 0, manager_addr, manager_len),
              static_cast<ssize_t>(frame.size()));
}

bool run_reactor_brief(IOReactor& reactor)
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
    worker.join();
    return ok;
}

}  // namespace

TEST(WifiUdpTest, SignalModeAcceptsZeroTrailer)
{
    const uint16_t fwd_port = reserve_udp_port();
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
    inject.sin_port = htons(19001);

    int rx_calls = 0;
    ASSERT_TRUE(wifi.open(reactor, inject, fwd_port,
                          [&](bfcext::shared_sized_buffer)
                          {
                              ++rx_calls;
                          }));
    wifi.set_fcs_mode(RadioFcsMode::signal);

    sockaddr_in manager_addr = {};
    socklen_t manager_len = sizeof(manager_addr);
    uint8_t reg_buf[8] = {};
    ASSERT_EQ(
        ::recvfrom(radio_sock.fd(), reg_buf, sizeof(reg_buf), 0,
                   reinterpret_cast<sockaddr*>(&manager_addr), &manager_len),
        1);

    std::vector<uint8_t> frame(28, 0);
    frame[0] = 0x08;
    frame[1] = 0x01;
    send_forward_frame(radio_sock,
                       reinterpret_cast<const sockaddr*>(&manager_addr),
                       manager_len, frame);

    EXPECT_TRUE(run_reactor_brief(reactor));
    EXPECT_EQ(rx_calls, 1);
    wifi.close();
}

TEST(WifiUdpTest, SignalModeRejectsNonZeroTrailer)
{
    const uint16_t fwd_port = reserve_udp_port();
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
    inject.sin_port = htons(19002);

    int rx_calls = 0;
    ASSERT_TRUE(wifi.open(reactor, inject, fwd_port,
                          [&](bfcext::shared_sized_buffer)
                          {
                              ++rx_calls;
                          }));
    wifi.set_fcs_mode(RadioFcsMode::signal);

    sockaddr_in manager_addr = {};
    socklen_t manager_len = sizeof(manager_addr);
    uint8_t reg_buf[8] = {};
    ASSERT_EQ(
        ::recvfrom(radio_sock.fd(), reg_buf, sizeof(reg_buf), 0,
                   reinterpret_cast<sockaddr*>(&manager_addr), &manager_len),
        1);

    std::vector<uint8_t> frame(28, 0);
    frame[0] = 0x08;
    frame[1] = 0x01;
    frame[24] = 0xff;
    frame[25] = 0xff;
    frame[26] = 0xff;
    frame[27] = 0xff;
    send_forward_frame(radio_sock,
                       reinterpret_cast<const sockaddr*>(&manager_addr),
                       manager_len, frame);

    EXPECT_TRUE(run_reactor_brief(reactor));
    EXPECT_EQ(rx_calls, 0);
    EXPECT_EQ(wifi.peek_counters().fcs_error_pkt, 1u);
    wifi.close();
}

TEST(WifiUdpTest, UnknownModeDropsAndCounts)
{
    const uint16_t fwd_port = reserve_udp_port();
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
    inject.sin_port = htons(19003);

    ASSERT_TRUE(wifi.open(reactor, inject, fwd_port, {}));

    sockaddr_in manager_addr = {};
    socklen_t manager_len = sizeof(manager_addr);
    uint8_t reg_buf[8] = {};
    ASSERT_EQ(
        ::recvfrom(radio_sock.fd(), reg_buf, sizeof(reg_buf), 0,
                   reinterpret_cast<sockaddr*>(&manager_addr), &manager_len),
        1);

    std::vector<uint8_t> frame(28, 0);
    frame[0] = 0x08;
    frame[1] = 0x01;
    uint8_t fcs[4];
    wifi_fcs_store(frame.data(), 24, fcs);
    frame.insert(frame.end(), fcs, fcs + 4);
    send_forward_frame(radio_sock,
                       reinterpret_cast<const sockaddr*>(&manager_addr),
                       manager_len, frame);

    EXPECT_TRUE(run_reactor_brief(reactor));
    EXPECT_EQ(wifi.peek_counters().fcs_unknown_pkt, 1u);
    wifi.close();
}

TEST(WifiUdpTest, SendEnforcesInjectMpduMax)
{
    const uint16_t fwd_port = reserve_udp_port();
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
    inject.sin_port = htons(19004);
    ASSERT_TRUE(wifi.open(reactor, inject, fwd_port, {}));

    std::vector<uint8_t> mpdu(WIFI_RADIO_INJECT_MAX, 0x08);
    EXPECT_TRUE(wifi.send(mpdu.data(), mpdu.size()));
    EXPECT_FALSE(wifi.send(mpdu.data(), mpdu.size() + 1));

    wifi.close();
}
