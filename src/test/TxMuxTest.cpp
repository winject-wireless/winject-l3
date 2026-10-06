#include "endpoint/Upstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/PhyAirtime.h"
#include "radio/RadioDefs.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/TxMux.h"
#include "radio/WifiUdp.h"
#include "utils/IOReactor.h"

#include <bfc/socket.hpp>
#include <chrono>
#include <deque>
#include <gtest/gtest.h>
#include <memory>
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

    void on_radio_rx(bfcext::shared_sized_buffer /*pkt*/,
                     bool /*is_fec*/) override
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
    explicit TxMuxHarness(size_t budget)
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
        EXPECT_TRUE(radio_->open(reactor_, [](bfcext::shared_sized_buffer) {}));
        radio_->set_tx_dplane(bind_addr);

        up_ = std::make_shared<QueueUpstream>();
        table_.add(up_, radio_, k_bus, k_bus, budget);
        mux_.configure(1, 32);
        PhyMode mode;
        EXPECT_TRUE(phy_mode_from_name("OFDM_54M", 1, &mode));
        mux_.set_phy_mode(mode, 0, 0);
    }

    ~TxMuxHarness()
    {
        radio_->close();
    }

    QueueUpstream& up()
    {
        return *up_;
    }

    // Ticks until the upstream queue is empty (or a deadline passes), then
    // returns every SDU received on the radio socket. The sleep before each
    // tick lets pacing credit build so one tick can send several MPDUs.
    std::vector<std::vector<uint8_t>> drain()
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            mux_.sync_tick();
            collect();
            if (up_->txq.empty())
            {
                break;
            }
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
