#include "frames/Mpdu.h"
#include "radio/RadioDefs.h"

#include <gtest/gtest.h>
#include <string.h>

using namespace winject;

namespace
{
class MpduTest : public ::testing::Test
{
};

void emit_bits(uint8_t* packed, size_t* bit, uint16_t value, int nbits)
{
    for (int i = 0; i < nbits; i++)
    {
        const size_t b = (*bit)++;
        if ((value & 1u) != 0)
        {
            packed[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
        }
        value = static_cast<uint16_t>(value >> 1);
    }
}

void pack_slot_sizes(uint8_t addr1[6], uint8_t addr2[6],
                     const uint16_t slot_sizes[WIFI_PDU_SLOTS])
{
    uint8_t packed[12] = {};
    size_t bit = 0;
    emit_bits(packed, &bit, 1, 1);
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        const uint16_t size = static_cast<uint16_t>(slot_sizes[i] & 0x7FFu);
        emit_bits(packed, &bit, size, 11);
    }
    memcpy(addr1, packed, 6);
    memcpy(addr2, packed + 6, 6);
}

static void assign_tx_sequence(Mpdu& mpdu)
{
    winject::ieee_802_11::SeqControl* seq = mpdu.ieee().seq_ctl;
    ASSERT_NE(seq, nullptr);
    seq->set_seq_num(next_tx_sequence());
    seq->set_fragment_num(0);
}
}  // namespace

TEST_F(MpduTest, EncodeDecodeRoundTrip)
{
    const uint8_t b0[] = {0xAA, 0xBB, 0xCC};
    const uint8_t b1[] = {0xDD, 0xEE};

    uint8_t buf[WIFI_RADIO_INJECT_MAX] = {};
    const size_t total = WIFI_HDR_LEN + 5;
    Mpdu tx(buf, total);
    tx.set_slot_payload(0, 3);
    tx.set_slot_payload(1, 2);
    ASSERT_TRUE(tx.rescan());
    memcpy(tx.get_slot_payload(0).data(), b0, 3);
    memcpy(tx.get_slot_payload(1).data(), b1, 2);
    assign_tx_sequence(tx);
    tx.set_domain(0x1234);

    Mpdu rx(buf, total);
    ASSERT_TRUE(rx.rescan());
    EXPECT_EQ(rx.slot_payload_size(0), 3u);
    EXPECT_EQ(rx.slot_payload_size(1), 2u);

    bfc::const_buffer_view body0 = rx.get_slot_payload(0);
    bfc::const_buffer_view body1 = rx.get_slot_payload(1);
    ASSERT_EQ(body0.size(), 3u);
    ASSERT_EQ(body1.size(), 2u);
    uint8_t merged[5];
    memcpy(merged, body0.data(), 3);
    memcpy(merged + 3, body1.data(), 2);
    EXPECT_EQ(memcmp(merged, "\xAA\xBB\xCC\xDD\xEE", 5), 0);
    EXPECT_TRUE(rx.is_valid_winject_frame());
    EXPECT_EQ(rx.get_domain(), 0x1234u);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(body0.data()),
              rx.ieee().frame_body);
}

TEST_F(MpduTest, SlotPayloadUsesFrameBody)
{
    const uint8_t b0[] = {0x10, 0x20};
    const uint8_t b2[] = {0x30};

    uint8_t buf[WIFI_RADIO_INJECT_MAX] = {};
    const size_t total = WIFI_HDR_LEN + 3;
    Mpdu tx(buf, total);
    tx.set_slot_payload(0, 2);
    tx.set_slot_payload(2, 1);
    ASSERT_TRUE(tx.rescan());
    memcpy(tx.get_slot_payload(0).data(), b0, 2);
    memcpy(tx.get_slot_payload(2).data(), b2, 1);
    assign_tx_sequence(tx);
    tx.set_domain(0x0001);

    Mpdu rx(buf, total);
    ASSERT_TRUE(rx.rescan());

    bfc::const_buffer_view s0 = rx.get_slot_payload(0);
    bfc::const_buffer_view s2 = rx.get_slot_payload(2);
    EXPECT_EQ(s0.size(), 2u);
    EXPECT_EQ(s2.size(), 1u);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(s0.data())[0], 0x10);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(s2.data())[0], 0x30);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(s0.data()),
              rx.ieee().frame_body);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(s2.data()),
              rx.ieee().frame_body + 2);
}

TEST_F(MpduTest, BindRejectsLengthMismatch)
{
    const uint8_t b0[] = {0x01, 0x02, 0x03};

    uint8_t buf[WIFI_RADIO_INJECT_MAX] = {};
    const size_t total = WIFI_HDR_LEN + 3;
    Mpdu tx(buf, total);
    tx.set_slot_payload(0, 3);
    ASSERT_TRUE(tx.rescan());
    memcpy(tx.get_slot_payload(0).data(), b0, 3);
    assign_tx_sequence(tx);
    tx.set_domain(0x0001);

    Mpdu rx(buf, total - 1);
    EXPECT_FALSE(rx.rescan());
}

TEST_F(MpduTest, Addr3DomainReject)
{
    const uint8_t b0[] = {0x55};

    uint8_t buf[WIFI_RADIO_INJECT_MAX] = {};
    const size_t total = WIFI_HDR_LEN + 1;
    Mpdu tx(buf, total);
    tx.set_slot_payload(0, 1);
    ASSERT_TRUE(tx.rescan());
    memcpy(tx.get_slot_payload(0).data(), b0, 1);
    assign_tx_sequence(tx);
    tx.set_domain(0x0001);

    Mpdu rx(buf, total);
    ASSERT_TRUE(rx.rescan());
    EXPECT_TRUE(rx.is_valid_winject_frame());
    EXPECT_EQ(rx.get_domain(), 0x0001u);
    EXPECT_NE(rx.get_domain(), 0x1234u);
}

TEST_F(MpduTest, InjectMaxMpduLength)
{
    uint8_t buf[WIFI_RADIO_INJECT_MAX] = {};
    Mpdu tx(buf, sizeof(buf));
    tx.set_slot_payload(0, static_cast<uint16_t>(WIFI_PAYLOAD_MAX));
    ASSERT_TRUE(tx.rescan());

    uint8_t over[WIFI_RADIO_INJECT_MAX + 1] = {};
    Mpdu too_large(over, sizeof(over));
    too_large.set_slot_payload(0, static_cast<uint16_t>(WIFI_PAYLOAD_MAX + 1));
    EXPECT_FALSE(too_large.rescan());
}

TEST_F(MpduTest, RescanAcceptsRadioRxMaxMpdu)
{
    uint8_t buf[WIFI_RADIO_RX_MAX] = {};
    Mpdu build(buf, WIFI_RADIO_INJECT_MAX);
    build.set_slot_payload(0, static_cast<uint16_t>(WIFI_PAYLOAD_MAX));
    ASSERT_TRUE(build.rescan());
    assign_tx_sequence(build);
    build.set_domain(0x00BE);

    uint16_t sizes[WIFI_PDU_SLOTS] = {};
    sizes[0] = static_cast<uint16_t>(WIFI_RX_PAYLOAD_MAX);
    pack_slot_sizes(buf + 4, buf + 10, sizes);
    memset(buf + WIFI_RADIO_INJECT_MAX, 0,
           WIFI_RADIO_RX_MAX - WIFI_RADIO_INJECT_MAX);

    Mpdu rx(buf, WIFI_RADIO_RX_MAX);
    ASSERT_TRUE(rx.rescan());
    EXPECT_EQ(rx.slot_payload_size(0),
              static_cast<uint16_t>(WIFI_RX_PAYLOAD_MAX));
}

TEST_F(MpduTest, BuildWithAddr3RoundTrip)
{
    const uint8_t b0[] = {0x01, 0x02};

    uint8_t out[WIFI_RADIO_INJECT_MAX] = {};
    const size_t total = WIFI_HDR_LEN + 2;
    Mpdu tx(out, total);
    tx.set_slot_payload(0, 2);
    ASSERT_TRUE(tx.rescan());
    memcpy(tx.get_slot_payload(0).data(), b0, 2);
    assign_tx_sequence(tx);
    tx.set_domain(0x00BE);

    Mpdu rx(out, total);
    ASSERT_TRUE(rx.rescan());
    EXPECT_EQ(rx.slot_payload_size(0), 2u);
}
