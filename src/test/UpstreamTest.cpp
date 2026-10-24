#include "ConsoleTestHelpers.h"
#include "endpoint/QueueUpstream.h"
#include "endpoint/UDPUpstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/RxDemux.h"
#include "radio/TxMux.h"
#include "utils/NetUtil.h"

#include <bfc/socket.hpp>
#include <future>
#include <gtest/gtest.h>
#include <poll.h>
#include <thread>

using namespace winject;
namespace
{
using Packet = bfc::shared_sized_buffer;
Packet packet(std::initializer_list<uint8_t> bytes)
{
    return Packet::copy_from(bytes.begin(), bytes.size());
}
Packet lc_packet(const Packet& payload, bool fec = false, uint16_t seq = 0)
{
    bfc::sized_buffer bytes(LCHeader::k_len + payload.size());
    auto out = reinterpret_cast<uint8_t*>(bytes.data());
    LCHeader::write(out, 7, seq, fec);
    memcpy(out + LCHeader::k_len, payload.data(), payload.size());
    return Packet::copy_from(bytes.data(), bytes.size());
}
std::vector<uint8_t> bytes(const Packet& p)
{
    return {p.u8(), p.u8() + p.size()};
}
class UpstreamTest : public ::testing::Test
{
protected:
    std::shared_ptr<Upstream::Reactor> cv =
        std::make_shared<Upstream::Reactor>();
    std::shared_ptr<ReactiveLossyQueue> in = std::make_shared<ReactiveLossyQueue>();
    std::shared_ptr<ReactiveLossyQueue> out = std::make_shared<ReactiveLossyQueue>();
    std::shared_ptr<BoundedLossyQueue> tx = std::make_shared<BoundedLossyQueue>();
    UpstreamConfig cfg;
    std::shared_ptr<IOReactor> io = std::make_shared<IOReactor>();
    std::shared_ptr<Upstream> up;
    void SetUp() override
    {
        in->set_capacity(64);
        out->set_capacity(64);
        cfg.mode = UpstreamMode::queue;
        cfg.endpoint = {"in", "out"};
        cfg.bus_tx = cfg.bus_rx = 7;
    }
    void TearDown() override
    {
        up.reset();
    }
    void create_queue()
    {
        up = std::make_shared<QueueUpstream>(cv, cfg, tx, in, out);
        up->bind_callbacks();
    }
    void create_udp(const std::string& rx, const std::string& dest = {})
    {
        cfg.mode = UpstreamMode::udp_static;
        cfg.endpoint = {rx, dest};
        up = std::make_shared<UDPUpstream>(cv, io, cfg, tx);
        up->bind_callbacks();
    }
    void pump_cv(int ms = 2)
    {
        cv->get_timer().wait_ms(ms,
                                [reactor = cv]
                                {
                                    reactor->stop();
                                });
        cv->run();
    }
    void pump_io(int ms = 2)
    {
        io->get_timer().wait_ms(ms,
                                [this]
                                {
                                    io->stop();
                                });
        io->run();
    }
    void rx(Packet p, bool fec = false, uint16_t seq = 0)
    {
        up->rx_queue()->push(lc_packet(p, fec, seq));
    }
    sockaddr_in bind_peer(bfc::socket& peer)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(peer.bind(addr), 0);
        socklen_t size = sizeof(addr);
        EXPECT_EQ(
            getsockname(peer.fd(), reinterpret_cast<sockaddr*>(&addr), &size),
            0);
        return addr;
    }
    std::vector<uint8_t> receive(bfc::socket& peer)
    {
        uint8_t buffer[2048];
        auto n = ::recv(peer.fd(), buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n <= 0)
        {
            return {};
        }
        return {buffer, buffer + n};
    }
    void send_to(bfc::socket& peer, uint16_t port, const Packet& p)
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dest.sin_port = htons(port);
        ASSERT_EQ(::sendto(peer.fd(), p.data(), p.size(), 0,
                           reinterpret_cast<sockaddr*>(&dest), sizeof(dest)),
                  ssize_t(p.size()));
    }
};

TEST_F(UpstreamTest, SharedOwnershipAndQueueOnlyRadioInterface)
{
    create_queue();
    EXPECT_EQ(up->shared_from_this(), up);
    auto queue = up->rx_queue();
    rx(packet({1, 2, 3}));
    EXPECT_FALSE(out->has_data());  // producer does no decoding or delivery
    EXPECT_EQ(queue->size(), 1u);
    pump_cv();
    auto received = out->pop();
    ASSERT_TRUE(received);
    EXPECT_EQ(bytes(*received), (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(up->rx_counters().accepted_packets, 1u);
}
TEST_F(UpstreamTest, AppInputImmediatelyPushesSuppliedTxQueue)
{
    create_queue();
    in->push(packet({4, 5}));  // no explicit wake
    EXPECT_FALSE(tx->has_data());
    pump_cv();
    auto sent = tx->pop();
    ASSERT_TRUE(sent);
    EXPECT_FALSE(LCHeader::read_is_fec(sent->u8()));
    EXPECT_EQ(bytes(*sent), bytes(lc_packet(packet({4, 5}), false, 0)));
    EXPECT_EQ(up->app_tx_packets(), 1u);
    EXPECT_EQ(up->tx_counters().packets, 1u);
    EXPECT_EQ(up->tx_counters().bytes, 2u);
    EXPECT_EQ(up->rx_counters().accepted_packets, 0u);
}
TEST_F(UpstreamTest, ConstructorDiscoversPrequeuedInput)
{
    in->push(packet({8}));
    create_queue();
    pump_cv();
    ASSERT_TRUE(tx->has_data());
    EXPECT_EQ(bytes(*tx->pop()), bytes(lc_packet(packet({8}), false, 0)));
}
TEST_F(UpstreamTest, RadioSequenceFilteringRunsInUpstream)
{
    create_queue();
    rx(packet({1}), false, 1);
    rx(packet({2}), false, 1);  // duplicate
    rx(packet({3}), false, 4);  // two missing
    EXPECT_EQ(up->stats().air_rx_gap_loss, 0u);
    pump_cv();
    EXPECT_EQ(out->size(), 2u);
    EXPECT_EQ(up->stats().air_rx_gap_loss, 2u);
    EXPECT_EQ(up->collect_air_seq_loss_delta(), 2u);
    EXPECT_EQ(up->collect_air_seq_loss_delta(), 0u);
}
TEST_F(UpstreamTest, ShortLcPacketIsRejected)
{
    create_queue();
    up->rx_queue()->push(packet({1, 2}));
    pump_cv();
    EXPECT_FALSE(out->has_data());
}
TEST_F(UpstreamTest, FecEncodePushesShardsWithoutSchedulerPull)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    cfg.fec_timeout_ms = 0;
    create_queue();
    in->push(packet({1}));
    in->push(packet({2}));
    pump_cv();
    EXPECT_EQ(tx->size(), 3u);
    while (auto p = tx->pop())
    {
        EXPECT_TRUE(LCHeader::read_is_fec(p->u8()));
    }
    EXPECT_EQ(up->fec_air_tx_packets(), 3u);
}
TEST_F(UpstreamTest, TxFecTimeoutUsesSuppliedCvReactor)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 3;
    cfg.fec_n = 4;
    cfg.fec_timeout_ms = 5;
    create_queue();
    in->push(packet({9}));
    pump_cv(1);
    EXPECT_FALSE(tx->has_data());
    pump_cv(15);
    EXPECT_EQ(tx->size(), 2u);
}
TEST_F(UpstreamTest, RxFecRecoversMissingShardOnCvReactor)
{
    create_queue();
    RsBlockErasure encoder;
    ASSERT_TRUE(encoder.init(2, 3, 20));
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(encoder.encode_block({{1, 2}, {3, 4}}, 100, &shards));
    rx(Packet::copy_from(shards[2].data(), shards[2].size()), true, 0);
    rx(Packet::copy_from(shards[1].data(), shards[1].size()), true, 1);
    EXPECT_FALSE(out->has_data());
    pump_cv();
    ASSERT_EQ(out->size(), 2u);
    EXPECT_EQ(bytes(*out->pop()), (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(bytes(*out->pop()), (std::vector<uint8_t>{3, 4}));
    EXPECT_GE(up->fec_recovered(), 1u);
}
TEST_F(UpstreamTest, RxFecTimerSalvagesWithoutNewRadioPackets)
{
    create_queue();
    RsBlockErasure encoder;
    ASSERT_TRUE(encoder.init(3, 4, 20));
    std::vector<std::vector<uint8_t>> shards;
    ASSERT_TRUE(encoder.encode_block({{1}, {2}, {3}}, 100, &shards));
    rx(Packet::copy_from(shards[1].data(), shards[1].size()), true);
    pump_cv(1);
    EXPECT_FALSE(out->has_data());
    pump_cv(300);
    ASSERT_EQ(out->size(), 1u);
    EXPECT_EQ(bytes(*out->pop()), (std::vector<uint8_t>{2}));
}
TEST_F(UpstreamTest, DestructorDropsPartialFecAndPendingRx)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 3;
    cfg.fec_n = 4;
    cfg.fec_timeout_ms = 10;
    create_queue();
    in->push(packet({1}));
    pump_cv(1);
    auto inbox = up->rx_queue();
    inbox->push(lc_packet(packet({7})));
    std::weak_ptr<Upstream> weak = up;
    up.reset();
    EXPECT_TRUE(weak.expired());
    pump_cv(20);
    EXPECT_FALSE(tx->has_data());
    EXPECT_FALSE(out->has_data());
    // Producer can safely retain a shared queue after endpoint destruction.
    inbox->push(lc_packet(packet({8})));
    pump_cv();
    EXPECT_FALSE(out->has_data());
}
TEST_F(UpstreamTest, ExtractedFecTimerCannotUseDestroyedEndpoint)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 3;
    cfg.fec_n = 4;
    cfg.fec_timeout_ms = 5;
    create_queue();
    cv->get_timer().wait_ms(2,
                            [this]
                            {
                                up.reset();
                            });
    in->push(packet({1}));
    pump_cv(1);
    cv->get_timer().schedule(Upstream::Reactor::timer_t::current_time_us() +
                             1000000);
    EXPECT_FALSE(up);
    EXPECT_FALSE(tx->has_data());
}
TEST_F(UpstreamTest, ReconstructionReleasesInputClaim)
{
    create_queue();
    EXPECT_THROW(std::make_shared<QueueUpstream>(cv, cfg, tx, in, out),
                 std::invalid_argument);
    up.reset();
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    create_queue();
    in->push(packet({1}));
    in->push(packet({2}));
    pump_cv();
    EXPECT_EQ(tx->size(), 3u);
}
TEST_F(UpstreamTest, InvalidConstructionDoesNotClaimInputOrLeaveCallbacks)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = cfg.fec_n = 3;
    EXPECT_THROW(create_queue(), std::invalid_argument);
    cfg.fec_type = FecType::none;
    create_queue();
    in->push(packet({1}));
    pump_cv();
    EXPECT_EQ(tx->size(), 1u);
}
TEST_F(UpstreamTest, RejectsMissingAndAliasedQueues)
{
    EXPECT_THROW(std::make_shared<QueueUpstream>(cv, cfg, tx, in, in),
                 std::invalid_argument);
    EXPECT_THROW(std::make_shared<QueueUpstream>(cv, cfg, tx, in, nullptr),
                 std::invalid_argument);
    EXPECT_THROW(std::make_shared<QueueUpstream>(nullptr, cfg, tx, in, out),
                 std::invalid_argument);
}
TEST_F(UpstreamTest, OutputEvictionCountsAcceptedNewPacket)
{
    out->set_capacity(1);
    create_queue();
    rx(packet({1}), false, 0);
    rx(packet({2}), false, 1);
    pump_cv();
    EXPECT_EQ(bytes(*out->pop()), (std::vector<uint8_t>{2}));
    auto counters = up->rx_counters();
    EXPECT_EQ(counters.accepted_packets, 2u);
    EXPECT_EQ(counters.queue_evictions, 1u);
}
TEST_F(UpstreamTest, TxOverflowKeepsNewestAndCountsDrops)
{
    tx = std::make_shared<BoundedLossyQueue>(1);
    create_queue();
    in->push(packet({1}));
    in->push(packet({2, 3}));
    pump_cv();
    EXPECT_EQ(up->tx_queue_drops(), 1u);
    EXPECT_EQ(up->tx_counters().queue_evictions, 1u);
    uint64_t count = 0, size = 0;
    up->tx_pending_stats(&count, &size);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(size, 5u);
    EXPECT_EQ(bytes(*tx->pop()),
              bytes(lc_packet(packet({2, 3}), false, 1)));
}
TEST_F(UpstreamTest, RxOverflowIsLossyAndKeepsNewest)
{
    cfg.rx_queue_capacity = 1;
    create_queue();
    EXPECT_EQ(up->rx_queue()->capacity(), 1u);
    EXPECT_EQ(in->capacity(), 64u);
    rx(packet({1}), false, 0);
    rx(packet({2}), false, 1);
    pump_cv();
    EXPECT_EQ(out->size(), 1u);
    EXPECT_EQ(bytes(*out->pop()), (std::vector<uint8_t>{2}));
}
TEST_F(UpstreamTest, QueueBurstIsRescheduledAfterBoundedTurn)
{
    create_queue();
    for (int i = 0; i < 60; ++i)
    {
        in->push(packet({uint8_t(i)}));
    }
    pump_cv(5);
    EXPECT_FALSE(in->has_data());
    EXPECT_EQ(tx->size(), 60u);
}
TEST_F(UpstreamTest, UdpInputOnlyEnqueuesUntilCvReactorRuns)
{
    uint16_t port = test::reserve_free_udp_port();
    ASSERT_NE(port, 0);
    create_udp("127.0.0.1:" + std::to_string(port));
    bfc::socket peer(bfc::create_udp4());
    send_to(peer, port, packet({1, 2}));
    pump_io();
    EXPECT_FALSE(tx->has_data());
    EXPECT_EQ(up->app_tx_packets(), 0u);
    pump_cv();
    ASSERT_EQ(tx->size(), 1u);
    EXPECT_EQ(bytes(*tx->pop()),
              bytes(lc_packet(packet({1, 2}), false, 0)));
}
TEST_F(UpstreamTest, UdpDeliveryRunsOnCvReactorAndPreservesPayload)
{
    bfc::socket peer(bfc::create_udp4());
    auto address = bind_peer(peer);
    create_udp({}, "127.0.0.1:" + std::to_string(ntohs(address.sin_port)));
    rx(packet({0, 'V', 'L', 2, 0}));
    EXPECT_TRUE(receive(peer).empty());
    pump_cv();
    EXPECT_EQ(receive(peer), (std::vector<uint8_t>{0, 'V', 'L', 2, 0}));
    EXPECT_EQ(up->rx_counters().accepted_packets, 1u);
}
TEST_F(UpstreamTest, UdpServerLearnsPeerAndReplacementStartsFresh)
{
    uint16_t port = test::reserve_free_udp_port();
    create_udp("127.0.0.1:" + std::to_string(port));
    rx(packet({1}));
    pump_cv();
    EXPECT_EQ(up->rx_counters().no_peer_drops, 1u);
    bfc::socket peer(bfc::create_udp4());
    bind_peer(peer);
    send_to(peer, port, packet({2}));
    pump_io();
    pump_cv();
    rx(packet({3}), false, 1);
    pump_cv();
    EXPECT_EQ(receive(peer), (std::vector<uint8_t>{3}));
    up.reset();
    create_udp("127.0.0.1:" + std::to_string(port));
    rx(packet({4}));
    pump_cv();
    EXPECT_TRUE(receive(peer).empty());
    EXPECT_EQ(up->rx_counters().no_peer_drops, 1u);
}
TEST_F(UpstreamTest, UdpSendFailureIsAccounted)
{
    create_udp({}, "255.255.255.255:9");
    rx(packet({1}));
    pump_cv();
    EXPECT_EQ(up->rx_counters().send_errors, 1u);
}
TEST_F(UpstreamTest, OversizedUdpDatagramIsRejected)
{
    uint16_t port = test::reserve_free_udp_port();
    create_udp("127.0.0.1:" + std::to_string(port));
    bfc::socket peer(bfc::create_udp4());
    std::vector<uint8_t> huge(4096, 1);
    send_to(peer, port, Packet::copy_from(huge.data(), huge.size()));
    pump_io();
    pump_cv();
    EXPECT_FALSE(tx->has_data());
    EXPECT_EQ(up->app_rx_oversize_pkt(), 1u);
    EXPECT_EQ(up->tx_counters().oversize_drops, 1u);
    EXPECT_EQ(up->app_tx_bytes(), huge.size());
}
TEST_F(UpstreamTest, UdpRetainsSharedIoReactorUntilDestruction)
{
    create_udp({}, "127.0.0.1:9");
    std::weak_ptr<IOReactor> weak = io;
    io.reset();
    EXPECT_FALSE(weak.expired());
    up.reset();
    EXPECT_TRUE(weak.expired());
}
TEST_F(UpstreamTest, BindingRequiresSharedOwnership)
{
    QueueUpstream local(cv, cfg, tx, in, out);
    EXPECT_THROW(local.bind_callbacks(), std::logic_error);
}
TEST_F(UpstreamTest, UdpExtractedSocketCallbackIsSafeAfterDestruction)
{
    uint16_t port = test::reserve_free_udp_port();
    create_udp("127.0.0.1:" + std::to_string(port));
    bfc::socket peer(bfc::create_udp4());
    send_to(peer, port, packet({1}));
    up.reset();
    pump_io();
    pump_cv();
    EXPECT_FALSE(tx->has_data());
}
TEST(BoundedLossyQueueTest, PeekParticipatesInCapacityAndByteAccounting)
{
    BoundedLossyQueue queue(2);
    queue.push(packet({1}));
    EXPECT_EQ(queue.head_size(), 1u);
    queue.push(packet({2, 3}));
    EXPECT_EQ(queue.push(packet({4, 5, 6})), 2u);
    uint64_t count = 0, size = 0;
    queue.pending_stats(&count, &size);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(size, 5u);
    EXPECT_FALSE(queue.pop(1));
    EXPECT_EQ(bytes(*queue.pop()), (std::vector<uint8_t>{2, 3}));
    EXPECT_EQ(bytes(*queue.pop()), (std::vector<uint8_t>{4, 5, 6}));
    EXPECT_FALSE(queue.has_data());
}
TEST(BoundedLossyQueueTest, PushNotifiesAfterUnlockingQueue)
{
    size_t notifications = 0;
    BoundedLossyQueue* observed = nullptr;
    BoundedLossyQueue queue(1,
                          [&]
                          {
                              ++notifications;
                              EXPECT_EQ(observed->size(), 1u);
                              uint64_t count = 0, size = 0;
                              observed->pending_stats(&count, &size);
                              EXPECT_EQ(count, 1u);
                              EXPECT_EQ(size, 2u);
                              EXPECT_EQ(bytes(*observed->pop()),
                                        (std::vector<uint8_t>{1, 2}));
                          });
    observed = &queue;
    EXPECT_EQ(queue.push(packet({1, 2})), 0u);
    EXPECT_EQ(notifications, 1u);
    EXPECT_FALSE(queue.has_data());
}
TEST(BoundedLossyQueueTest, ZeroCapacityDiscardsWithoutNotifying)
{
    size_t notifications = 0;
    BoundedLossyQueue queue(0,
                          [&]
                          {
                              ++notifications;
                          });
    EXPECT_EQ(queue.push(packet({1})), 0u);
    EXPECT_EQ(notifications, 0u);
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(queue.head_size(), 0u);
    EXPECT_FALSE(queue.pop());
    uint64_t count = 1, size = 1;
    queue.pending_stats(&count, &size);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(size, 0u);
}
TEST(BoundedLossyQueueTest, RetainedQueueCannotWakeDestroyedMux)
{
    RadioUpstreamTable table;
    auto mux = std::make_unique<TxMux>(table);
    auto queue = mux->make_tx_queue();
    mux.reset();
    queue->push(packet({1}));
    EXPECT_EQ(queue->size(), 1u);
}
}  // namespace
