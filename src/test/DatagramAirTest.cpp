#include "endpoint/DatagramAir.h"
#include "frames/LCHeader.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace winject::core;
namespace
{
using bytes = std::vector<uint8_t>;
using std::chrono::milliseconds;

// The CV reactor on its own thread: DatagramAir's data path and its streams'
// timers run there, as in an upstream.
struct CvThread
{
    std::shared_ptr<DatagramAir::Reactor> cv =
        std::make_shared<DatagramAir::Reactor>(10);
    std::thread thread{[cv = cv] { cv->run(); }};
    ~CvThread()
    {
        cv->stop();
        thread.join();
    }
    void run_on(const std::function<void()>& fn)
    {
        std::promise<void> done;
        cv->wake_up(
            [&]
            {
                fn();
                done.set_value();
            });
        done.get_future().wait();
    }
};

template <typename F>
bool wait_for(F f, milliseconds timeout = milliseconds(2000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end)
    {
        if (f())
        {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return f();
}

struct Slot
{
    uint8_t bus = 0;
    uint8_t type = 0;
    uint16_t sn = 0;
    bfc::shared_sized_buffer body;
};

// One end of a link: a DatagramAir with its radio TX queue and what it
// delivers to the app.
struct End
{
    CvThread& cvt;
    std::shared_ptr<BoundedLossyQueue> tx_queue =
        std::make_shared<BoundedLossyQueue>();
    UpstreamStats tx_stats;
    std::mutex tx_stats_mu;
    std::shared_ptr<int> owner = std::make_shared<int>(0);
    std::mutex got_mu;
    std::vector<bytes> got;
    std::unique_ptr<DatagramAir> air = std::make_unique<DatagramAir>();

    explicit End(CvThread& c) : cvt(c) {}
    ~End()
    {
        air.reset();
    }
    bool start(const UpstreamConfig& cfg)
    {
        const bool ok = air->start(
            tx_queue,
            [this](const uint8_t* p, size_t n)
            {
                std::lock_guard<std::mutex> lock(got_mu);
                got.emplace_back(p, p + n);
            },
            cvt.cv, cfg, cfg.bus_tx, &tx_stats, &tx_stats_mu);
        if (ok)
        {
            air->set_lifetime(owner);
        }
        return ok;
    }
    void send(const bytes& b)
    {
        cvt.run_on([&] { air->accept_app_datagram(b.data(), b.size()); });
    }
    void receive(const Slot& s)
    {
        cvt.run_on([&] { air->on_radio_rx(s.body, LcType(s.type)); });
    }
    std::vector<Slot> pull()
    {
        std::vector<Slot> v;
        while (auto p = tx_queue->pop())
        {
            Slot s;
            s.bus = LCHeader::read_bus(p->u8());
            s.type = LCHeader::read_type(p->u8());
            s.sn = LCHeader::read_seq(p->u8());
            s.body = p->subview(k_slot_prefix_len, p->size() - k_slot_prefix_len);
            v.push_back(std::move(s));
        }
        return v;
    }
    std::vector<bytes> received()
    {
        std::lock_guard<std::mutex> lock(got_mu);
        return got;
    }
    size_t received_count()
    {
        std::lock_guard<std::mutex> lock(got_mu);
        return got.size();
    }
};

UpstreamConfig fec_cfg(int k, int n, int timeout_ms = 20, int spread_ms = 0)
{
    UpstreamConfig cfg;
    cfg.bus_tx = 7;
    cfg.fec_type = FecType::RsBlockErasure;
    cfg.fec_k = k;
    cfg.fec_n = n;
    cfg.fec_timeout_ms = timeout_ms;
    cfg.fec_spread_ms = spread_ms;
    return cfg;
}

UpstreamConfig plain_cfg()
{
    UpstreamConfig cfg;
    cfg.bus_tx = 7;
    return cfg;
}

bytes app(uint8_t i, size_t len = 3)
{
    bytes b(len, i);
    b[0] = i;
    return b;
}
}  // namespace

TEST(DatagramAirTest, PlainGoesOutAtOnceWithLcPrefix)
{
    CvThread cvt;
    End a(cvt);
    ASSERT_TRUE(a.start(plain_cfg()));
    a.send(app(1));
    a.send(app(2));
    const auto slots = a.pull();
    ASSERT_EQ(slots.size(), 2u);
    for (size_t i = 0; i < 2; i++)
    {
        EXPECT_EQ(slots[i].bus, 7);
        EXPECT_EQ(slots[i].type, static_cast<uint8_t>(LcType::plain));
        EXPECT_EQ(bytes(slots[i].body.u8(), slots[i].body.u8() + 3),
                  app(uint8_t(i + 1)));
    }
    EXPECT_EQ(uint16_t(slots[1].sn - slots[0].sn), 1u);
    EXPECT_EQ(a.air->fec_air_tx_packets(), 0u);
}

// Data shards go out as they come; parity follows the block's last data
// shard (spread 0). Every slot takes the next sn.
TEST(DatagramAirTest, FecDataAtOnceParityAtClose)
{
    CvThread cvt;
    End a(cvt);
    ASSERT_TRUE(a.start(fec_cfg(2, 3)));
    a.send(app(1));
    auto slots = a.pull();
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots[0].type, static_cast<uint8_t>(LcType::fec));
    a.send(app(2));
    const auto more = a.pull();
    ASSERT_EQ(more.size(), 2u);
    slots.insert(slots.end(), more.begin(), more.end());
    for (size_t i = 0; i < slots.size(); i++)
    {
        EXPECT_EQ(slots[i].type, static_cast<uint8_t>(LcType::fec));
        EXPECT_EQ(uint16_t(slots[i].sn - slots[0].sn), i);
    }
    EXPECT_EQ(a.air->fec_air_tx_packets(), 3u);
}

TEST(DatagramAirTest, TimeoutFlushesShortBlockOnTheReactor)
{
    CvThread cvt;
    End a(cvt);
    ASSERT_TRUE(a.start(fec_cfg(3, 4, 5)));
    a.send(app(1));
    EXPECT_EQ(a.pull().size(), 1u);  // the data shard
    ASSERT_TRUE(wait_for([&] { return a.tx_queue->size() == 1; }));
    EXPECT_EQ(a.pull()[0].type, static_cast<uint8_t>(LcType::fec));
}

// With a spread, parity waits for its place instead of following the data.
TEST(DatagramAirTest, SpreadHoldsParityBack)
{
    CvThread cvt;
    End a(cvt);
    ASSERT_TRUE(a.start(fec_cfg(2, 4, 20, 40)));
    const auto t0 = std::chrono::steady_clock::now();
    a.send(app(1));
    a.send(app(2));
    EXPECT_EQ(a.pull().size(), 2u);
    ASSERT_TRUE(wait_for([&] { return a.tx_queue->size() == 2; }));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, milliseconds(25));
}

// Two ends over a lossy hop: the receiver rebuilds the lost data shard and
// delivers everything once, in order.
TEST(DatagramAirTest, LoopbackRecoversLostDataInOrder)
{
    CvThread cvt;
    End a(cvt);
    End b(cvt);
    ASSERT_TRUE(a.start(fec_cfg(4, 6)));
    ASSERT_TRUE(b.start(plain_cfg()));
    std::vector<bytes> sent;
    for (uint8_t i = 0; i < 8; i++)
    {
        sent.push_back(app(i, 10 + i));
        a.send(sent.back());
    }
    const auto slots = a.pull();
    ASSERT_EQ(slots.size(), 12u);
    for (size_t i = 0; i < slots.size(); i++)
    {
        if (i == 1 || i == 8)  // a data shard of each block
        {
            continue;
        }
        b.receive(slots[i]);
    }
    ASSERT_TRUE(wait_for([&] { return b.received_count() == 8; }));
    EXPECT_EQ(b.received(), sent);
    EXPECT_EQ(b.air->fec_recovered(), 2u);
    EXPECT_EQ(b.air->fec_decode_fail(), 0u);
    // b sends plain: FEC from the peer is counted as unexpected.
    EXPECT_EQ(b.air->fec_air_rx_unexpected(), 10u);
}

// A hole with no parity to rebuild it is given up on the reactor timer, with
// nothing more arriving; what follows it is then released.
TEST(DatagramAirTest, RxGivesUpHoleOnItsTimer)
{
    CvThread cvt;
    End a(cvt);
    End b(cvt);
    ASSERT_TRUE(a.start(fec_cfg(3, 4)));
    ASSERT_TRUE(b.start(fec_cfg(3, 4)));
    for (uint8_t i = 0; i < 3; i++)
    {
        a.send(app(i));
    }
    const auto slots = a.pull();
    ASSERT_EQ(slots.size(), 4u);
    b.receive(slots[0]);
    b.receive(slots[2]);  // data 1 and the parity lost
    EXPECT_EQ(b.received(), (std::vector<bytes>{app(0)}));
    ASSERT_TRUE(wait_for([&] { return b.received_count() == 2; },
                         milliseconds(1000)));
    EXPECT_EQ(b.received()[1], app(2));
    EXPECT_GE(b.air->fec_decode_fail(), 1u);
}

TEST(DatagramAirTest, PayloadStaysOpaque)
{
    CvThread cvt;
    End b(cvt);
    ASSERT_TRUE(b.start(plain_cfg()));
    // Bytes that look like a FEC header: only the LC type decides.
    bytes telemetry(68, 0);
    telemetry[0] = 0x80;
    telemetry[4] = 'V';
    Slot s;
    s.type = static_cast<uint8_t>(LcType::plain);
    s.body = bfc::shared_sized_buffer::copy_from(telemetry.data(),
                                                 telemetry.size());
    b.receive(s);
    EXPECT_EQ(b.received(), (std::vector<bytes>{telemetry}));
}

TEST(DatagramAirTest, EncodedBacklogIsBoundedAndDropsCounted)
{
    CvThread cvt;
    End a(cvt);
    ASSERT_TRUE(a.start(fec_cfg(2, 3)));
    cvt.run_on(
        [&]
        {
            const uint8_t p = 1;
            for (int i = 0; i < 10000; ++i)
            {
                a.air->accept_app_datagram(&p, 1);
            }
        });
    EXPECT_EQ(a.tx_queue->size(), 1024u);
    EXPECT_EQ(a.air->tx_queue_drops(), 15000u - 1024u);
}

TEST(DatagramAirTest, OversizeIsDroppedAndCounted)
{
    CvThread cvt;
    End a(cvt);
    ASSERT_TRUE(a.start(fec_cfg(2, 3)));
    winject::fec::fec_write_stream::config w;
    ASSERT_TRUE(DatagramAir::fec_write_config(fec_cfg(2, 3), &w));
    const size_t max = winject::fec::fec_write_stream::max_original(w);
    a.send(bytes(max, 1));
    a.send(bytes(max + 1, 1));
    EXPECT_EQ(a.air->app_rx_oversize_pkt(), 1u);
    EXPECT_EQ(a.pull().size(), 1u);
    // Plain takes the whole slot body.
    End p(cvt);
    ASSERT_TRUE(p.start(plain_cfg()));
    p.send(bytes(k_stream_payload_max, 1));
    p.send(bytes(k_stream_payload_max + 1, 1));
    EXPECT_EQ(p.air->app_rx_oversize_pkt(), 1u);
    EXPECT_EQ(p.pull().size(), 1u);
}

TEST(DatagramAirTest, FecConfigRules)
{
    EXPECT_TRUE(DatagramAir::fec_write_config(plain_cfg(), nullptr));
    EXPECT_TRUE(DatagramAir::fec_write_config(fec_cfg(1, 2), nullptr));
    EXPECT_TRUE(DatagramAir::fec_write_config(fec_cfg(30, 31), nullptr));
    EXPECT_TRUE(DatagramAir::fec_write_config(fec_cfg(8, 12, 20, 250), nullptr));
    EXPECT_FALSE(DatagramAir::fec_write_config(fec_cfg(4, 4), nullptr));
    EXPECT_FALSE(DatagramAir::fec_write_config(fec_cfg(8, 32), nullptr));
    EXPECT_FALSE(DatagramAir::fec_write_config(fec_cfg(2, 3, 0), nullptr));
    EXPECT_FALSE(DatagramAir::fec_write_config(fec_cfg(2, 3, 20, 251), nullptr));
}

// A restart carries the FEC sequence, and a new config replaces the old FEC.
TEST(DatagramAirTest, RestartCarriesSequenceAndDropsOldFec)
{
    CvThread cvt;
    End a(cvt);
    ASSERT_TRUE(a.start(fec_cfg(2, 3)));
    a.send(app(1));
    a.pull();
    const uint16_t next = a.air->fec_tx_sdu_seq();
    a.air->stop();
    EXPECT_EQ(a.air->fec_tx_sdu_seq(), next);
    ASSERT_TRUE(a.start(plain_cfg()));
    a.send(app(2));
    const auto slots = a.pull();
    ASSERT_EQ(slots.size(), 1u);  // no parity of the dropped block
    EXPECT_EQ(slots[0].type, static_cast<uint8_t>(LcType::plain));
    a.air->stop();
    ASSERT_TRUE(a.start(fec_cfg(2, 3)));
    a.send(app(3));
    const auto fec = a.pull();
    ASSERT_EQ(fec.size(), 1u);
    // Data shard header: u8 flag | u16 seq.
    const uint16_t seq =
        uint16_t((fec[0].body.u8()[1] << 8) | fec[0].body.u8()[2]);
    EXPECT_EQ(seq, next);
}

// Destroyed while its streams' timers are pending on a running reactor.
TEST(DatagramAirTest, DestroyWithPendingTimers)
{
    CvThread cvt;
    for (int round = 0; round < 20; round++)
    {
        End a(cvt);
        ASSERT_TRUE(a.start(fec_cfg(3, 4, 2, 10)));
        a.send(app(1));
        a.air.reset();
        a.owner.reset();
    }
    std::this_thread::sleep_for(milliseconds(20));
}
