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

class QueueUpstream : public Upstream
{
public:
    std::deque<std::vector<uint8_t>> txq;
    // Parallel to txq: whether each SDU is an FEC shard. Missing means raw.
    std::deque<bool> txq_fec;
    std::thread::id pull_thread;

    void on_radio_rx(bfc::shared_sized_buffer /*pkt*/, bool /*is_fec*/) override
    {
    }

    bool has_tx() override
    {
        return !txq.empty();
    }

    size_t get_tx_size() override
    {
        return txq.empty() ? 0 : txq.front().size();
    }

    bfc::sized_buffer pull_tx(size_t max, bool* is_fec) override
    {
        if (txq.empty() || txq.front().size() > max)
        {
            return bfc::sized_buffer();
        }
        pull_thread = std::this_thread::get_id();
        bfc::sized_buffer out(txq.front().size());
        memcpy(out.data(), txq.front().data(), txq.front().size());
        txq.pop_front();
        *is_fec = !txq_fec.empty() && txq_fec.front();
        if (!txq_fec.empty())
        {
            txq_fec.pop_front();
        }
        return out;
    }
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
        : radio_sock_(bfc::create_udp4()), mux_(table_)
    {
        sockaddr_in bind_addr = {};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = 0;
        EXPECT_EQ(radio_sock_.bind(bind_addr), 0);
        socklen_t len = sizeof(bind_addr);
        getsockname(radio_sock_.fd(), reinterpret_cast<sockaddr*>(&bind_addr),
                    &len);

        radio_ = std::make_shared<WifiUdp>();
        EXPECT_TRUE(radio_->open(reactor_, [](bfc::shared_sized_buffer) {}));
        radio_->set_tx_dplane(bind_addr);

        up_ = std::make_shared<QueueUpstream>();
        table_.add(up_, radio_, k_bus, k_bus, budget);
        mux_.configure(1);
        PhyMode mode;
        EXPECT_TRUE(phy_mode_from_name(modulation, 1, &mode));
        mux_.set_phy_mode(mode);
    }

    ~TxMuxHarness()
    {
        mux_.stop();
        radio_->close();
    }

    QueueUpstream& up()
    {
        return *up_;
    }

    TxMux& mux()
    {
        return mux_;
    }

    RadioUpstreamTable& table()
    {
        return table_;
    }

    void fail_radio_sends()
    {
        radio_->set_tx_dplane(sockaddr_in{});
    }

    IOReactor& reactor()
    {
        return reactor_;
    }
    void collect_pending()
    {
        collect();
    }
    size_t received_count() const
    {
        return received_.size();
    }

    // The test owns the reactor; TxMux must not create another thread.
    size_t run_for(std::chrono::microseconds window)
    {
        mux_.start(reactor_);
        reactor_.get_timer().wait_us(window.count(),
                                     [this]()
                                     {
                                         reactor_.stop();
                                     });
        reactor_.run();
        collect();
        return received_.size();
    }

    size_t wait_received(size_t count, std::chrono::milliseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (received_.size() < count &&
               std::chrono::steady_clock::now() < end)
        {
            run_for(std::chrono::milliseconds(1));
        }
        return received_.size();
    }

    std::vector<std::vector<uint8_t>> drain()
    {
        mux_.start(reactor_);
        mux_.request_tick();
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!up_->txq.empty() && std::chrono::steady_clock::now() < end)
        {
            run_for(std::chrono::milliseconds(2));
        }
        collect();
        return received_;
    }

    // LC FEC flag and seq of every slot collected, in order.
    const std::vector<bool>& received_fec() const
    {
        return received_fec_;
    }
    const std::vector<uint16_t>& received_seq() const
    {
        return received_seq_;
    }

private:
    static constexpr uint8_t k_bus = 7;

    void collect()
    {
        uint8_t buf[WIFI_RADIO_INJECT_MAX + 64];
        for (;;)
        {
            const ssize_t n =
                ::recv(radio_sock_.fd(), buf, sizeof(buf), MSG_DONTWAIT);
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
                EXPECT_EQ(p[0], k_bus);
                received_fec_.push_back(LCHeader::read_is_fec(p));
                received_seq_.push_back(LCHeader::read_seq(p));
                received_.emplace_back(p + LCHeader::k_len, p + slot.size());
            }
        }
    }

    IOReactor reactor_;
    bfc::socket radio_sock_;
    RadioUpstreamTable table_;
    TxMux mux_;
    std::shared_ptr<WifiUdp> radio_;
    std::shared_ptr<QueueUpstream> up_;
    std::vector<std::vector<uint8_t>> received_;
    std::vector<bool> received_fec_;
    std::vector<uint16_t> received_seq_;
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
        h.up().txq.push_back(s);
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
        h.up().txq.push_back(s);
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
        h.up().txq.push_back(s);
    }
    EXPECT_EQ(h.drain(), sdus);
    EXPECT_TRUE(h.up().txq.empty());
}

// The LC header FEC flag follows the upstream's per-SDU tag, whatever the SDU
// bytes are, and one seq counter covers both kinds.
TEST(TxMuxTest, StampsFecFlagPerSdu)
{
    TxMuxHarness h(4096);
    std::vector<std::vector<uint8_t>> sdus = {
        make_sdu(40, 0xF1), make_sdu(40, 0x10), make_sdu(40, 0xF1),
        make_sdu(40, 0x20)};
    const std::vector<bool> fec = {false, true, true, false};
    for (size_t i = 0; i < sdus.size(); i++)
    {
        h.up().txq.push_back(sdus[i]);
        h.up().txq_fec.push_back(fec[i]);
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
    const auto start = std::chrono::steady_clock::now();
    TxMuxHarness h(4096, "OFDM_24M");
    constexpr size_t k_sdu = 1400;
    for (int i = 0; i < 200; i++)
    {
        h.up().txq.push_back(make_sdu(k_sdu, static_cast<uint8_t>(i)));
    }
    PhyMode mode;
    ASSERT_TRUE(phy_mode_from_name("OFDM_24M", 1, &mode));
    const size_t mpdu_len = WIFI_HDR_LEN + LCHeader::k_len + k_sdu;
    const uint32_t iv_us =
        ((phy_txtime_us(mode, mpdu_len + 4) + 96) * 100 + 89) / 90;

    const size_t sent = h.run_for(std::chrono::milliseconds(60));
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    const size_t expected = static_cast<size_t>(elapsed_us / iv_us) + 1;
    // Never faster than paced. A slow test loop (e.g. under TSAN) can lose
    // slots beyond the two-interval catch-up, so the lower bound is loose.
    EXPECT_LE(sent, expected + 1) << "iv_us=" << iv_us;
    EXPECT_GE(sent * 4, expected * 3) << "iv_us=" << iv_us;
}

TEST(TxMuxTest, IdleHasNoPollingAndProducerWakeRunsOnIoReactor)
{
    TxMuxHarness h(4096, "OFDM_24M");
    h.run_for(std::chrono::milliseconds(2));
    int64_t deadline = 0;
    EXPECT_FALSE(h.reactor().get_timer().get_next_deadline_us(deadline));
    h.up().txq.push_back(make_sdu(100, 1));
    // No fallback poll: non-reactive queue producers must notify after push.
    EXPECT_EQ(h.run_for(std::chrono::milliseconds(10)), 0u);
    h.mux().request_tick();
    EXPECT_EQ(h.wait_received(1, std::chrono::milliseconds(100)), 1u);
    EXPECT_EQ(h.up().pull_thread, std::this_thread::get_id());
    EXPECT_FALSE(h.reactor().get_timer().get_next_deadline_us(deadline));
}

// A scheduler budget below the SDU size (default 256) weights upstreams; it
// must not cap the rate. One 1400 B SDU spends the share, and the next round
// refills it while pacing is open, instead of waiting for a periodic poll.
TEST(TxMuxTest, SmallBudgetDoesNotCapRate)
{
    TxMuxHarness h(256, "OFDM_24M");
    constexpr size_t k_frames = 40;
    {
        std::lock_guard<std::mutex> lock(h.table().mutex());
        for (size_t i = 0; i < k_frames; i++)
        {
            h.up().txq.push_back(make_sdu(1400, static_cast<uint8_t>(i)));
        }
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
        h.up().txq.push_back(make_sdu(1400, i));
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
    EXPECT_EQ(h.up().pull_thread, std::this_thread::get_id());
    int64_t deadline = 0;
    EXPECT_FALSE(cv.get_timer().get_next_deadline_us(deadline));
    h.mux().stop();
}

TEST(TxMuxTest, NotificationsAreCoalesced)
{
    TxMuxHarness h(4096);
    struct Probe
    {
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
    h.up().txq.push_back(make_sdu(100, 1));
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
            {
                std::lock_guard<std::mutex> lock(h.table().mutex());
                h.up().txq.push_back(make_sdu(100, 7));
            }
            h.mux().request_tick();
        });
    h.run_for(std::chrono::milliseconds(30));
    producer.join();
    EXPECT_EQ(h.received_count(), 1u);
    EXPECT_EQ(h.up().pull_thread, std::this_thread::get_id());
}

TEST(TxMuxTest, StopCancelsPacingAndRestartFindsRemainingBacklog)
{
    TxMuxHarness h(256, "DSS_1M_L");
    for (int i = 0; i < 5; ++i)
    {
        h.up().txq.push_back(make_sdu(1400, i));
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
        h.up().txq.push_back(make_sdu(1400, i));
    }
    h.run_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(h.up().txq.empty());
    EXPECT_EQ(h.mux().tx_send_fail_mpdu(), 5u);
    int64_t deadline = 0;
    EXPECT_FALSE(h.reactor().get_timer().get_next_deadline_us(deadline));
}
