#include <gtest/gtest.h>
#include <string.h>

#include "frames/Mpdu.h"

using namespace winject;

namespace
{
class MpduSlotTest : public ::testing::Test
{
};

static void expect_mac6(const uint8_t got[6], const uint8_t want[6])
{
    EXPECT_EQ(memcmp(got, want, 6), 0);
}

static void assign_tx_sequence(Mpdu& mpdu)
{
    winject::ieee_802_11::SeqControl* seq = mpdu.ieee().seq_ctl;
    ASSERT_NE(seq, nullptr);
    seq->set_seq_num(next_tx_sequence());
    seq->set_fragment_num(0);
}
}  // namespace

TEST_F(MpduSlotTest, PackOnePduGolden)
{
    uint8_t buf[WIFI_HDR_LEN + 3] = {};
    Mpdu mpdu(buf, sizeof(buf));
    mpdu.set_slot_payload(0, 3);
    const uint8_t want1[6] = {0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t want2[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    expect_mac6(buf + 4, want1);
    expect_mac6(buf + 10, want2);
    EXPECT_EQ(buf[4] & 0x01, 0x01);  // 802.11 I/G group

    ASSERT_TRUE(mpdu.rescan());
    EXPECT_EQ(mpdu.slot_payload_size(0), 3u);
    for (int i = 1; i < WIFI_PDU_SLOTS; i++)
    {
        EXPECT_EQ(mpdu.slot_payload_size(static_cast<uint8_t>(i)), 0u);
    }
}

TEST_F(MpduSlotTest, PackTwoPduGolden)
{
    uint8_t buf[WIFI_HDR_LEN + 5] = {};
    Mpdu mpdu(buf, sizeof(buf));
    mpdu.set_slot_payload(0, 2);
    mpdu.set_slot_payload(1, 3);
    const uint8_t want1[6] = {0x05, 0x30, 0x00, 0x00, 0x00, 0x00};
    const uint8_t want2[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    expect_mac6(buf + 4, want1);
    expect_mac6(buf + 10, want2);
    EXPECT_EQ(buf[4] & 0x01, 0x01);

    ASSERT_TRUE(mpdu.rescan());
    EXPECT_EQ(mpdu.slot_payload_size(0), 2u);
    EXPECT_EQ(mpdu.slot_payload_size(1), 3u);
    EXPECT_EQ(mpdu.slot_payload_size(2), 0u);
}

TEST_F(MpduSlotTest, StampAndAddr3)
{
    const uint8_t body[] = {0x01, 0x02, 0x03};
    uint8_t buf[WIFI_HDR_LEN + 3] = {};
    Mpdu mpdu(buf, sizeof(buf));
    mpdu.set_slot_payload(0, 3);
    ASSERT_TRUE(mpdu.rescan());
    memcpy(mpdu.get_slot_payload(0).data(), body, 3);
    assign_tx_sequence(mpdu);
    mpdu.set_domain(0x1234);

    EXPECT_EQ(buf[0], 0x08);
    EXPECT_EQ(buf[1], 0x00);
    const uint8_t want1[6] = {0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t want3[6] = {0xCA, 0xFE, 0xBA, 0xBE, 0x12, 0x34};
    expect_mac6(buf + 4, want1);
    expect_mac6(buf + 16, want3);

    Mpdu rx(buf, sizeof(buf));
    ASSERT_TRUE(rx.rescan());
    EXPECT_TRUE(rx.is_valid_winject_frame());
    EXPECT_EQ(rx.get_domain(), 0x1234u);
    EXPECT_NE(rx.get_domain(), 0x0001u);

    uint8_t hdr[WIFI_HDR_LEN] = {};
    Mpdu domain_only(hdr, sizeof(hdr));
    domain_only.set_domain(0x0001);
    EXPECT_EQ(domain_only.get_domain(), 0x0001u);
    const uint8_t want_sa[6] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
    expect_mac6(hdr + 16, want_sa);
}

TEST_F(MpduSlotTest, Size11RoundTrip)
{
    uint8_t buf[WIFI_HDR_LEN + 100] = {};
    Mpdu mpdu(buf, sizeof(buf));
    mpdu.set_slot_payload(0, 0xFFFF);
    EXPECT_EQ(mpdu.slot_payload_size(0), 0x7FFu);

    uint8_t buf2[WIFI_HDR_LEN + 1] = {};
    Mpdu mpdu2(buf2, sizeof(buf2));
    mpdu2.set_slot_payload(4, 1);
    ASSERT_TRUE(mpdu2.rescan());
    EXPECT_EQ(mpdu2.slot_payload_size(4), 1u);
    EXPECT_EQ(mpdu2.slot_payload_size(1), 0u);
}

TEST_F(MpduSlotTest, FullMpduBodyRoundTrip)
{
    const uint8_t b0[] = {0xAA, 0xBB, 0xCC};
    const uint8_t b1[] = {0xDD, 0xEE};
    uint8_t mpdu_buf[WIFI_HDR_LEN + 5] = {};
    Mpdu tx(mpdu_buf, sizeof(mpdu_buf));
    tx.set_slot_payload(0, 3);
    tx.set_slot_payload(1, 2);
    ASSERT_TRUE(tx.rescan());
    memcpy(tx.get_slot_payload(0).data(), b0, 3);
    memcpy(tx.get_slot_payload(1).data(), b1, 2);
    assign_tx_sequence(tx);
    tx.set_domain(0x1234);

    EXPECT_EQ(memcmp(mpdu_buf + WIFI_HDR_LEN, "\xAA\xBB\xCC\xDD\xEE", 5), 0);

    Mpdu rx(mpdu_buf, sizeof(mpdu_buf));
    ASSERT_TRUE(rx.rescan());
    EXPECT_EQ(rx.slot_payload_size(0), 3u);
    EXPECT_EQ(rx.slot_payload_size(1), 2u);
    EXPECT_TRUE(rx.is_valid_winject_frame());
    EXPECT_EQ(rx.get_domain(), 0x1234u);
}
