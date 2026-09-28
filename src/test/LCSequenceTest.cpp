#include "endpoint/UpstreamStats.h"
#include "frames/LCSequence.h"

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace
{
using winject::LCSequence;
using winject::UpstreamStats;
using winject::accept_air_payload;
using winject::stamp_air_payload;

std::vector<uint8_t> frame(UpstreamStats* tx, const uint8_t* data, size_t len)
{
    std::vector<uint8_t> out(len + LCSequence::k_len + 8);
    size_t n = 0;
    EXPECT_TRUE(stamp_air_payload(tx, out.data(), out.size(), data, len, &n));
    out.resize(n);
    return out;
}
}  // namespace

TEST(LCSequenceTest, StampThenAccept)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t payload[] = {'h', 'i'};
    const auto pkt = frame(&tx, payload, sizeof(payload));
    ASSERT_EQ(pkt.size(), LCSequence::k_len + sizeof(payload));
    EXPECT_EQ(pkt[0], 0);
    EXPECT_EQ(pkt[1], 0);
    EXPECT_EQ(tx.air_tx, 1);

    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(accept_air_payload(&rx, pkt.data(), pkt.size(), &body, &plen));
    ASSERT_EQ(plen, sizeof(payload));
    EXPECT_EQ(memcmp(body, payload, plen), 0);
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);
}

TEST(LCSequenceTest, DetectsGap)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t a[] = {1};
    const uint8_t b[] = {2};
    const uint8_t c[] = {3};
    const auto p0 = frame(&tx, a, sizeof(a));
    const auto p1 = frame(&tx, b, sizeof(b));
    const auto p2 = frame(&tx, c, sizeof(c));
    (void)p1;

    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(accept_air_payload(&rx, p0.data(), p0.size(), &body, &plen));
    ASSERT_TRUE(accept_air_payload(&rx, p2.data(), p2.size(), &body, &plen));
    EXPECT_EQ(rx.air_rx_gap_loss, 1u);
}

TEST(LCSequenceTest, DuplicateReplayRejected)
{
    UpstreamStats tx;
    UpstreamStats rx;
    const uint8_t a[] = {9};
    const auto p0 = frame(&tx, a, sizeof(a));
    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(accept_air_payload(&rx, p0.data(), p0.size(), &body, &plen));
    EXPECT_FALSE(accept_air_payload(&rx, p0.data(), p0.size(), &body, &plen));
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);
}

TEST(LCSequenceTest, WrapAround)
{
    UpstreamStats tx;
    UpstreamStats rx;
    uint8_t one = 1;
    std::vector<uint8_t> last;
    for (int i = 0; i < 65536; i++)
    {
        last = frame(&tx, &one, 1);
    }
    EXPECT_EQ(tx.air_tx, 0);

    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(accept_air_payload(&rx, last.data(), last.size(), &body, &plen));
    const auto wrapped = frame(&tx, &one, 1);
    ASSERT_EQ(wrapped[0], 0);
    ASSERT_EQ(wrapped[1], 0);
    ASSERT_TRUE(
        accept_air_payload(&rx, wrapped.data(), wrapped.size(), &body, &plen));
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);

    const auto skip = frame(&tx, &one, 1);
    const auto next = frame(&tx, &one, 1);
    (void)skip;
    ASSERT_TRUE(accept_air_payload(&rx, next.data(), next.size(), &body, &plen));
    EXPECT_EQ(rx.air_rx_gap_loss, 1u);
}

TEST(LCSequenceTest, RejectsShort)
{
    UpstreamStats rx;
    const uint8_t one[] = {0x00};
    const uint8_t* body = nullptr;
    size_t plen = 1;
    EXPECT_FALSE(accept_air_payload(&rx, one, sizeof(one), &body, &plen));
}

TEST(LCSequenceTest, StampTooSmallDoesNotAdvance)
{
    UpstreamStats tx;
    uint8_t buf[2];
    const uint8_t payload[] = {1, 2, 3};
    size_t n = 99;
    EXPECT_FALSE(
        stamp_air_payload(&tx, buf, sizeof(buf), payload, sizeof(payload), &n));
    EXPECT_EQ(tx.air_tx, 0);
}

TEST(LCSequenceTest, FirstPacketIsNotAGap)
{
    UpstreamStats tx;
    UpstreamStats rx;
    uint8_t one = 7;
    (void)frame(&tx, &one, 1);
    (void)frame(&tx, &one, 1);
    const auto third = frame(&tx, &one, 1);
    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(
        accept_air_payload(&rx, third.data(), third.size(), &body, &plen));
    EXPECT_EQ(rx.air_rx_gap_loss, 0u);
}

TEST(LCSequenceTest, WriteReadAliasWire)
{
    uint8_t buf[LCSequence::k_len] = {0xAB, 0xCD};
    LCSequence::write(buf, 0x1234);
    EXPECT_EQ(LCSequence::read(buf), 0x1234u);
}
