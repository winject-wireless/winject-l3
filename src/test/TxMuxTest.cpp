#include "endpoint/Upstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/PhyAirtime.h"
#include "radio/RadioDefs.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/TxMux.h"
#include "radio/WifiUdp.h"
#include "utils/IOReactor.h"

#include <bfc/cv_reactor.hpp>
#include <bfc/socket.hpp>
#include <chrono>
#include <deque>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

using namespace winject;

namespace
{

class QueueProducer
{
public:
    static constexpr uint8_t k_bus = 7;

    struct QueueFacade
    {
        std::shared_ptr<BoundedLossyQueue> queue;
        QueueProducer* owner = nullptr;
        void push_back(const std::vector<uint8_t>& slot_payload)
        {
            queue->push(bfc::shared_sized_buffer::copy_from(
                slot_payload.data(), slot_payload.size()));
        }
        bool empty() const
        {
            return !queue->has_data();
        }
    } txq;
    explicit QueueProducer(std::shared_ptr<BoundedLossyQueue> queue)
        : txq{std::move(queue)}
    {
        txq.owner = this;
    }
    void push_sdu(const std::vector<uint8_t>& sdu, bool fec = false)
    {
        txq.push_back(frame_sdu(sdu, fec));
    }

private:
    friend struct QueueFacade;
    std::vector<uint8_t> frame_sdu(const std::vector<uint8_t>& sdu, bool fec)
    {
        std::vector<uint8_t> slot(LCHeader::k_len + sdu.size());
        LCHeader::write(slot.data(), k_bus, seq, fec);
        seq = static_cast<uint16_t>((seq + 1) & LCHeader::k_seq_mask);
        memcpy(slot.data() + LCHeader::k_len, sdu.data(), sdu.size());
        return slot;
    }
    uint16_t seq = 0;
};

std::vector<uint8_t> make_sdu(size_t len, uint8_t tag)
{
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; i++)
    {
        v[i] = static_cast<uint8_t>(tag + i);
    }
    return v;
}

// Drives a TxMux over a real WifiUdp aimed at a loopback "radio" socket and
// collects the SDUs that reach the air, in order.
class TxMuxHarness
{
public:
    explicit TxMuxHarness(size_t budget, const char* modulation = "OFDM_54M")
        : radio_sock(bfc::create_udp4()), tx_mux(upstream_table)
    {
        sockaddr_in bind_addr = {};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = 0;
        EXPECT_EQ(radio_sock.bind(bind_addr), 0);
        socklen_t len = sizeof(bind_addr);
        getsockname(radio_sock.fd(), reinterpret_cast<sockaddr*>(&bind_addr),
                    &len);

        radio_udp = std::make_shared<WifiUdp>();
        EXPECT_TRUE(radio_udp->open(io_reactor, [](bfc::shared_sized_buffer) {}));
        radio_udp->set_tx_dplane(bind_addr);

        producer = std::make_shared<QueueProducer>(tx_mux.make_tx_queue());
        upstream_table.add(std::make_shared<ReactiveLossyQueue>(), producer->txq.queue,
                           radio_udp, QueueProducer::k_bus, QueueProducer::k_bus,
                           budget);
        tx_mux.configure(1);
        PhyMode mode;
        EXPECT_TRUE(phy_mode_from_name(modulation, 1, &mode));
        tx_mux.set_phy_mode(mode);
    }

    ~TxMuxHarness()
    {
        tx_mux.stop();
        radio_udp->close();
    }

    QueueProducer& up()
    {
        return *producer;
    }

    TxMux& mux()
    {
        return tx_mux;
    }

    RadioUpstreamTable& table()
    {
        return upstream_table;
    }

    void fail_radio_sends()
    {
        radio_udp->set_tx_dplane(sockaddr_in{});
    }

    IOReactor& reactor()
    {
        return io_reactor;
    }
    void collect_pending()
    {
        collect();
    }
    size_t received_count() const
    {
        return received_pdus.size();
    }

    // The test owns the reactor; TxMux must not create another thread.
    size_t run_for(std::chrono::microseconds window)
    {
        tx_mux.start(io_reactor);
        io_reactor.get_timer().wait_us(window.count(),
                                     [this]()
                                     {
                                         io_reactor.stop();
                                     });
        io_reactor.run();
        collect();
        return received_pdus.size();
    }

    size_t wait_received(size_t count, std::chrono::milliseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (received_pdus.size() < count &&
               std::chrono::steady_clock::now() < end)
        {
            run_for(std::chrono::milliseconds(1));
        }
        return received_pdus.size();
    }

    std::vector<std::vector<uint8_t>> drain()
    {
        tx_mux.start(io_reactor);
        tx_mux.request_tick();
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!producer->txq.empty() && std::chrono::steady_clock::now() < end)
        {
            run_for(std::chrono::milliseconds(2));
        }
        collect();
        return received_pdus;
    }

    // LC FEC flag and seq of every slot collected, in order.
    const std::vector<bool>& received_fec() const
    {
        return received_fec_flags;
    }
    const std::vector<uint16_t>& received_seq() const
    {
        return received_seq_nums;
    }

private:
    void collect()
    {
        uint8_t buf[WIFI_RADIO_INJECT_MAX + 64];
        for (;;)
        {
            const ssize_t n =
                ::recv(radio_sock.fd(), buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0)
            {
                return;
            }
            Mpdu mpdu(buf, static_cast<size_t>(n));
            ASSERT_TRUE(mpdu.rescan());
            for (uint8_t s = 0; s < WIFI_PDU_SLOTS; s++)
            {
                bfc::const_buffer_view slot =
                    static_cast<const Mpdu&>(mpdu).get_slot_payload(s);
                if (slot.empty())
                {
                    continue;
                }
                ASSERT_GE(slot.size(), LCHeader::k_len);
                const uint8_t* p =
                    reinterpret_cast<const uint8_t*>(slot.data());
                EXPECT_EQ(p[0], QueueProducer::k_bus);
                received_fec_flags.push_back(LCHeader::read_is_fec(p));
                received_seq_nums.push_back(LCHeader::read_seq(p));
                received_pdus.emplace_back(p + LCHeader::k_len, p + slot.size());
            }
        }
    }

    IOReactor io_reactor;
    bfc::socket radio_sock;
    RadioUpstreamTable upstream_table;
    TxMux tx_mux;
    std::shared_ptr<WifiUdp> radio_udp;
    std::shared_ptr<QueueProducer> producer;
    std::vector<std::vector<uint8_t>> received_pdus;
    std::vector<bool> received_fec_flags;
    std::vector<uint16_t> received_seq_nums;
};

}  // namespace

// An SDU that fits the remaining share but not with its LC header used to be
// pulled from the queue and then discarded.
TEST(TxMuxTest, SduNearShareLimitIsNotDropped)
{
    TxMuxHarness h(200);
    const std::vector<std::vector<uint8_t>> sdus = {
        make_sdu(100, 1), make_sdu(96, 2), make_sdu(50, 3)};
    for (const auto& s : sdus)
    {
        h.up().push_sdu(s);
    }
    EXPECT_EQ(h.drain(), sdus);
    EXPECT_TRUE(h.up().txq.empty());
}

TEST(TxMuxTest, SduExactlyBudgetIsNotDropped)
{
    TxMuxHarness h(100);
    const std::vector<std::vector<uint8_t>> sdus = {
        make_sdu(98, 1), make_sdu(99, 2), make_sdu(100, 3)};
    for (const auto& s : sdus)
    {
        h.up().push_sdu(s);
    }
    EXPECT_EQ(h.drain(), sdus);
}

// With the default budget (256), an SDU larger than the budget used to block
// the upstream forever.
TEST(TxMuxTest, SduLargerThanBudgetIsSent)
{
    TxMuxHarness h(256);
    const std::vector<std::vector<uint8_t>> sdus = {
        make_sdu(1000, 1), make_sdu(k_stream_payload_max, 2), make_sdu(10, 3)};
    for (const auto& s : sdus)
    {
        h.up().push_sdu(s);
    }
    EXPECT_EQ(h.drain(), sdus);
    EXPECT_TRUE(h.up().txq.empty());
}

// Pre-framed slot payloads pass through the mux with FEC flag and seq intact.
TEST(TxMuxTest, ForwardsPreframedFecFlagPerSdu)
{
    TxMuxHarness h(4096);
    std::vector<std::vector<uint8_t>> sdus = {
        make_sdu(40, 0xF1), make_sdu(40, 0x10), make_sdu(40, 0xF1),
        make_sdu(40, 0x20)};
    const std::vector<bool> fec = {false, true, true, false};
    for (size_t i = 0; i < sdus.size(); i++)
    {
        h.up().push_sdu(sdus[i], fec[i]);
    }
    EXPECT_EQ(h.drain(), sdus);
    EXPECT_EQ(h.received_fec(), fec);
    const auto& seq = h.received_seq();
    ASSERT_EQ(seq.size(), sdus.size());
    for (size_t i = 1; i < seq.size(); i++)
    {
        EXPECT_EQ(seq[i], static_cast<uint16_t>((seq[i - 1] + 1) &
                                                LCHeader::k_seq_mask));
    }
}

// Full frame at OFDM 24M: 530 us airtime + 96 us gap, at 90 % = 696 us.
TEST(TxMuxTest, PacingIntervalIsAirtimePlusGapOverNinetyPercent)
{
    TxMuxHarness h(4096, "OFDM_24M");
    EXPECT_EQ(h.mux().pacing_txtime_full_us(), 530u);
    EXPECT_EQ(h.mux().pacing_gap_us(), 96u);
    EXPECT_EQ(h.mux().pacing_interval_full_us(), 696u);
}

// Each MPDU is spaced by its whole on-air size (802.11 header + slots + FCS),
// so a backlog drains at the paced rate, not faster.
TEST(TxMuxTest, BacklogDrainsAtPacedRate)
{
    TxMuxHarness h(4096, "OFDM_24M");
    constexpr size_t k_sdu = 1400;
    for (int i = 0; i < 200; i++)
    {
        h.up().push_sdu(make_sdu(k_sdu, static_cast<uint8_t>(i)));
    }
    PhyMode mode;
    ASSERT_TRUE(phy_mode_from_name("OFDM_24M", 1, &mode));
    const size_t mpdu_len = WIFI_HDR_LEN + LCHeader::k_len + k_sdu;
    const uint32_t iv_us =
        ((phy_txtime_us(mode, mpdu_len + 4) + 96) * 100 + 89) / 90;

    // Time only the reactor run: setup before it (slow under TSan) is idle
    // time the mux may make up by at most two intervals.
    const auto start = std::chrono::steady_clock::now();
    const size_t sent = h.run_for(std::chrono::milliseconds(60));
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    const size_t expected = static_cast<size_t>(elapsed_us / iv_us) + 1;
    // Never faster than paced, apart from the two-interval catch-up.
    EXPECT_LE(sent, expected + 2) << "iv_us=" << iv_us;
    EXPECT_GE(sent * 4, expected * 3) << "iv_us=" << iv_us;
}

TEST(TxMuxTest, IdleHasNoPollingAndProducerWakeRunsOnIoReactor)
{
    TxMuxHarness h(4096, "OFDM_24M");
    h.run_for(std::chrono::milliseconds(2));
    int64_t deadline = 0;
    EXPECT_FALSE(h.reactor().get_timer().get_next_deadline_us(deadline));
    h.up().push_sdu(make_sdu(100, 1));
    // The bound TX queue wakes the scheduler automatically after push.
    EXPECT_EQ(h.wait_received(1, std::chrono::milliseconds(100)), 1u);
    EXPECT_FALSE(h.reactor().get_timer().get_next_deadline_us(deadline));
}

// A scheduler budget below the SDU size (default 256) weights upstreams; it
// must not cap the rate. One 1400 B SDU spends the share, and the next round
// refills it while pacing is open, instead of waiting for a periodic poll.
TEST(TxMuxTest, SmallBudgetDoesNotCapRate)
{
    TxMuxHarness h(256, "OFDM_24M");
    constexpr size_t k_frames = 40;
    // Queue pushes wake TxMux, which takes the table lock: never push under it.
    for (size_t i = 0; i < k_frames; i++)
    {
        h.up().push_sdu(make_sdu(1400, static_cast<uint8_t>(i)));
    }
    // Start behind schedule, as after any stall: pacing credit is open.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    h.mux().start(h.reactor());
    h.mux().request_tick();
    // ~669 us per frame: ~27 ms paced; 40 x 5 ms = 200 ms if the share capped
    // each wake to one frame.
    EXPECT_EQ(h.wait_received(k_frames, std::chrono::milliseconds(80)),
              k_frames);
    h.mux().stop();
}

TEST(TxMuxTest, SharesCvReactorAndDrainsFromPacingTimers)
{
    TxMuxHarness h(256, "OFDM_24M");
    bfc::cv_reactor<> cv;
    for (int i = 0; i < 20; ++i)
    {
        h.up().push_sdu(make_sdu(1400, i));
    }
    h.mux().start(cv);
    cv.get_timer().wait_ms(60,
                           [&]()
                           {
                               cv.stop();
                           });
    cv.run();
    h.collect_pending();
    EXPECT_EQ(h.received_count(), 20u);
    int64_t deadline = 0;
    EXPECT_FALSE(cv.get_timer().get_next_deadline_us(deadline));
    h.mux().stop();
}

// A coarse blocking reactor cannot honor sub-step wakeups. The mux must
// batch using its existing bounded pacing credit rather than request those
// wakeups or limit TX to one MPDU per coarse tick.
TEST(TxMuxTest, CoarseTimerUsesReportedStepAndKeepsPacedThroughput)
{
    TxMuxHarness h(4096, "OFDM_24M");
    struct CoarseReactor
    {
        static constexpr uint64_t minimum_timer_step() { return 1000; }
        bfc::cv_reactor<> cv;
        struct Timer
        {
            bfc::cv_reactor<>& cv;
            size_t positive_arms = 0;
            size_t sub_step_arms = 0;
            auto wait_us(int64_t us, bfc::light_function<void()> fn)
            {
                if (us > 0)
                {
                    ++positive_arms;
                    if (us < 1000 || us % 1000 != 0) ++sub_step_arms;
                }
                return cv.get_timer().wait_us(us, std::move(fn));
            }
            void cancel(std::pair<int64_t, uint64_t> id)
            {
                cv.get_timer().cancel(id);
            }
        } timer{cv};
        auto& get_timer() { return timer; }
        void wake_up(bfc::light_function<void()> fn)
        {
            cv.wake_up(std::move(fn));
        }
    } coarse;
    for (int i = 0; i < 100; ++i)
    {
        h.up().push_sdu(make_sdu(1400, i));
    }
    h.mux().start(coarse);
    // Collect frequently so the loopback socket does not become the limit.
    const auto start = std::chrono::steady_clock::now();
    coarse.cv.get_timer().wait_ms(60, [&] { coarse.cv.stop(); });
    std::function<void()> collect;
    collect = [&]
    {
        h.collect_pending();
        coarse.cv.get_timer().wait_ms(2, [&collect] { collect(); });
    };
    coarse.cv.get_timer().wait_ms(2, [&collect] { collect(); });
    coarse.cv.run();
    h.collect_pending();
    h.mux().stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start).count();
    // About 90 full SDUs fit in 60 ms; one per millisecond would deliver ~60.
    // Allow scheduling noise, while catching that throughput regression.
    const size_t expected = std::min<size_t>(100, elapsed / 669 + 1);
    EXPECT_GE(h.received_count() * 4, expected * 3);
    EXPECT_LE(h.received_count(), expected + 2);
    EXPECT_GT(coarse.timer.positive_arms, 0u);
    EXPECT_EQ(coarse.timer.sub_step_arms, 0u);
}

// On FreeRTOS a timed wait also sleeps one tick more than asked, so a 1 ms
// step reactor wakes up to ~2 ms apart. Two full frames of credit (1392 us at
// OFDM_24M) is less than that gap, so the schedule's lag was clamped and
// intervals were lost: ~93% of the paced rate here, against ~99% with credit
// for three steps.
TEST(TxMuxTest, TickLateCoarseTimerKeepsPacedThroughput)
{
    TxMuxHarness h(4096, "OFDM_24M");
    struct TickLateReactor
    {
        static constexpr uint64_t minimum_timer_step() { return 1000; }
        bfc::cv_reactor<> cv;
        struct Timer
        {
            bfc::cv_reactor<>& cv;
            auto wait_us(int64_t us, bfc::light_function<void()> fn)
            {
                return cv.get_timer().wait_us(us > 0 ? us + 1000 : us,
                                              std::move(fn));
            }
            void cancel(std::pair<int64_t, uint64_t> id)
            {
                cv.get_timer().cancel(id);
            }
        } timer{cv};
        auto& get_timer() { return timer; }
        void wake_up(bfc::light_function<void()> fn)
        {
            cv.wake_up(std::move(fn));
        }
    } late;
    for (int i = 0; i < 200; ++i)
    {
        h.up().push_sdu(make_sdu(1400, i));
    }
    h.mux().start(late);
    const auto start = std::chrono::steady_clock::now();
    late.cv.get_timer().wait_ms(100, [&] { late.cv.stop(); });
    std::function<void()> collect;
    collect = [&]
    {
        h.collect_pending();
        late.cv.get_timer().wait_ms(2, [&collect] { collect(); });
    };
    late.cv.get_timer().wait_ms(2, [&collect] { collect(); });
    late.cv.run();
    h.collect_pending();
    h.mux().stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start).count();
    // ~150 full SDUs fit in 100 ms at the paced rate; the clamped schedule
    // delivered 140-142.
    const size_t expected = std::min<size_t>(200, elapsed / 669 + 1);
    EXPECT_GE(h.received_count() * 100, expected * 97);
    EXPECT_LE(h.received_count(), expected + 4);
}

TEST(TxMuxTest, NotificationsAreCoalesced)
{
    TxMuxHarness h(4096);
    struct Probe
    {
        static constexpr uint64_t minimum_timer_step()
        {
            return IOReactor::minimum_timer_step();
        }
        IOReactor& io;
        size_t wakes = 0;
        auto& get_timer()
        {
            return io.get_timer();
        }
        void wake_up(std::function<void()> cb)
        {
            ++wakes;
            io.wake_up(std::move(cb));
        }
    } probe{h.reactor()};
    h.mux().start(probe);
    for (int i = 0; i < 100; ++i)
    {
        h.mux().request_tick();
    }
    EXPECT_EQ(probe.wakes, 1u);
    h.run_for(std::chrono::milliseconds(1));
    h.up().push_sdu(make_sdu(100, 1));
    for (int i = 0; i < 100; ++i)
    {
        h.mux().request_tick();
    }
    EXPECT_EQ(probe.wakes, 2u);
    EXPECT_EQ(h.run_for(std::chrono::milliseconds(2)), 1u);
    h.mux().stop();
}

TEST(TxMuxTest, CrossThreadProducerWakesSharedReactor)
{
    TxMuxHarness h(4096);
    h.run_for(std::chrono::milliseconds(1));
    std::thread producer(
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            h.up().push_sdu(make_sdu(100, 7));
            h.mux().request_tick();
        });
    h.run_for(std::chrono::milliseconds(30));
    producer.join();
    EXPECT_EQ(h.received_count(), 1u);
}

TEST(TxMuxTest, StopCancelsPacingAndRestartFindsRemainingBacklog)
{
    TxMuxHarness h(256, "DSS_1M_L");
    for (int i = 0; i < 5; ++i)
    {
        h.up().push_sdu(make_sdu(1400, i));
    }
    h.run_for(std::chrono::milliseconds(1));
    ASSERT_FALSE(h.up().txq.empty());
    h.mux().stop();
    const size_t sent = h.received_count();
    // Run IO without restarting TxMux; old callbacks must not transmit.
    h.reactor().get_timer().wait_ms(20,
                                    [&]()
                                    {
                                        h.reactor().stop();
                                    });
    h.reactor().run();
    h.collect_pending();
    EXPECT_EQ(h.received_count(), sent);
    h.mux().start(h.reactor());
    EXPECT_EQ(h.wait_received(5, std::chrono::milliseconds(150)), 5u);
}

TEST(TxMuxTest, PendingCallbackDoesNotTouchDestroyedMux)
{
    IOReactor reactor;
    RadioUpstreamTable table;
    auto mux = std::make_unique<TxMux>(table);
    mux->start(reactor);
    mux.reset();
    reactor.get_timer().wait_ms(1,
                                [&]()
                                {
                                    reactor.stop();
                                });
    reactor.run();
}

TEST(TxMuxTest, FailedSubmissionsDoNotStrandRemainingBacklog)
{
    TxMuxHarness h(4096, "OFDM_24M");
    h.fail_radio_sends();
    for (int i = 0; i < 5; ++i)
    {
        h.up().push_sdu(make_sdu(1400, i));
    }
    h.run_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(h.up().txq.empty());
    EXPECT_EQ(h.mux().tx_send_fail_mpdu(), 5u);
    int64_t deadline = 0;
    EXPECT_FALSE(h.reactor().get_timer().get_next_deadline_us(deadline));
}
