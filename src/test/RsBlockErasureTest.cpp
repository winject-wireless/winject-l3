#include "fec/RsBlockErasure.h"
#include "utils/NetUtil.h"

#include <chrono>
#include <gtest/gtest.h>
#include <string.h>
#include <thread>
#include <vector>

using namespace winject;

namespace
{
std::vector<uint8_t> pkt(size_t n, uint8_t seed)
{
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; i++)
    {
        out[i] = static_cast<uint8_t>(seed + i);
    }
    return out;
}

struct ShardInfo
{
    uint16_t sdu_base = 0;
    int idx = -1;
    int k = 0;
    int n = 0;
    int sdu_n = 0;
};

ShardInfo info(const std::vector<uint8_t>& shard)
{
    ShardInfo s;
    EXPECT_TRUE(RsBlockErasure::unpack_header(
        shard.data(), shard.size(), &s.sdu_base, &s.idx, &s.k, &s.n, &s.sdu_n));
    return s;
}

// Shard index (from each header) -> body, skipping the indices in drop.
std::unordered_map<int, std::vector<uint8_t>> frags_except(
    const std::vector<std::vector<uint8_t>>& air, const std::vector<int>& drop)
{
    std::unordered_map<int, std::vector<uint8_t>> out;
    for (const auto& p : air)
    {
        const int idx = info(p).idx;
        bool skip = false;
        for (int d : drop)
        {
            if (idx == d)
            {
                skip = true;
                break;
            }
        }
        if (skip)
        {
            continue;
        }
        out[idx] = std::vector<uint8_t>(
            p.begin() + static_cast<long>(RsBlockErasure::k_header_len),
            p.end());
    }
    return out;
}

std::vector<std::vector<uint8_t>> feed(RsBlockErasure& dec,
                                       const std::vector<uint8_t>& shard)
{
    std::vector<std::vector<uint8_t>> payloads;
    dec.push_air(shard.data(), shard.size(), &payloads);
    return payloads;
}
}  // namespace

TEST(FecTest, RoundtripNoLoss)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig;
    for (int i = 0; i < 10; i++)
    {
        orig.push_back(
            pkt(40 + static_cast<size_t>(i) * 3, static_cast<uint8_t>(i)));
    }
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 0x1234, &air));
    ASSERT_EQ(air.size(), 15u);
    for (size_t i = 0; i < air.size(); i++)
    {
        EXPECT_LE(air[i].size(), k_stream_payload_max);
        const ShardInfo s = info(air[i]);
        EXPECT_EQ(s.sdu_base, 0x1234u);
        EXPECT_EQ(s.idx, static_cast<int>(i));
        EXPECT_EQ(s.k, 10);
        EXPECT_EQ(s.n, 15);
        EXPECT_EQ(s.sdu_n, 10);
    }
    auto frags = frags_except(air, {});
    std::vector<std::vector<uint8_t>> got;
    int rec = -1;
    ASSERT_TRUE(fec.decode_block(frags, &got, &rec));
    EXPECT_EQ(rec, 0);
    EXPECT_EQ(got, orig);
}

// Byte layout shared with vstreamer's rs_block_erasure.
TEST(FecTest, HeaderWireLayout)
{
    uint8_t h[RsBlockErasure::k_header_len] = {};
    ASSERT_TRUE(RsBlockErasure::pack_header(h, 0xF1A2, 17, 16, 24, 14));
    // k=16 n=24 idx=17: 10000 11000 10001 0 = 0x8622; sdu_n=14: 01110 000.
    const uint8_t want[] = {0xF1, 0xA2, 0x86, 0x22, 0x70};
    EXPECT_EQ(memcmp(h, want, sizeof(want)), 0);

    uint16_t base = 0;
    int idx = 0;
    int k = 0;
    int n = 0;
    int sdu_n = 0;
    ASSERT_TRUE(RsBlockErasure::unpack_header(h, sizeof(h), &base, &idx, &k, &n,
                                              &sdu_n));
    EXPECT_EQ(base, 0xF1A2u);
    EXPECT_EQ(idx, 17);
    EXPECT_EQ(k, 16);
    EXPECT_EQ(n, 24);
    EXPECT_EQ(sdu_n, 14);

    ASSERT_TRUE(RsBlockErasure::pack_header(h, 0, 30, 30, 31, 1));
    ASSERT_TRUE(RsBlockErasure::unpack_header(h, sizeof(h), &base, &idx, &k, &n,
                                              &sdu_n));
    EXPECT_EQ(k, 30);
    EXPECT_EQ(n, 31);
    EXPECT_EQ(idx, 30);
}

TEST(FecTest, PackRejectsOutOfRange)
{
    uint8_t h[RsBlockErasure::k_header_len] = {};
    EXPECT_FALSE(RsBlockErasure::pack_header(h, 0, 0, 10, 32, 10));   // n > 31
    EXPECT_FALSE(RsBlockErasure::pack_header(h, 0, 0, 10, 10, 10));   // n == k
    EXPECT_FALSE(RsBlockErasure::pack_header(h, 0, 15, 10, 15, 10));  // idx
    EXPECT_FALSE(RsBlockErasure::pack_header(h, 0, 0, 10, 15, 0));    // sdu_n
    EXPECT_FALSE(RsBlockErasure::pack_header(h, 0, 0, 10, 15, 11));   // sdu_n
    RsBlockErasure fec;
    EXPECT_FALSE(fec.init(10, 32, 20));
    EXPECT_TRUE(fec.init(10, 31, 20));
}

TEST(FecTest, RecoversFiveErasures)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig;
    for (int i = 0; i < 10; i++)
    {
        orig.push_back(
            pkt(80 + static_cast<size_t>(i), static_cast<uint8_t>(0xA0 + i)));
    }
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 7, &air));

    auto drop_data = frags_except(air, {0, 1, 2, 3, 4});
    std::vector<std::vector<uint8_t>> got;
    int rec = 0;
    ASSERT_TRUE(fec.decode_block(drop_data, &got, &rec));
    EXPECT_EQ(rec, 5);
    EXPECT_EQ(got, orig);

    auto drop_parity = frags_except(air, {10, 11, 12, 13, 14});
    rec = -1;
    ASSERT_TRUE(fec.decode_block(drop_parity, &got, &rec));
    EXPECT_EQ(rec, 0);
    EXPECT_EQ(got, orig);

    auto drop_mix = frags_except(air, {1, 4, 8, 11, 14});
    ASSERT_TRUE(fec.decode_block(drop_mix, &got, &rec));
    EXPECT_EQ(got, orig);
}

TEST(FecTest, SixErasuresFail)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig(10, pkt(32, 1));
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 2, &air));
    auto frags = frags_except(air, {0, 1, 2, 3, 4, 5});
    std::vector<std::vector<uint8_t>> got;
    EXPECT_FALSE(fec.decode_block(frags, &got, nullptr));
}

// A short block sends only its sdu_n data shards and the parity; the
// receiver rebuilds the empty pads, so any sdu_n shards recover it.
TEST(FecTest, PartialBlockSkipsPads)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig = {pkt(5, 'a'), pkt(9, 'b'),
                                              pkt(3, 'c')};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 3, &air));
    ASSERT_EQ(air.size(), 3u + 5u);
    const std::vector<int> want_idx = {0, 1, 2, 10, 11, 12, 13, 14};
    for (size_t i = 0; i < air.size(); i++)
    {
        EXPECT_EQ(info(air[i]).idx, want_idx[i]);
        EXPECT_EQ(info(air[i]).sdu_n, 3);
    }

    std::vector<std::vector<uint8_t>> got;
    auto three_left = frags_except(air, {0, 1, 10, 12, 13});
    ASSERT_TRUE(fec.decode_block(10, 15, 3, three_left, &got, nullptr));
    EXPECT_EQ(got, orig);

    auto two_left = frags_except(air, {0, 1, 2, 10, 12, 13});
    EXPECT_FALSE(fec.decode_block(10, 15, 3, two_left, &got, nullptr));
}

TEST(FecTest, PartialBlockFromParityOnly)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(5, 6, 20));
    std::vector<std::vector<uint8_t>> orig = {pkt(400, 9)};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 1, &air));
    ASSERT_EQ(air.size(), 2u);
    EXPECT_EQ(air[0].size(), RsBlockErasure::k_header_len +
                                 RsBlockErasure::k_len_prefix + 400u);
    EXPECT_EQ(air[1].size(), RsBlockErasure::k_header_len +
                                 RsBlockErasure::k_len_prefix + 400u);

    auto frags = frags_except(air, {0});
    std::vector<std::vector<uint8_t>> got;
    int rec = 0;
    ASSERT_TRUE(fec.decode_block(5, 6, 1, frags, &got, &rec));
    EXPECT_EQ(rec, 1);
    EXPECT_EQ(got, orig);
}

TEST(FecTest, FeedFullBlock)
{
    RsBlockErasure enc;
    RsBlockErasure dec;  // RX needs no init; k/n come from shard headers.
    ASSERT_TRUE(enc.init(10, 15, 20));
    std::vector<std::vector<uint8_t>> orig;
    std::vector<std::vector<uint8_t>> got;
    for (int i = 0; i < 10; i++)
    {
        orig.push_back(
            pkt(20 + static_cast<size_t>(i), static_cast<uint8_t>(i)));
        std::vector<std::vector<uint8_t>> air;
        enc.push_app(orig.back().data(), orig.back().size(), &air);
        if (i < 9)
        {
            EXPECT_TRUE(air.empty());
        }
        for (const auto& p : air)
        {
            const auto payloads = feed(dec, p);
            got.insert(got.end(), payloads.begin(), payloads.end());
        }
    }
    EXPECT_EQ(got, orig);
    EXPECT_EQ(enc.blocks(), 1u);
    EXPECT_EQ(dec.blocks(), 1u);
}

// Timeout flush of a short block, two data shards lost on the way.
TEST(FecTest, FeedPartialBlockWithLoss)
{
    RsBlockErasure enc;
    RsBlockErasure dec;
    ASSERT_TRUE(enc.init(8, 12, 20));
    std::vector<std::vector<uint8_t>> orig = {pkt(30, 1), pkt(31, 2),
                                              pkt(32, 3)};
    for (const auto& o : orig)
    {
        std::vector<std::vector<uint8_t>> none;
        enc.push_app(o.data(), o.size(), &none);
        EXPECT_TRUE(none.empty());
    }
    std::vector<std::vector<uint8_t>> air;
    enc.flush(&air);
    ASSERT_EQ(air.size(), 3u + 4u);

    std::vector<std::vector<uint8_t>> got;
    for (size_t i = 0; i < air.size(); i++)
    {
        if (i == 0 || i == 2)
        {
            continue;
        }
        const auto payloads = feed(dec, air[i]);
        got.insert(got.end(), payloads.begin(), payloads.end());
    }
    EXPECT_EQ(got, orig);
    EXPECT_EQ(dec.recovered(), 2u);
    EXPECT_EQ(dec.decode_fail(), 0u);
}

// Bodies the LC header marks as FEC but that are not valid shard headers
// are dropped and counted, never passed to the application.
TEST(FecTest, InvalidShardHeaderDropped)
{
    RsBlockErasure dec;  // no init
    const std::vector<std::vector<uint8_t>> bad = {
        {0x00, 0x01},                         // too short
        {0x00, 0x00, 0x86, 0x23, 0x70, 'x'},  // spare bit in config
        {0x00, 0x00, 0x86, 0x22, 0x71, 'x'},  // spare bits in byte 4
        {0x00, 0x00, 0x06, 0x22, 0x70, 'x'},  // k = 0
        {0x00, 0x00, 0x86, 0x22, 0x00, 'x'},  // sdu_n = 0
        {0x00, 0x00, 0x86, 0x1E, 0x70, 'x'},  // idx 15 is a pad (sdu_n 14)
    };
    for (const auto& b : bad)
    {
        EXPECT_TRUE(feed(dec, b).empty());
    }
    EXPECT_EQ(dec.decode_fail(), bad.size());
}

TEST(FecTest, MaxPayloadFitsWifi)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(10, 15, 20));
    const size_t max_orig = RsBlockErasure::max_original();
    EXPECT_EQ(max_orig, k_stream_payload_max - RsBlockErasure::k_header_len -
                            RsBlockErasure::k_len_prefix);
    std::vector<std::vector<uint8_t>> orig;
    for (int i = 0; i < 10; i++)
    {
        orig.push_back(pkt(max_orig, static_cast<uint8_t>(0x33 + i)));
    }
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 4, &air));
    for (const auto& p : air)
    {
        EXPECT_LE(p.size(), k_stream_payload_max);
    }
    auto frags = frags_except(air, {0, 2, 9, 11, 13});
    std::vector<std::vector<uint8_t>> got;
    ASSERT_TRUE(fec.decode_block(frags, &got, nullptr));
    EXPECT_EQ(got, orig);
}

TEST(FecTest, SystematicNativeSize)
{
    RsBlockErasure fec;
    ASSERT_TRUE(fec.init(3, 4, 20));
    std::vector<std::vector<uint8_t>> orig = {pkt(100, 1), pkt(300, 2),
                                              pkt(200, 3)};
    std::vector<std::vector<uint8_t>> air;
    ASSERT_TRUE(fec.encode_block(orig, 1, &air));
    ASSERT_EQ(air.size(), 4u);
    EXPECT_EQ(air[0].size(), RsBlockErasure::k_header_len +
                                 RsBlockErasure::k_len_prefix + 100u);
    EXPECT_EQ(air[1].size(), RsBlockErasure::k_header_len +
                                 RsBlockErasure::k_len_prefix + 300u);
    EXPECT_EQ(air[2].size(), RsBlockErasure::k_header_len +
                                 RsBlockErasure::k_len_prefix + 200u);
    EXPECT_EQ(air[3].size(), RsBlockErasure::k_header_len +
                                 RsBlockErasure::k_len_prefix + 300u);

    auto drop_largest = frags_except(air, {1});
    std::vector<std::vector<uint8_t>> got;
    int rec = 0;
    ASSERT_TRUE(fec.decode_block(drop_largest, &got, &rec));
    EXPECT_EQ(rec, 1);
    EXPECT_EQ(got, orig);

    auto drop_small = frags_except(air, {0});
    rec = -1;
    ASSERT_TRUE(fec.decode_block(drop_small, &got, &rec));
    EXPECT_EQ(rec, 1);
    EXPECT_EQ(got, orig);
}

// sdu_base advances by sdu_n per block and wraps at 16 bits.
TEST(FecTest, SduBaseAdvancesBySduN)
{
    RsBlockErasure enc;
    enc.set_tx_sdu_seq(0xFFFE);
    ASSERT_TRUE(enc.init(4, 6, 20));
    EXPECT_EQ(enc.tx_sdu_seq(), 0xFFFEu);  // init keeps the TX sequence

    std::vector<std::vector<uint8_t>> air;
    for (int i = 0; i < 3; i++)
    {
        const auto p = pkt(10, static_cast<uint8_t>(i));
        enc.push_app(p.data(), p.size(), &air);
    }
    enc.flush(&air);
    ASSERT_FALSE(air.empty());
    EXPECT_EQ(info(air[0]).sdu_base, 0xFFFEu);
    EXPECT_EQ(info(air[0]).sdu_n, 3);
    EXPECT_EQ(enc.tx_sdu_seq(), 0x0001u);

    for (int i = 0; i < 4; i++)
    {
        const auto p = pkt(10, static_cast<uint8_t>(i));
        enc.push_app(p.data(), p.size(), &air);
    }
    ASSERT_FALSE(air.empty());
    EXPECT_EQ(info(air[0]).sdu_base, 0x0001u);
    EXPECT_EQ(enc.tx_sdu_seq(), 0x0005u);
}

TEST(FecTest, DuplicateBlockIdDroppedThenExpires)
{
    RsBlockErasure enc;
    RsBlockErasure dec;
    ASSERT_TRUE(enc.init(1, 2, 1));
    ASSERT_TRUE(dec.init(1, 2, 1));
    const std::vector<uint8_t> orig = pkt(8, 0x11);
    std::vector<std::vector<uint8_t>> air;
    enc.push_app(orig.data(), orig.size(), &air);
    ASSERT_EQ(air.size(), 2u);

    std::vector<std::vector<uint8_t>> got;
    dec.push_air(air[0].data(), air[0].size(), &got);
    ASSERT_EQ(got, std::vector<std::vector<uint8_t>>{orig});

    got.clear();
    dec.push_air(air[0].data(), air[0].size(), &got);
    dec.push_air(air[1].data(), air[1].size(), &got);
    EXPECT_TRUE(got.empty());

    std::this_thread::sleep_for(
        std::chrono::milliseconds(dec.done_hold_ms() + 50));
    dec.push_air(air[0].data(), air[0].size(), &got);
    EXPECT_EQ(got, std::vector<std::vector<uint8_t>>{orig});
}
