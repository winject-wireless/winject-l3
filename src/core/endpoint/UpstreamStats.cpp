#include "endpoint/UpstreamStats.h"

#include "frames/LCHeader.h"
#include "utils/NetUtil.h"


namespace winject::core
{

namespace
{

void note_air_rx_seq(UpstreamStats* stats, uint16_t seq)
{
    if (stats == nullptr)
    {
        return;
    }
    if (!stats->air_rx_have)
    {
        stats->air_rx = seq;
        stats->air_rx_have = true;
        return;
    }
    if (seq == stats->air_rx)
    {
        return;
    }
    const uint16_t expected = static_cast<uint16_t>(stats->air_rx + 1);
    const uint16_t ahead = static_cast<uint16_t>(seq - expected);
    if (ahead < UpstreamAirHeader::k_seq_half)
    {
        stats->air_rx_gap_loss += ahead;
        stats->air_rx = seq;
        return;
    }
    const uint16_t behind = static_cast<uint16_t>(stats->air_rx - seq);
    if (behind > k_air_rx_reorder_window)
    {
        ++stats->air_rx_resyncs;
        stats->air_rx = seq;
    }
}

}  // namespace

bool stamp_slot_prefix(UpstreamStats* stats, uint8_t route_bus, LcType type,
                       uint8_t* out)
{
    if (stats == nullptr || out == nullptr || route_bus == 0)
    {
        return false;
    }
    LCHeader::write(out, route_bus, type, stats->air_tx);
    stats->air_tx = static_cast<uint16_t>(stats->air_tx + 1);
    return true;
}

bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen, LcType* type)
{
    if (stats == nullptr || data == nullptr || payload == nullptr ||
        plen == nullptr || type == nullptr || len < k_slot_prefix_len)
    {
        return false;
    }
    const uint16_t seq = LCHeader::read_seq(data);
    if (stats->air_rx_have && seq == stats->air_rx)
    {
        return false;
    }
    // The sn is counted whatever the type: the slot was on the air.
    note_air_rx_seq(stats, seq);
    const uint8_t t = LCHeader::read_type(data);
    if (!UpstreamAirHeader::known_type(t))
    {
        ++stats->air_rx_lc_type_unknown;
        return false;
    }
    *type = static_cast<LcType>(t);
    *payload = data + k_slot_prefix_len;
    *plen = len - k_slot_prefix_len;
    return true;
}

}  // namespace winject::core
