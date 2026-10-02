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
using winject::stamp_air_payload;
using winject::UpstreamStats;

std::vector<uint8_t> frame(uint16_t* tx_seq, uint8_t bus, const uint8_t* data,
                           size_t len)
{
    std::vector<uint8_t> out(len + LCHeader::k_len + 8);
    size_t n = 0;
    EXPECT_TRUE(
        stamp_air_payload(tx_seq, bus, out.data(), out.size(), data, len, &n));
    out.resize(n);
    return out;
}
}  // namespace

TEST(LCHeaderTest, StampThenAccept)
{
    uint16_t tx_seq = 0;
    UpstreamStats rx;
    const uint8_t payload[] = {'h', 'i'};
    const auto pkt = frame(&tx_seq, 0x42, payload, sizeof(payload));
    ASSERT_EQ(pkt.size(), LCHeader::k_len + sizeof(payload));
    EXPECT_EQ(pkt[0], 0x42);
    EXPECT_EQ(tx_seq, 1);

    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(accept_air_payload(&rx, pkt.data(), pkt.size(), &body, &plen));
    ASSERT_EQ(plen, sizeof(payload));
    EXPECT_EQ(memcmp(body, payload, plen), 0);
}

TEST(LCHeaderTest, BusRoundTrip)
{
    uint16_t tx_seq = 7;
    const auto pkt = frame(&tx_seq, 0xAB, nullptr, 0);
    EXPECT_EQ(LCHeader::read_bus(pkt.data()), 0xAB);
    EXPECT_EQ(LCHeader::read_seq(pkt.data()), 7u);
}

TEST(LCHeaderTest, DetectsGap)
{
    uint16_t tx_seq = 0;
    UpstreamStats rx;
    const uint8_t a[] = {1};
    const uint8_t c[] = {3};
    const auto p0 = frame(&tx_seq, 1, a, sizeof(a));
    tx_seq = 2;
    const auto p2 = frame(&tx_seq, 1, c, sizeof(c));

    const uint8_t* body = nullptr;
    size_t plen = 0;
    ASSERT_TRUE(accept_air_payload(&rx, p0.data(), p0.size(), &body, &plen));
    ASSERT_TRUE(accept_air_payload(&rx, p2.data(), p2.size(), &body, &plen));
    EXPECT_EQ(rx.air_rx_gap_loss, 1u);
}
