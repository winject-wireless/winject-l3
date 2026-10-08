#include "Config.h"
#include "endpoint/UdpEndpoint.h"
#include "fec/RsBlockErasure.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <bfc/socket.hpp>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
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

// The application side of a static upstream: bound on its own port, it
// receives what the endpoint delivers from the radio.
class AppPeer
{
public:
    AppPeer() : sock_(bfc::create_udp4())
    {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(sock_.bind(addr), 0);
        socklen_t len = sizeof(addr);
        getsockname(sock_.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    std::string addr() const
    {
        return "127.0.0.1:" + std::to_string(port_);
    }

    // Next datagram, or empty after 200 ms.
    std::vector<uint8_t> recv()
    {
        pollfd pfd = {sock_.fd(), POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0)
        {
            return {};
        }
        std::vector<uint8_t> buf(2048);
        const ssize_t n = ::recv(sock_.fd(), buf.data(), buf.size(), 0);
        buf.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return buf;
    }

private:
    bfc::socket sock_;
    uint16_t port_ = 0;
};

bfcext::shared_sized_buffer air(const std::vector<uint8_t>& bytes)
{
    return bfcext::shared_sized_buffer::copy_from(bytes.data(), bytes.size());
}

}  // namespace

// Only the LC header FEC flag selects the decoder. A raw SDU that looks like
// an FEC shard (old 0xF1 magic, or a valid 5-byte shard header) is delivered
// unchanged.
TEST(UdpEndpointTest, RawSlotIsNeverDecoded)
{
    const uint16_t port = reserve_free_udp_port();
    ASSERT_NE(port, 0u);
    AppPeer app;

    IOReactor reactor;
    UdpEndpoint endpoint;
    UpstreamConfig cfg;
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 1;
    cfg.fec_n = 2;
    cfg.endpoint.rx = "127.0.0.1:" + std::to_string(port);
    cfg.endpoint.tx = app.addr();
    ASSERT_TRUE(endpoint.open(reactor, cfg));

    std::vector<uint8_t> shard_like(RsBlockErasure::k_header_len + 4);
    ASSERT_TRUE(RsBlockErasure::pack_header(shard_like.data(), 0, 0, 1, 2, 1));
    const std::vector<std::vector<uint8_t>> raws = {
        {0xF1, 0x02, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 'v'},
        {0xF1, 0x00, 0x90, 0x02},
        shard_like,
    };
    for (const auto& raw : raws)
    {
        endpoint.on_radio_rx(air(raw), false);
        EXPECT_EQ(app.recv(), raw);
    }
    EXPECT_EQ(endpoint.fec_air_rx_packets(), 0u);
    EXPECT_EQ(endpoint.app_rx_packets(), raws.size());
    endpoint.close();
}

// Flagged slots are decoded from their shard header even when this upstream
// has FEC disabled, and the mismatch is counted.
TEST(UdpEndpointTest, FecSlotDecodesWithFecDisabled)
{
    const uint16_t port = reserve_free_udp_port();
    ASSERT_NE(port, 0u);
    AppPeer app;

    IOReactor reactor;
    UdpEndpoint endpoint;
    UpstreamConfig cfg;
    cfg.fec_type = FecType::none;
    cfg.endpoint.rx = "127.0.0.1:" + std::to_string(port);
    cfg.endpoint.tx = app.addr();
    ASSERT_TRUE(endpoint.open(reactor, cfg));

    RsBlockErasure enc;
    ASSERT_TRUE(enc.init(2, 3, 20));
    const std::vector<std::vector<uint8_t>> orig = {{0xF1, 1, 2, 3},
                                                    {0xF1, 4, 5}};
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(enc.encode_block(orig, 100, &shards));
    ASSERT_EQ(shards.size(), 3u);

    endpoint.on_radio_rx(air(shards[2]), true);  // parity
    endpoint.on_radio_rx(air(shards[1]), true);  // data 1; data 0 lost
    EXPECT_EQ(app.recv(), orig[0]);
    EXPECT_EQ(app.recv(), orig[1]);
    EXPECT_EQ(endpoint.fec_air_rx_packets(), 2u);
    EXPECT_EQ(endpoint.fec_air_rx_unexpected(), 2u);
    EXPECT_EQ(endpoint.fec_recovered(), 1u);
    endpoint.close();
}

// pull_tx tags encoder output as FEC and plain datagrams as raw.
TEST(UdpEndpointTest, PullTxTagsFecShards)
{
    for (const bool use_fec : {false, true})
    {
        const uint16_t port = reserve_free_udp_port();
        ASSERT_NE(port, 0u);

        IOReactor reactor;
        UdpEndpoint endpoint;
        UpstreamConfig cfg;
        cfg.fec_type = use_fec ? FecType::RsBlockErasure : FecType::none;
        cfg.fec_k = 2;
        cfg.fec_n = 3;
        cfg.endpoint.rx = "127.0.0.1:" + std::to_string(port);
        ASSERT_TRUE(endpoint.open(reactor, cfg));

        bfc::socket client(bfc::create_udp4());
        sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dest.sin_port = htons(port);
        const std::vector<uint8_t> payload(50, 0xF1);
        const bfc::const_buffer_view view(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        for (int i = 0; i < 2; i++)
        {
            ASSERT_EQ(
                client.send(view, 0, reinterpret_cast<const sockaddr*>(&dest),
                            sizeof(dest)),
                static_cast<ssize_t>(payload.size()));
        }
        pump_reactor(reactor);

        size_t pulled = 0;
        for (;;)
        {
            bool is_fec = !use_fec;
            const bfc::sized_buffer sdu =
                endpoint.pull_tx(k_stream_payload_max, &is_fec);
            if (sdu.empty())
            {
                break;
            }
            EXPECT_EQ(is_fec, use_fec);
            pulled++;
        }
        EXPECT_EQ(pulled, use_fec ? 3u : 2u);
        EXPECT_EQ(endpoint.fec_air_tx_packets(), use_fec ? 3u : 0u);
        endpoint.close();
    }
}

// Bench iperf egress uses UDP_CLIENT_FORWARDING (connect_address = iperf -s).
// on_radio_rx must keep delivering to that fixed dest even after ICMP errors
// on the socket (e.g. iperf -s stopped between directions).
TEST(UdpEndpointTest, ClientModeForwardsRadioToConnectAddress)
{
    AppPeer iperf_server;

    IOReactor reactor;
    UdpEndpoint endpoint;
    UpstreamConfig cfg;
    cfg.fec_type = FecType::none;
    cfg.endpoint.tx = iperf_server.addr();
    ASSERT_TRUE(endpoint.open(reactor, cfg));

    const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    endpoint.on_radio_rx(air(payload), false);
    EXPECT_EQ(iperf_server.recv(), payload);
    endpoint.close();
}

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
