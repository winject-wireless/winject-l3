#include "endpoint/UpstreamStats.h"

#include "frames/LCHeader.h"
#include "utils/NetUtil.h"

#include <cstring>

namespace winject
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
    const uint16_t expected = static_cast<uint16_t>(
        (stats->air_rx + 1) & UpstreamAirHeader::k_seq_mask);
    const uint16_t ahead =
        static_cast<uint16_t>((seq - expected) & UpstreamAirHeader::k_seq_mask);
    if (ahead < UpstreamAirHeader::k_seq_half)
    {
        stats->air_rx_gap_loss += ahead;
        stats->air_rx = seq;
        return;
    }
    const uint16_t behind = static_cast<uint16_t>((stats->air_rx - seq) &
                                                  UpstreamAirHeader::k_seq_mask);
    if (behind > k_air_rx_reorder_window)
    {
        ++stats->air_rx_resyncs;
        stats->air_rx = seq;
    }
}

}  // namespace

bool stamp_slot_payload(UpstreamStats* stats, uint8_t route_bus, bool is_fec,
                        const uint8_t* sdu, size_t sdu_len, uint8_t* out,
                        size_t out_max, size_t* out_len)
{
    if (stats == nullptr || out == nullptr || out_len == nullptr)
    {
        return false;
    }
    if (sdu_len > 0 && sdu == nullptr)
    {
        return false;
    }
    if (route_bus == 0)
    {
        return false;
    }
    if (sdu_len > k_stream_payload_max ||
        out_max < k_slot_prefix_len + sdu_len)
    {
        return false;
    }
    RouteHeader::write(out, route_bus);
    UpstreamAirHeader::write(out + RouteHeader::k_len, stats->air_tx, is_fec);
    if (sdu_len > 0)
    {
        memcpy(out + k_slot_prefix_len, sdu, sdu_len);
    }
    stats->air_tx = static_cast<uint16_t>(
        (stats->air_tx + 1) & UpstreamAirHeader::k_seq_mask);
    *out_len = k_slot_prefix_len + sdu_len;
    return true;
}

bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen, bool* is_fec)
{
    if (stats == nullptr || data == nullptr || payload == nullptr ||
        plen == nullptr || is_fec == nullptr || len < k_slot_prefix_len)
    {
        return false;
    }
    const uint16_t seq = UpstreamAirHeader::read_seq(data);
    if (stats->air_rx_have && seq == stats->air_rx)
    {
        return false;
    }
    note_air_rx_seq(stats, seq);
    *is_fec = UpstreamAirHeader::read_is_fec(data);
    *payload = data + k_slot_prefix_len;
    *plen = len - k_slot_prefix_len;
    return true;
}

}  // namespace winject
