#include "endpoint/Upstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/RadioDefs.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RxDemux.h"

#include <bfcext/shared_sized_buffer.hpp>
#include <gtest/gtest.h>
#include <string.h>

namespace
{
using winject::LCHeader;
using winject::Mpdu;
using winject::RadioUpstreamTable;
using winject::RxDemux;
using winject::Upstream;

class CaptureUpstream : public Upstream
{
public:
    size_t rx_count = 0;
    size_t last_payload_len = 0;
    bool last_is_fec = false;

    void on_radio_rx(bfcext::shared_sized_buffer pkt, bool is_fec) override
    {
        ++rx_count;
        last_payload_len = pkt.size();
        last_is_fec = is_fec;
    }

    bool has_tx() override
    {
        return false;
    }

    size_t get_tx_size() override
    {
        return 0;
    }

    bfc::sized_buffer pull_tx(size_t /*max*/, bool* /*is_fec*/) override
    {
        return bfc::sized_buffer();
    }
};

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

bfcext::shared_sized_buffer make_slot_mpdu(uint16_t domain, uint8_t bus,
                                           const uint8_t* payload, size_t plen,
                                           bool is_fec = false,
                                           uint16_t seq = 0)
{
    const size_t slot_len = LCHeader::k_len + plen;
    const size_t total = WIFI_HDR_LEN + slot_len;
    uint8_t buf[WIFI_RADIO_INJECT_MAX] = {};
    Mpdu tx(buf, total);
    tx.set_slot_payload(0, static_cast<uint16_t>(slot_len));
    if (!tx.rescan())
    {
        return bfcext::shared_sized_buffer();
    }
    uint8_t* slot = reinterpret_cast<uint8_t*>(tx.get_slot_payload(0).data());
    LCHeader::write(slot, bus, seq, is_fec);
    if (plen > 0)
    {
        memcpy(slot + LCHeader::k_len, payload, plen);
    }
    if (!assign_tx_sequence(tx))
    {
        return bfcext::shared_sized_buffer();
    }
    tx.set_domain(domain);
    return bfcext::shared_sized_buffer::copy_from(buf, total);
}
}  // namespace

TEST(RxDemuxTest, DropsForeignDomain)
{
    RadioUpstreamTable table;
    auto up = std::make_shared<CaptureUpstream>();
    table.add(up, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x1111);

    const uint8_t body[] = {0xAB};
    demux.on_mpdu(make_slot_mpdu(0x2222, 1, body, sizeof(body)));

    EXPECT_EQ(up->rx_count, 0u);
    EXPECT_EQ(demux.rx_drop_domain(), 1u);
}

TEST(RxDemuxTest, DeliversMatchingDomainAndBus)
{
    RadioUpstreamTable table;
    auto up = std::make_shared<CaptureUpstream>();
    table.add(up, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x1234);

    const uint8_t body[] = {0xCD, 0xEF};
    demux.on_mpdu(make_slot_mpdu(0x1234, 1, body, sizeof(body)));

    EXPECT_EQ(up->rx_count, 1u);
    EXPECT_EQ(up->last_payload_len, sizeof(body));
    EXPECT_EQ(demux.rx_drop_domain(), 0u);
}

TEST(RxDemuxTest, DropsUnknownBus)
{
    RadioUpstreamTable table;
    auto up = std::make_shared<CaptureUpstream>();
    table.add(up, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x0001);

    const uint8_t body[] = {1};
    demux.on_mpdu(make_slot_mpdu(0x0001, 0x55, body, sizeof(body)));

    EXPECT_EQ(up->rx_count, 0u);
    EXPECT_EQ(demux.rx_drop_bus(), 1u);
}

TEST(RxDemuxTest, DeliversLcFecFlag)
{
    RadioUpstreamTable table;
    auto up = std::make_shared<CaptureUpstream>();
    table.add(up, nullptr, 1, 1, 1);
    RxDemux demux(table);
    demux.set_domain(0x1234);

    const uint8_t body[] = {0xF1, 0x02, 0x03};
    demux.on_mpdu(make_slot_mpdu(0x1234, 1, body, sizeof(body), false));
    EXPECT_EQ(up->rx_count, 1u);
    EXPECT_FALSE(up->last_is_fec);

    demux.on_mpdu(make_slot_mpdu(0x1234, 1, body, sizeof(body), true, 1));
    EXPECT_EQ(up->rx_count, 2u);
    EXPECT_TRUE(up->last_is_fec);
}
