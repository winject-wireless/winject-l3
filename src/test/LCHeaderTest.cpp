#include "endpoint/UpstreamStats.h"
#include "frames/LCHeader.h"

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace
{
using winject::accept_air_payload;
using winject::LCHeader;
using winject::stamp_slot_payload;
using winject::UpstreamStats;

std::vector<uint8_t> frame(UpstreamStats* tx, uint8_t bus, const uint8_t* data,
                           size_t len, bool is_fec = false)
{
    std::vector<uint8_t> out(len + LCHeader::k_len + 8);
    size_t n = 0;
    EXPECT_TRUE(stamp_slot_payload(tx, bus, is_fec, data, len, out.data(),
                                   out.size(), &n));
    out.resize(n);
    return out;
}

bool accept(UpstreamStats* rx, const std::vector<uint8_t>& pkt,
            bool* is_fec = nullptr)
{
    const uint8_t* body = nullptr;
    size_t plen = 0;
    bool fec = false;
    const bool ok =
        accept_air_payload(rx, pkt.data(), pkt.size(), &body, &plen, &fec);
    if (is_fec != nullptr)
    {
        *is_fec = fec;
    }
    return ok;
}
}  // namespace

TEST(LCHeaderTest, StampThenAccept)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t payload[] = {'h', 'i'};
    const auto pkt = frame(&tx, 0x42, payload, sizeof(payload));
    ASSERT_EQ(pkt.size(), LCHeader::k_len + sizeof(payload));
    EXPECT_EQ(pkt[0], 0x42);
    EXPECT_EQ(tx.air_tx, 1);

    const uint8_t* body = nullptr;
    size_t plen = 0;
    bool is_fec = true;
    ASSERT_TRUE(
        accept_air_payload(&rx, pkt.data(), pkt.size(), &body, &plen, &is_fec));
    ASSERT_EQ(plen, sizeof(payload));
    EXPECT_EQ(memcmp(body, payload, plen), 0);
    EXPECT_FALSE(is_fec);
}

TEST(LCHeaderTest, BusRoundTrip)
{
    UpstreamStats tx;
    tx.air_tx = 7;
    const auto pkt = frame(&tx, 0xAB, nullptr, 0);
    EXPECT_EQ(LCHeader::read_bus(pkt.data()), 0xAB);
    EXPECT_EQ(LCHeader::read_seq(pkt.data()), 7u);
    EXPECT_FALSE(LCHeader::read_is_fec(pkt.data()));
}

// The flag is the MSB of the BE seq word and does not leak into the seq.
TEST(LCHeaderTest, FecFlagWireLayout)
{
    uint8_t out[LCHeader::k_len] = {};
    LCHeader::write(out, 0xC3, 0x1234, true);
    EXPECT_EQ(out[0], 0xC3);
    EXPECT_EQ(out[1], 0x92);
    EXPECT_EQ(out[2], 0x34);
    EXPECT_TRUE(LCHeader::read_is_fec(out));
    EXPECT_EQ(LCHeader::read_seq(out), 0x1234u);

    LCHeader::write(out, 0xC3, 0x7FFF, false);
    EXPECT_EQ(out[1], 0x7F);
    EXPECT_EQ(out[2], 0xFF);
    EXPECT_FALSE(LCHeader::read_is_fec(out));
    EXPECT_EQ(LCHeader::read_seq(out), 0x7FFFu);

    // A seq wider than 15 bits never sets the flag.
    LCHeader::write(out, 1, 0xFFFF, false);
    EXPECT_FALSE(LCHeader::read_is_fec(out));
    EXPECT_EQ(LCHeader::read_seq(out), 0x7FFFu);
}

TEST(LCHeaderTest, FecFlagRoundTrip)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t a[] = {1};
    bool is_fec = false;
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, a, sizeof(a), true), &is_fec));
    EXPECT_TRUE(is_fec);
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, a, sizeof(a), false), &is_fec));
    EXPECT_FALSE(is_fec);
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);
}

TEST(LCHeaderTest, DetectsGap)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t a[] = {1};
    const uint8_t c[] = {3};
    const auto p0 = frame(&tx, 1, a, sizeof(a));
    tx.air_tx = 2;
    const auto p2 = frame(&tx, 1, c, sizeof(c));

    ASSERT_TRUE(accept(&rx, p0));
    ASSERT_TRUE(accept(&rx, p2));
    EXPECT_EQ(rx.air_rx_gap_loss, 1u);
}

TEST(LCHeaderTest, TxSeqWrapsAt15Bits)
{
    UpstreamStats tx;
    tx.air_tx = LCHeader::k_seq_mask;
    const auto last = frame(&tx, 1, nullptr, 0, true);
    EXPECT_EQ(LCHeader::read_seq(last.data()), 0x7FFFu);
    EXPECT_EQ(tx.air_tx, 0u);
    const auto first = frame(&tx, 1, nullptr, 0, true);
    EXPECT_EQ(LCHeader::read_seq(first.data()), 0u);
    EXPECT_EQ(tx.air_tx, 1u);
}

// 0x7FFF -> 0x0000 is the next seq, with or without the FEC flag set.
TEST(LCHeaderTest, NoGapAcrossWrap)
{
    for (const bool fec : {false, true})
    {
        UpstreamStats tx;
        tx.air_tx = 0x7FFE;
        UpstreamStats rx;
        for (int i = 0; i < 4; i++)
        {
            ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0, fec)));
        }
        EXPECT_EQ(rx.air_rx_gap_loss, 0u) << "fec=" << fec;
        EXPECT_EQ(rx.air_rx, 1u);
    }
}

TEST(LCHeaderTest, GapAcrossWrap)
{
    UpstreamStats tx;
    tx.air_tx = 0x7FFD;
    UpstreamStats rx;
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0, true)));
    tx.air_tx = 2;  // 0x7FFE, 0x7FFF, 0x0000, 0x0001 lost
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0, true)));
    EXPECT_EQ(rx.air_rx_gap_loss, 4u);
}

// A slot from behind (late or reordered) is delivered but neither counted as
// a gap nor allowed to move the reference back.
TEST(LCHeaderTest, OldSeqIsNotAGap)
{
    UpstreamStats tx;
    tx.air_tx = 0x0005;
    UpstreamStats rx;
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0)));
    tx.air_tx = 0x7FF0;  // 21 behind on the 15-bit ring
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0)));
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);
    EXPECT_EQ(rx.air_rx, 0x0005u);
}

TEST(LCHeaderTest, DropsRepeatOfLastSeq)
{
    UpstreamStats tx;
    tx.air_tx = 9;
    UpstreamStats rx;
    const auto pkt = frame(&tx, 1, nullptr, 0, true);
    EXPECT_TRUE(accept(&rx, pkt));
    EXPECT_FALSE(accept(&rx, pkt));
    // Same seq with the other flag value is still the same slot seq.
    tx.air_tx = 9;
    EXPECT_FALSE(accept(&rx, frame(&tx, 1, nullptr, 0, false)));
}
