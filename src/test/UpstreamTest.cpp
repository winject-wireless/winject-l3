#include "ConsoleTestHelpers.h"
#include "endpoint/QueueUpstream.h"
#include "endpoint/UDPUpstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RxDemux.h"
#include "radio/TxMux.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <bfc/socket.hpp>
#include <chrono>
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
    std::shared_ptr<ReactiveLossyQueue> app_tx_queue =
        std::make_shared<ReactiveLossyQueue>();
    std::shared_ptr<ReactiveLossyQueue> app_rx_queue =
        std::make_shared<ReactiveLossyQueue>();
    std::shared_ptr<BoundedLossyQueue> tx = std::make_shared<BoundedLossyQueue>();
    UpstreamConfig cfg;
    std::shared_ptr<IOReactor> io = std::make_shared<IOReactor>();
    std::shared_ptr<Upstream> up;
    void SetUp() override
    {
        app_tx_queue->set_capacity(64);
        app_rx_queue->set_capacity(64);
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
        up = std::make_shared<QueueUpstream>(cv, cfg, tx, app_tx_queue,
                                             app_rx_queue);
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
    EXPECT_FALSE(app_rx_queue->has_data());  // producer does no decoding or delivery
    EXPECT_EQ(queue->size(), 1u);
    pump_cv();
    auto received = app_rx_queue->pop();
    ASSERT_TRUE(received);
    EXPECT_EQ(bytes(*received), (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(up->rx_counters().accepted_packets, 1u);
}
TEST_F(UpstreamTest, AppInputImmediatelyPushesSuppliedTxQueue)
{
    create_queue();
    app_tx_queue->push(packet({4, 5}));  // no explicit wake
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
    app_tx_queue->push(packet({8}));
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
    EXPECT_EQ(app_rx_queue->size(), 2u);
    EXPECT_EQ(up->stats().air_rx_gap_loss, 2u);
    EXPECT_EQ(up->collect_air_seq_loss_delta(), 2u);
    EXPECT_EQ(up->collect_air_seq_loss_delta(), 0u);
}
// A peer restart restarts its air seq. Landing well behind the last seq
// resyncs without counting loss, and gap accounting resumes from there.
TEST_F(UpstreamTest, AirRestartLandingBehindResyncsWithoutLoss)
{
    create_queue();
    rx(packet({1}), false, 1000);
    rx(packet({2}), false, 1001);
    rx(packet({3}), false, 0);  // restarted sender
    rx(packet({4}), false, 1);
    rx(packet({5}), false, 3);  // seq 2 lost
    pump_cv();
    EXPECT_EQ(app_rx_queue->size(), 5u);
    EXPECT_EQ(up->stats().air_rx_resyncs, 1u);
    EXPECT_EQ(up->stats().air_rx_gap_loss, 1u);
}
// Late packets inside the reorder window are delivered but keep the reference.
TEST_F(UpstreamTest, AirLateWithinReorderWindowKeepsReference)
{
    create_queue();
    rx(packet({1}), false, 100);
    rx(packet({2}), false, 101);
    rx(packet({3}), false, uint16_t(101 - k_air_rx_reorder_window));
    rx(packet({4}), false, 102);
    pump_cv();
    EXPECT_EQ(app_rx_queue->size(), 4u);
    EXPECT_EQ(up->stats().air_rx_resyncs, 0u);
    EXPECT_EQ(up->stats().air_rx_gap_loss, 0u);
}
TEST_F(UpstreamTest, AirSeqWrapIsNotARestart)
{
    create_queue();
    rx(packet({1}), false, 32766);
    rx(packet({2}), false, 32767);
    rx(packet({3}), false, 0);
    rx(packet({4}), false, 1);
    pump_cv();
    EXPECT_EQ(app_rx_queue->size(), 4u);
    EXPECT_EQ(up->stats().air_rx_resyncs, 0u);
    EXPECT_EQ(up->stats().air_rx_gap_loss, 0u);
}
TEST_F(UpstreamTest, AirRestartBehindAcrossWrapResyncs)
{
    create_queue();
    rx(packet({1}), false, 10);
    rx(packet({2}), false, 32000);  // 778 behind across the wrap
    rx(packet({3}), false, 32001);
    pump_cv();
    EXPECT_EQ(app_rx_queue->size(), 3u);
    EXPECT_EQ(up->stats().air_rx_resyncs, 1u);
    EXPECT_EQ(up->stats().air_rx_gap_loss, 0u);
}
// A restart that lands ahead is indistinguishable from an outage: loss.
TEST_F(UpstreamTest, AirRestartLandingAheadCountsAsLoss)
{
    create_queue();
    rx(packet({1}), false, 10);
    rx(packet({2}), false, 5000);
    pump_cv();
    EXPECT_EQ(app_rx_queue->size(), 2u);
    EXPECT_EQ(up->stats().air_rx_resyncs, 0u);
    EXPECT_EQ(up->stats().air_rx_gap_loss, 4989u);
}
TEST_F(UpstreamTest, ShortLcPacketIsRejected)
{
    create_queue();
    up->rx_queue()->push(packet({1, 2}));
    pump_cv();
    EXPECT_FALSE(app_rx_queue->has_data());
}
TEST_F(UpstreamTest, FecEncodePushesShardsWithoutSchedulerPull)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    cfg.fec_timeout_ms = 0;
    create_queue();
    app_tx_queue->push(packet({1}));
    app_tx_queue->push(packet({2}));
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
    app_tx_queue->push(packet({9}));
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
    EXPECT_FALSE(app_rx_queue->has_data());
    pump_cv();
    ASSERT_EQ(app_rx_queue->size(), 2u);
    EXPECT_EQ(bytes(*app_rx_queue->pop()), (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(bytes(*app_rx_queue->pop()), (std::vector<uint8_t>{3, 4}));
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
    EXPECT_FALSE(app_rx_queue->has_data());
    pump_cv(300);
    ASSERT_EQ(app_rx_queue->size(), 1u);
    EXPECT_EQ(bytes(*app_rx_queue->pop()), (std::vector<uint8_t>{2}));
}
TEST_F(UpstreamTest, DestructorDropsPartialFecAndPendingRx)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 3;
    cfg.fec_n = 4;
    cfg.fec_timeout_ms = 10;
    create_queue();
    app_tx_queue->push(packet({1}));
    pump_cv(1);
    auto inbox = up->rx_queue();
    inbox->push(lc_packet(packet({7})));
    std::weak_ptr<Upstream> weak = up;
    up.reset();
    EXPECT_TRUE(weak.expired());
    pump_cv(20);
    EXPECT_FALSE(tx->has_data());
    EXPECT_FALSE(app_rx_queue->has_data());
    // Producer can safely retain a shared queue after endpoint destruction.
    inbox->push(lc_packet(packet({8})));
    pump_cv();
    EXPECT_FALSE(app_rx_queue->has_data());
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
    app_tx_queue->push(packet({1}));
    pump_cv(1);
    cv->get_timer().schedule(Upstream::Reactor::timer_t::current_time_us() +
                             1000000);
    EXPECT_FALSE(up);
    EXPECT_FALSE(tx->has_data());
}
TEST_F(UpstreamTest, TeardownReleasesInputClaim)
{
    create_queue();
    std::thread cv_worker(
        [this]()
        {
            cv->run();
        });
    up->teardown();
    up.reset();
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    create_queue();
    app_tx_queue->push(packet({1}));
    app_tx_queue->push(packet({2}));
    cv->get_timer().wait_ms(
        50,
        [this]()
        {
            cv->stop();
        });
    cv_worker.join();
    EXPECT_EQ(tx->size(), 3u);
}
// Runs the reactor on a worker thread for one test.
struct CvWorker
{
    std::shared_ptr<Upstream::Reactor> cv;
    std::thread thread;
    explicit CvWorker(std::shared_ptr<Upstream::Reactor> reactor)
        : cv(std::move(reactor)),
          thread(
              [cv = cv]
              {
                  cv->run();
              })
    {
    }
    ~CvWorker()
    {
        cv->stop();
        thread.join();
    }
};
// Blocks the first radio TX push until released.
struct PushGate
{
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> armed{true};
    void on_push()
    {
        if (armed.exchange(false))
        {
            entered.set_value();
            released.wait();
        }
    }
};
// Longer than any fixed fence timeout teardown could have used.
constexpr auto k_held = std::chrono::milliseconds(200);

// teardown() waits out this upstream's own data-path call however long it
// runs, and none of its handlers runs afterwards.
TEST_F(UpstreamTest, TeardownWaitsForInFlightDataPath)
{
    PushGate gate;
    tx = std::make_shared<BoundedLossyQueue>(1024,
                                             [g = &gate]
                                             {
                                                 g->on_push();
                                             });
    create_queue();
    CvWorker worker(cv);
    app_tx_queue->push(packet({1}));
    ASSERT_EQ(gate.entered.get_future().wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    auto torn_down = std::async(std::launch::async,
                                [this]
                                {
                                    up->teardown();
                                });
    EXPECT_EQ(torn_down.wait_for(k_held), std::future_status::timeout);
    gate.release.set_value();
    ASSERT_EQ(torn_down.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    torn_down.get();
    EXPECT_EQ(tx->size(), 1u);
    app_tx_queue->push(packet({2}));
    cv->wake_up();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(tx->size(), 1u);
    EXPECT_TRUE(app_tx_queue->has_data());  // left for a replacement
}
// teardown() waits out a firing FEC timer and none fires afterwards.
TEST_F(UpstreamTest, TeardownWaitsForFiringFecTimer)
{
    PushGate gate;
    tx = std::make_shared<BoundedLossyQueue>(1024,
                                             [g = &gate]
                                             {
                                                 g->on_push();
                                             });
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 3;
    cfg.fec_n = 4;
    cfg.fec_timeout_ms = 5;
    create_queue();
    CvWorker worker(cv);
    app_tx_queue->push(packet({1}));  // partial block; the timer flushes it
    ASSERT_EQ(gate.entered.get_future().wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    auto torn_down = std::async(std::launch::async,
                                [this]
                                {
                                    up->teardown();
                                });
    EXPECT_EQ(torn_down.wait_for(k_held), std::future_status::timeout);
    gate.release.set_value();
    ASSERT_EQ(torn_down.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    torn_down.get();
    EXPECT_EQ(tx->size(), 2u);  // the flush that was firing completes
    app_tx_queue->push(packet({2}));
    cv->wake_up();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(tx->size(), 2u);
}
// A reactor stuck in another handler does not hold up teardown, and nothing
// of the old upstream runs once it resumes.
TEST_F(UpstreamTest, TeardownDoesNotWaitForBusyReactor)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 3;
    cfg.fec_n = 4;
    cfg.fec_timeout_ms = 5;
    create_queue();
    auto blocker = std::make_shared<ReactiveLossyQueue>();
    blocker->set_capacity(1);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    cv->add_read_rdy(*blocker,
                     [&]
                     {
                         blocker->pop();
                         entered.set_value();
                         released.wait();
                     });
    CvWorker worker(cv);
    auto wait_for = [](auto pred)
    {
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!pred() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pred();
    };

    app_tx_queue->push(packet({1}));  // partial block arms the FEC timer
    ASSERT_TRUE(wait_for([&] { return up->app_tx_packets() == 1u; }));
    EXPECT_FALSE(tx->has_data());

    blocker->push(packet({0}));
    cv->wake_up();
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    auto torn_down = std::async(std::launch::async,
                                [this]
                                {
                                    up->teardown();
                                });
    EXPECT_EQ(torn_down.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    torn_down.get();
    up.reset();
    release.set_value();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(tx->has_data());  // FEC timeout never flushed the block
    app_tx_queue->push(packet({2}));
    cv->wake_up();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(app_tx_queue->has_data());  // no handler consumes input
    cv->remove_read_rdy(*blocker);
}

// A reactor that is not running never blocks teardown; when it runs later,
// nothing of the old upstream runs.
TEST_F(UpstreamTest, TeardownWithStoppedReactorReturnsAtOnce)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 3;
    cfg.fec_n = 4;
    cfg.fec_timeout_ms = 5;
    create_queue();
    app_tx_queue->push(packet({1}));
    pump_cv(1);  // arms the FEC timer, then the reactor stops
    const auto begin = std::chrono::steady_clock::now();
    up->teardown();
    EXPECT_LT(std::chrono::steady_clock::now() - begin,
              std::chrono::milliseconds(50));
    up.reset();
    app_tx_queue->push(packet({2}));
    pump_cv(20);  // the FEC timer was cancelled
    EXPECT_FALSE(tx->has_data());
    EXPECT_TRUE(app_tx_queue->has_data());
}

// A replacement continues the air and FEC SDU sequences, so the peer's
// sequencers see one stream across a hot-swap.
TEST_F(UpstreamTest, ReplacementContinuesTxSequences)
{
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    cfg.fec_timeout_ms = 0;
    auto sdu_base = [](const Packet& p)
    {
        const uint8_t* shard = p.u8() + LCHeader::k_len;
        return uint16_t((shard[0] << 8) | shard[1]);
    };
    create_queue();
    app_tx_queue->push(packet({1}));
    app_tx_queue->push(packet({2}));
    pump_cv();
    ASSERT_EQ(tx->size(), 3u);
    const uint16_t first_base = sdu_base(*tx->pop());
    while (tx->pop())
    {
    }
    EXPECT_THROW(up->tx_sequences(), std::logic_error);
    up->teardown();
    const auto sequences = up->tx_sequences();
    EXPECT_EQ(sequences.air, 3u);
    up.reset();

    up = std::make_shared<QueueUpstream>(cv, cfg, tx, app_tx_queue,
                                         app_rx_queue);
    up->resume_tx_sequences(sequences);
    up->bind_callbacks();
    EXPECT_THROW(up->resume_tx_sequences(sequences), std::logic_error);
    app_tx_queue->push(packet({3}));
    app_tx_queue->push(packet({4}));
    pump_cv();
    ASSERT_EQ(tx->size(), 3u);
    const auto next = *tx->pop();
    EXPECT_EQ(LCHeader::read_seq(next.u8()), 3u);
    EXPECT_EQ(sdu_base(next), uint16_t(first_base + 2));
}

TEST_F(UpstreamTest, ReconstructionReleasesInputClaim)
{
    create_queue();
    EXPECT_THROW(std::make_shared<QueueUpstream>(cv, cfg, tx, app_tx_queue,
                                                 app_rx_queue),
                 std::invalid_argument);
    up.reset();
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = 2;
    cfg.fec_n = 3;
    create_queue();
    app_tx_queue->push(packet({1}));
    app_tx_queue->push(packet({2}));
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
    app_tx_queue->push(packet({1}));
    pump_cv();
    EXPECT_EQ(tx->size(), 1u);
}
TEST_F(UpstreamTest, RejectsMissingAndAliasedQueues)
{
    EXPECT_THROW(std::make_shared<QueueUpstream>(cv, cfg, tx, app_tx_queue,
                                                 app_tx_queue),
                 std::invalid_argument);
    EXPECT_THROW(std::make_shared<QueueUpstream>(cv, cfg, tx, app_tx_queue,
                                                 nullptr),
                 std::invalid_argument);
    EXPECT_THROW(std::make_shared<QueueUpstream>(nullptr, cfg, tx, app_tx_queue,
                                                 app_rx_queue),
                 std::invalid_argument);
}
TEST_F(UpstreamTest, OutputEvictionCountsAcceptedNewPacket)
{
    app_rx_queue->set_capacity(1);
    create_queue();
    rx(packet({1}), false, 0);
    rx(packet({2}), false, 1);
    pump_cv();
    EXPECT_EQ(bytes(*app_rx_queue->pop()), (std::vector<uint8_t>{2}));
    auto counters = up->rx_counters();
    EXPECT_EQ(counters.accepted_packets, 2u);
    EXPECT_EQ(counters.queue_evictions, 1u);
}
TEST_F(UpstreamTest, TxOverflowKeepsNewestAndCountsDrops)
{
    tx = std::make_shared<BoundedLossyQueue>(1);
    create_queue();
    app_tx_queue->push(packet({1}));
    app_tx_queue->push(packet({2, 3}));
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
    EXPECT_EQ(app_tx_queue->capacity(), 64u);
    rx(packet({1}), false, 0);
    rx(packet({2}), false, 1);
    pump_cv();
    EXPECT_EQ(app_rx_queue->size(), 1u);
    EXPECT_EQ(bytes(*app_rx_queue->pop()), (std::vector<uint8_t>{2}));
}
TEST_F(UpstreamTest, QueueBurstIsRescheduledAfterBoundedTurn)
{
    create_queue();
    for (int i = 0; i < 60; ++i)
    {
        app_tx_queue->push(packet({uint8_t(i)}));
    }
    pump_cv(5);
    EXPECT_FALSE(app_tx_queue->has_data());
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
// Replace on the IO thread as App does: the old socket's epoll removal is
// immediate, so a replacement reusing the fd number stays registered.
TEST_F(UpstreamTest, UdpTeardownOnIoThreadKeepsReplacementArmed)
{
    uint16_t port = test::reserve_free_udp_port();
    ASSERT_NE(port, 0);
    const auto rx_addr = "127.0.0.1:" + std::to_string(port);
    create_udp(rx_addr);
    std::thread cv_worker(
        [this]()
        {
            cv->run();
        });
    io->wake_up(
        [&]
        {
            up->teardown();
            up.reset();
            create_udp(rx_addr);
        });
    pump_io();
    bfc::socket peer(bfc::create_udp4());
    send_to(peer, port, packet({5}));
    pump_io();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!tx->has_data() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cv->stop();
    cv_worker.join();
    ASSERT_EQ(tx->size(), 1u);
    EXPECT_EQ(bytes(*tx->pop()), bytes(lc_packet(packet({5}), false, 0)));
}
// Off the IO thread the old socket's epoll removal is deferred. The old fd
// stays open until it runs, so the replacement never gets the same fd number
// and the deferred removal cannot unregister it.
TEST_F(UpstreamTest, UdpTeardownOffIoThreadKeepsReplacementArmed)
{
    uint16_t port = test::reserve_free_udp_port();
    ASSERT_NE(port, 0);
    const auto rx_addr = "127.0.0.1:" + std::to_string(port);
    create_udp(rx_addr);
    up->teardown();  // IO reactor not running: removal is deferred
    up.reset();
    create_udp(rx_addr);
    pump_io();  // runs the old removal, then closes the old socket
    bfc::socket peer(bfc::create_udp4());
    send_to(peer, port, packet({5}));
    pump_io();
    pump_cv();
    ASSERT_EQ(tx->size(), 1u);
    EXPECT_EQ(bytes(*tx->pop()), bytes(lc_packet(packet({5}), false, 0)));
}
TEST_F(UpstreamTest, UdpInputEvictionIsCounted)
{
    cfg.rx_queue_capacity = 1;
    uint16_t port = test::reserve_free_udp_port();
    ASSERT_NE(port, 0);
    create_udp("127.0.0.1:" + std::to_string(port));
    bfc::socket peer(bfc::create_udp4());
    send_to(peer, port, packet({1}));
    send_to(peer, port, packet({2}));
    pump_io();
    EXPECT_EQ(up->tx_counters().app_queue_evictions, 1u);
    pump_cv();
    ASSERT_EQ(tx->size(), 1u);
    EXPECT_EQ(bytes(*tx->pop()), bytes(lc_packet(packet({2}), false, 0)));
}
TEST_F(UpstreamTest, RadioTableSharesInboxEvictionCounter)
{
    create_queue();
    RadioUpstreamTable table;
    table.add(up, tx, nullptr, 7, 7, 1);
    EXPECT_EQ(table.entries()[0].rx_queue_evictions,
              up->rx_queue_eviction_counter());
    table.entries()[0].rx_queue_evictions->fetch_add(1);
    EXPECT_EQ(up->rx_queue_evictions(), 1u);
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
    QueueUpstream local(cv, cfg, tx, app_tx_queue, app_rx_queue);
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
