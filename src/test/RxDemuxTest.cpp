#include "endpoint/Upstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/RadioDefs.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RxDemux.h"

#include <bfc/shared_sized_buffer.hpp>
#include <gtest/gtest.h>
#include <string.h>

namespace
{
using winject::LCHeader;
using winject::Mpdu;
using winject::RadioUpstreamTable;
using winject::RxDemux;
using winject::Upstream;

bool assign_tx_sequence(Mpdu& mpdu)
{
    winject::ieee_802_11::SeqControl* seq = mpdu.ieee().seq_ctl;
    if (seq == nullptr)
    {
        return false;
    }
    seq->set_seq_num(winject::next_tx_sequence());
    seq->set_fragment_num(0);
    return true;
}

bfc::shared_sized_buffer make_slot_mpdu(uint16_t domain, uint8_t bus,
                                        const uint8_t* payload, size_t plen,
                                        bool is_fec = false, uint16_t seq = 0)
{
    const size_t slot_len = LCHeader::k_len + plen;
    const size_t total = WIFI_HDR_LEN + slot_len;
    uint8_t buf[WIFI_RADIO_INJECT_MAX] = {};
    Mpdu tx(buf, total);
    tx.set_slot_payload(0, static_cast<uint16_t>(slot_len));
    if (!tx.rescan())
    {
        return bfc::shared_sized_buffer();
    }
    uint8_t* slot = reinterpret_cast<uint8_t*>(tx.get_slot_payload(0).data());
    LCHeader::write(slot, bus, seq, is_fec);
    if (plen > 0)
    {
        memcpy(slot + LCHeader::k_len, payload, plen);
    }
    if (!assign_tx_sequence(tx))
    {
        return bfc::shared_sized_buffer();
    }
    tx.set_domain(domain);
    return bfc::shared_sized_buffer::copy_from(buf, total);
}
}  // namespace

TEST(RxDemuxTest, DropsForeignDomain)
{
    RadioUpstreamTable table;
    auto queue = std::make_shared<winject::ReactiveLossyQueue>();
    queue->set_capacity(16);
    table.add(queue, nullptr, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x1111);

    const uint8_t body[] = {0xAB};
    demux.on_mpdu(make_slot_mpdu(0x2222, 1, body, sizeof(body)));

    EXPECT_EQ(queue->size(), 0u);
    EXPECT_EQ(demux.rx_drop_domain(), 1u);
}

TEST(RxDemuxTest, DeliversMatchingDomainAndBus)
{
    RadioUpstreamTable table;
    auto queue = std::make_shared<winject::ReactiveLossyQueue>();
    queue->set_capacity(16);
    table.add(queue, nullptr, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x1234);

    const uint8_t body[] = {0xCD, 0xEF};
    demux.on_mpdu(make_slot_mpdu(0x1234, 1, body, sizeof(body)));

    EXPECT_EQ(queue->size(), 1u);
    auto packet = queue->pop();
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->size(), LCHeader::k_len + sizeof(body));
    EXPECT_EQ(memcmp(packet->u8() + LCHeader::k_len, body, sizeof(body)), 0);
    EXPECT_EQ(demux.rx_drop_domain(), 0u);
}

TEST(RxDemuxTest, DropsUnknownBus)
{
    RadioUpstreamTable table;
    auto queue = std::make_shared<winject::ReactiveLossyQueue>();
    queue->set_capacity(16);
    table.add(queue, nullptr, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x0001);

    const uint8_t body[] = {1};
    demux.on_mpdu(make_slot_mpdu(0x0001, 0x55, body, sizeof(body)));

    EXPECT_EQ(queue->size(), 0u);
    EXPECT_EQ(demux.rx_drop_bus(), 1u);
}

TEST(RxDemuxTest, DeliversLcFecFlag)
{
    RadioUpstreamTable table;
    auto queue = std::make_shared<winject::ReactiveLossyQueue>();
    queue->set_capacity(16);
    table.add(queue, nullptr, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x1234);

    const uint8_t body[] = {0xF1, 0x02, 0x03};
    demux.on_mpdu(make_slot_mpdu(0x1234, 1, body, sizeof(body), false));
    EXPECT_EQ(queue->size(), 1u);
    auto raw = queue->pop();
    ASSERT_TRUE(raw);
    EXPECT_FALSE(LCHeader::read_is_fec(raw->u8()));

    demux.on_mpdu(make_slot_mpdu(0x1234, 1, body, sizeof(body), true, 1));
    EXPECT_EQ(queue->size(), 1u);
    auto fec = queue->pop();
    ASSERT_TRUE(fec);
    EXPECT_TRUE(LCHeader::read_is_fec(fec->u8()));
}

TEST(RxDemuxTest, CountsInboxEvictions)
{
    RadioUpstreamTable table;
    auto queue = std::make_shared<winject::ReactiveLossyQueue>();
    queue->set_capacity(1);
    table.add(queue, nullptr, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x1111);

    const uint8_t first[] = {0x01};
    const uint8_t second[] = {0x02};
    demux.on_mpdu(make_slot_mpdu(0x1111, 1, first, sizeof(first)));
    demux.on_mpdu(make_slot_mpdu(0x1111, 1, second, sizeof(second), false, 1));

    ASSERT_EQ(queue->size(), 1u);
    EXPECT_EQ(table.entries()[0].rx_queue_evictions->load(), 1u);
    auto kept = queue->pop();
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->u8()[LCHeader::k_len], 0x02);
}
