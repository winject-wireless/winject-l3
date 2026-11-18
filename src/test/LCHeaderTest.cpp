#include "endpoint/UpstreamStats.h"
#include "frames/LCHeader.h"

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace
{
using winject::core::accept_air_payload;
using winject::core::k_slot_prefix_len;
using winject::core::LCHeader;
using winject::core::LcType;
using winject::core::stamp_slot_prefix;
using winject::core::UpstreamStats;

// A slot as DatagramAir builds it: the body already in place behind the
// prefix room, the prefix stamped in front.
std::vector<uint8_t> frame(UpstreamStats* tx, uint8_t bus, const uint8_t* data,
                           size_t len, LcType type = LcType::plain)
{
    std::vector<uint8_t> out(k_slot_prefix_len + len);
    if (len > 0)
    {
        memcpy(out.data() + k_slot_prefix_len, data, len);
    }
    EXPECT_TRUE(stamp_slot_prefix(tx, bus, type, out.data()));
    return out;
}

bool accept(UpstreamStats* rx, const std::vector<uint8_t>& pkt,
            LcType* type = nullptr)
{
    const uint8_t* body = nullptr;
    size_t plen = 0;
    LcType t = LcType::plain;
    const bool ok =
        accept_air_payload(rx, pkt.data(), pkt.size(), &body, &plen, &t);
    if (type != nullptr)
    {
        *type = t;
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
    LcType type = LcType::fec;
    ASSERT_TRUE(
        accept_air_payload(&rx, pkt.data(), pkt.size(), &body, &plen, &type));
    ASSERT_EQ(plen, sizeof(payload));
    EXPECT_EQ(memcmp(body, payload, plen), 0);
    EXPECT_EQ(type, LcType::plain);
}

// u8 bus | u8 lc_type | u16 sn (big endian) | body
TEST(LCHeaderTest, WireLayout)
{
    EXPECT_EQ(LCHeader::k_len, 4u);
    uint8_t out[LCHeader::k_len] = {};
    LCHeader::write(out, 0xC3, LcType::fec, 0x1234);
    EXPECT_EQ(out[0], 0xC3);
    EXPECT_EQ(out[1], 1);
    EXPECT_EQ(out[2], 0x12);
    EXPECT_EQ(out[3], 0x34);
    EXPECT_EQ(LCHeader::read_bus(out), 0xC3);
    EXPECT_EQ(LCHeader::read_type(out), 1);
    EXPECT_EQ(LCHeader::read_seq(out), 0x1234u);

    LCHeader::write(out, 0xC3, LcType::plain, 0xFFFF);
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[2], 0xFF);
    EXPECT_EQ(out[3], 0xFF);
    EXPECT_EQ(LCHeader::read_seq(out), 0xFFFFu);
}

TEST(LCHeaderTest, TypeRoundTrip)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t a[] = {1};
    LcType type = LcType::plain;
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, a, sizeof(a), LcType::fec), &type));
    EXPECT_EQ(type, LcType::fec);
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, a, sizeof(a), LcType::plain), &type));
    EXPECT_EQ(type, LcType::plain);
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);
}

// A type this build does not know is dropped and counted; its sn still counts
// as on the air, so it is not taken for a gap.
TEST(LCHeaderTest, UnknownTypeIsDroppedAndCounted)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t a[] = {1};
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, a, sizeof(a))));
    auto odd = frame(&tx, 1, a, sizeof(a));
    odd[1] = 7;
    EXPECT_FALSE(accept(&rx, odd));
    EXPECT_EQ(rx.air_rx_lc_type_unknown, 1u);
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, a, sizeof(a))));
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

TEST(LCHeaderTest, TxSeqWrapsAt16Bits)
{
    UpstreamStats tx;
    tx.air_tx = 0xFFFF;
    const auto last = frame(&tx, 1, nullptr, 0, LcType::fec);
    EXPECT_EQ(LCHeader::read_seq(last.data()), 0xFFFFu);
    EXPECT_EQ(tx.air_tx, 0u);
    const auto first = frame(&tx, 1, nullptr, 0, LcType::fec);
    EXPECT_EQ(LCHeader::read_seq(first.data()), 0u);
    EXPECT_EQ(tx.air_tx, 1u);
}

// 0xFFFF -> 0x0000 is the next sn, whatever the type.
TEST(LCHeaderTest, NoGapAcrossWrap)
{
    for (const LcType type : {LcType::plain, LcType::fec})
    {
        UpstreamStats tx;
        tx.air_tx = 0xFFFE;
        UpstreamStats rx;
        for (int i = 0; i < 4; i++)
        {
            ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0, type)));
        }
        EXPECT_EQ(rx.air_rx_gap_loss, 0u);
        EXPECT_EQ(rx.air_rx, 1u);
    }
}

TEST(LCHeaderTest, GapAcrossWrap)
{
    UpstreamStats tx;
    tx.air_tx = 0xFFFD;
    UpstreamStats rx;
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0, LcType::fec)));
    tx.air_tx = 2;  // 0xFFFE, 0xFFFF, 0x0000, 0x0001 lost
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0, LcType::fec)));
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
    tx.air_tx = 0xFFF0;  // 21 behind on the 16-bit ring
    ASSERT_TRUE(accept(&rx, frame(&tx, 1, nullptr, 0)));
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);
    EXPECT_EQ(rx.air_rx, 0x0005u);
}

TEST(LCHeaderTest, DropsRepeatOfLastSeq)
{
    UpstreamStats tx;
    tx.air_tx = 9;
    UpstreamStats rx;
    const auto pkt = frame(&tx, 1, nullptr, 0, LcType::fec);
    EXPECT_TRUE(accept(&rx, pkt));
    EXPECT_FALSE(accept(&rx, pkt));
    // The same sn with the other type is still the same slot.
    tx.air_tx = 9;
    EXPECT_FALSE(accept(&rx, frame(&tx, 1, nullptr, 0, LcType::plain)));
}
