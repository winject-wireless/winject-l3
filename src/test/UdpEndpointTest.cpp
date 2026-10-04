#include "Config.h"
#include "endpoint/UdpEndpoint.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <bfc/socket.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <thread>
#include <vector>

using namespace winject;

namespace
{

uint16_t reserve_free_udp_port()
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

void pump_reactor(IOReactor& reactor)
{
    reactor.get_timer().wait_ms(100,
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
    ASSERT_EQ(finished.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    worker.join();
}

}  // namespace

TEST(UdpEndpointTest, DropsOversizeAppDatagram)
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

    std::vector<uint8_t> ok(k_stream_payload_max, 0xAB);
    const bfc::const_buffer_view ok_view(
        reinterpret_cast<const std::byte*>(ok.data()), ok.size());
    ASSERT_EQ(client.send(ok_view, 0, reinterpret_cast<const sockaddr*>(&dest),
                          sizeof(dest)),
              static_cast<ssize_t>(ok.size()));

    std::vector<uint8_t> big(k_stream_payload_max + 1, 0xCD);
    const bfc::const_buffer_view big_view(
        reinterpret_cast<const std::byte*>(big.data()), big.size());
    ASSERT_EQ(client.send(big_view, 0, reinterpret_cast<const sockaddr*>(&dest),
                          sizeof(dest)),
              static_cast<ssize_t>(big.size()));

    pump_reactor(reactor);
    EXPECT_EQ(endpoint.app_rx_oversize_pkt(), 1u);
    EXPECT_TRUE(endpoint.has_tx());
    EXPECT_EQ(endpoint.get_tx_size(), k_stream_payload_max);

    endpoint.close();
}

// Mirrors App::console_remove_upstream: the endpoint is destroyed from another
// reactor callback while its own read event is pending in the same epoll
// batch. Its callback must not run on the freed object.
TEST(UdpEndpointTest, DestroyFromCallbackWithPendingRead)
{
    const uint16_t port = reserve_free_udp_port();
    ASSERT_NE(port, 0u);

    IOReactor reactor;
    auto endpoint = std::make_unique<UdpEndpoint>();
    UpstreamConfig cfg;
    cfg.fec_type = FecType::none;
    cfg.endpoint.rx = "127.0.0.1:" + std::to_string(port);
    ASSERT_TRUE(endpoint->open(reactor, cfg));

    // Readable before the endpoint so epoll reports it first in the batch.
    const int trigger = eventfd(1, EFD_NONBLOCK);
    ASSERT_GE(trigger, 0);
    ASSERT_TRUE(reactor.add_read_rdy(trigger,
                                     [&]()
                                     {
                                         uint64_t v;
                                         auto res [[maybe_unused]] =
                                             read(trigger, &v, sizeof(v));
                                         endpoint.reset();
                                     }));

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

    pump_reactor(reactor);
    EXPECT_EQ(endpoint, nullptr);

    reactor.rem_read_rdy(trigger);
    close(trigger);
}
