#include "ConsoleTestHelpers.h"
#include "endpoint/HostEndpoint.h"

#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/socket.hpp>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <poll.h>
#include <thread>

using namespace winject;
namespace
{
using Queue = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
using Registry =
    bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

class HostEndpointTest : public ::testing::TestWithParam<bool>
{
protected:
    IOReactor io;
    std::shared_ptr<Queue> in = std::make_shared<Queue>();
    std::shared_ptr<Queue> out = std::make_shared<Queue>();
    std::shared_ptr<bfc::cv_reactor<>> cv =
        std::make_shared<bfc::cv_reactor<>>();
    bfc::socket peer{bfc::create_udp4()};
    HostEndpoint endpoint;
    UpstreamConfig cfg;

    void SetUp() override
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(peer.bind(addr), 0);
        socklen_t len = sizeof(addr);
        ASSERT_EQ(
            getsockname(peer.fd(), reinterpret_cast<sockaddr*>(&addr), &len),
            0);
        if (GetParam())
        {
            in->set_capacity(4);
            out->set_capacity(4);
            ASSERT_TRUE(Registry::get_instance().add("host-test-in", in));
            ASSERT_TRUE(Registry::get_instance().add("host-test-out", out));
            ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add(
                "host-test-cv", cv));
            cfg.mode = UpstreamMode::queue;
            cfg.endpoint = {"host-test-in", "host-test-out"};
            cfg.reactor = "host-test-cv";
        }
        else
        {
            auto port = test::reserve_free_udp_port();
            ASSERT_NE(port, 0);
            cfg.endpoint.rx = "127.0.0.1:" + std::to_string(port);
            cfg.endpoint.tx =
                "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
        }
    }
    void TearDown() override
    {
        endpoint.close();
        Registry::get_instance().rem("host-test-in");
        Registry::get_instance().rem("host-test-out");
        bfc::cv_reactor_registry::get_instance().rem("host-test-cv");
    }
    void deliver(const std::vector<uint8_t>& p)
    {
        endpoint.on_radio_rx(
            bfc::shared_sized_buffer::copy_from(p.data(), p.size()), false);
    }
    std::vector<uint8_t> receive()
    {
        if (GetParam())
        {
            auto p = out->pop();
            return p ? std::vector<uint8_t>(p->u8(), p->u8() + p->size())
                     : std::vector<uint8_t>{};
        }
        pollfd fd{peer.fd(), POLLIN, 0};
        if (poll(&fd, 1, 30) != 1)
        {
            return {};
        }
        std::vector<uint8_t> p(2048);
        auto n = ::recv(peer.fd(), p.data(), p.size(), MSG_DONTWAIT);
        p.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return p;
    }
};

TEST_P(HostEndpointTest, PreservesPayloadAndAccountsLocalAcceptance)
{
    ASSERT_TRUE(endpoint.open(io, cfg));
    EXPECT_STREQ(endpoint.type_name(), GetParam() ? "QUEUE" : "UDP");
    EXPECT_EQ(endpoint.endpoint_rx(), cfg.endpoint.rx);
    EXPECT_EQ(endpoint.endpoint_tx(), cfg.endpoint.tx);
    // vstreamer telemetry framing remains an opaque upstream payload.
    const std::vector<uint8_t> p = {0, 1, 0x10, 0, 'V', 'L', 2, 0};
    deliver(p);
    EXPECT_EQ(receive(), p);
    auto c = endpoint.transport_counters();
    EXPECT_EQ(c.delivery_attempts, 1u);
    EXPECT_EQ(c.accepted_packets, 1u);
    EXPECT_EQ(c.accepted_bytes, p.size());
    EXPECT_EQ(c.send_errors, 0u);
}

TEST_P(HostEndpointTest, DoubleOpenPreservesActiveSession)
{
    ASSERT_TRUE(endpoint.open(io, cfg));
    auto bad = cfg;
    bad.endpoint = {};
    EXPECT_FALSE(endpoint.open(io, bad));
    EXPECT_TRUE(endpoint.is_open());
    deliver({1, 2});
    EXPECT_EQ(receive(), (std::vector<uint8_t>{1, 2}));
}

TEST_P(HostEndpointTest, FailedOpenCleansUpForRetry)
{
    auto bad = cfg;
    bad.fec_type = FecType::RsBlockErasure;
    bad.fec_k = 3;
    bad.fec_n = 3;
    EXPECT_FALSE(endpoint.open(io, bad));
    EXPECT_FALSE(endpoint.is_open());
    ASSERT_TRUE(endpoint.open(io, cfg));
    deliver({7});
    EXPECT_EQ(receive(), (std::vector<uint8_t>{7}));
}

TEST_P(HostEndpointTest, CloseIsIdempotentAndReopenStartsFreshSession)
{
    ASSERT_TRUE(endpoint.open(io, cfg));
    deliver({1});
    EXPECT_EQ(receive(), (std::vector<uint8_t>{1}));
    endpoint.close();
    endpoint.close();
    EXPECT_FALSE(endpoint.is_open());
    deliver({2});
    EXPECT_TRUE(receive().empty());
    EXPECT_EQ(endpoint.transport_counters().accepted_packets, 1u);
    ASSERT_TRUE(endpoint.open(io, cfg));
    EXPECT_EQ(endpoint.transport_counters().accepted_packets, 0u);
    deliver({3});
    EXPECT_EQ(receive(), (std::vector<uint8_t>{3}));
}

TEST_P(HostEndpointTest, UdpServerDoesNotRetainPreviousSessionPeer)
{
    if (GetParam())
    {
        GTEST_SKIP() << "UDP-specific learned peer";
    }
    ASSERT_TRUE(endpoint.open(io, cfg));
    endpoint.close();
    cfg.endpoint.tx.clear();
    ASSERT_TRUE(endpoint.open(io, cfg));
    deliver({1});
    EXPECT_TRUE(receive().empty());
    EXPECT_EQ(endpoint.transport_counters().no_peer_drops, 1u);
    EXPECT_EQ(endpoint.transport_counters().accepted_packets, 0u);
    EXPECT_EQ(endpoint.app_rx_packets(), 1u);
}

TEST_P(HostEndpointTest, UdpSendFailureIsAccounted)
{
    if (GetParam())
    {
        GTEST_SKIP() << "UDP-specific send error";
    }
    cfg.endpoint.tx =
        "255.255.255.255:9";  // socket has no SO_BROADCAST permission
    ASSERT_TRUE(endpoint.open(io, cfg));
    deliver({1});
    EXPECT_EQ(endpoint.transport_counters().send_errors, 1u);
    EXPECT_EQ(endpoint.transport_counters().accepted_packets, 0u);
}

TEST_P(HostEndpointTest, QueueEvictionAcceptsNewPacketAndAccountsOldLoss)
{
    if (!GetParam())
    {
        GTEST_SKIP() << "queue-specific eviction";
    }
    out->set_capacity(1);
    ASSERT_TRUE(endpoint.open(io, cfg));
    deliver({1});
    deliver({2});
    EXPECT_EQ(receive(), (std::vector<uint8_t>{2}));
    EXPECT_EQ(endpoint.transport_counters().accepted_packets, 2u);
    EXPECT_EQ(endpoint.transport_counters().queue_evictions, 1u);
}

TEST_P(HostEndpointTest, QueueInputAliasesCannotReplaceAnActiveConsumer)
{
    if (!GetParam())
    {
        GTEST_SKIP() << "queue-specific consumer identity";
    }
    ASSERT_TRUE(endpoint.open(io, cfg));
    ASSERT_TRUE(Registry::get_instance().add("host-test-alias", in));
    auto other_cfg = cfg;
    other_cfg.endpoint.rx = "host-test-alias";
    HostEndpoint other;
    EXPECT_FALSE(other.open(io, other_cfg));
    EXPECT_TRUE(endpoint.is_open());
    endpoint.close();
    EXPECT_TRUE(other.open(io, other_cfg));
    other.close();
    Registry::get_instance().rem("host-test-alias");
}

INSTANTIATE_TEST_SUITE_P(Transports, HostEndpointTest, ::testing::Bool(),
                         [](const auto& info)
                         {
                             return info.param ? "Queue" : "Udp";
                         });

// Simulates a reactor stalled after copying a callback and before invoking it.
class StalledQueue : public Queue
{
public:
    std::mutex mutex;
    callback_t copied;
    std::promise<void> ready;
    std::shared_future<void> release;
    void set_callback(callback_t cb) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        copied = std::move(cb);
    }
    void notify_callback() override
    {
        callback_t cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cb = copied;
        }
        ready.set_value();
        release.wait();
        if (cb)
        {
            cb();
        }
    }
};

TEST(HostEndpointLifetimeTest,
     CopiedQueueCallbackAndContextSurviveCloseWithoutAck)
{
    IOReactor io;
    auto in = std::make_shared<StalledQueue>();
    auto out = std::make_shared<Queue>();
    auto cv = std::make_shared<bfc::cv_reactor<>>();
    in->set_capacity(4);
    out->set_capacity(4);
    std::promise<void> release;
    in->release = release.get_future().share();
    auto copied = in->ready.get_future();
    ASSERT_TRUE(Registry::get_instance().add("stall-in", in));
    ASSERT_TRUE(Registry::get_instance().add("stall-out", out));
    ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add("stall-cv", cv));
    auto endpoint = std::make_unique<HostEndpoint>();
    UpstreamConfig cfg;
    cfg.mode = UpstreamMode::queue;
    cfg.endpoint = {"stall-in", "stall-out"};
    cfg.reactor = "stall-cv";
    ASSERT_TRUE(endpoint->open(io, cfg));
    uint8_t p = 1;
    in->push(bfc::shared_sized_buffer::copy_from(&p, 1));
    std::thread worker(
        [&]()
        {
            cv->run();
        });
    // Always release/join on failure, so a test failure cannot hang the
    // process.
    const auto status = copied.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready)
    {
        release.set_value();
        cv->stop();
        worker.join();
        FAIL() << "reactor did not reach copied callback";
    }
    std::weak_ptr<Queue> weak = in;
    const auto before = std::chrono::steady_clock::now();
    endpoint.reset();  // no timed reactor acknowledgement is needed
    EXPECT_LT(std::chrono::steady_clock::now() - before,
              std::chrono::seconds(1));
    Registry::get_instance().rem("stall-in");
    in.reset();
    EXPECT_FALSE(weak.expired());  // deferred context lease protects BFC's scan
    release.set_value();
    cv->stop();
    worker.join();
    // stop can bypass deferred callbacks; their captured queue remains safe
    // until the host destroys or resumes the reactor.
    bfc::cv_reactor_registry::get_instance().rem("stall-cv");
    cv.reset();
    EXPECT_TRUE(weak.expired());
    Registry::get_instance().rem("stall-out");
}
}  // namespace
