#include "endpoint/UpstreamStats.h"

#include "frames/LCHeader.h"

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
    const uint16_t expected =
        static_cast<uint16_t>((stats->air_rx + 1) & LCHeader::k_seq_mask);
    const uint16_t ahead =
        static_cast<uint16_t>((seq - expected) & LCHeader::k_seq_mask);
    if (ahead < LCHeader::k_seq_half)
    {
        stats->air_rx_gap_loss += ahead;
        stats->air_rx = seq;
    }
}

}  // namespace

bool stamp_air_payload(uint16_t* tx_seq, uint8_t bus, bool is_fec, uint8_t* out,
                       size_t max, const uint8_t* data, size_t len,
                       size_t* out_len)
{
    if (tx_seq == nullptr || out == nullptr || out_len == nullptr)
    {
        return false;
    }
    if (len > 0 && data == nullptr)
    {
        return false;
    }
    if (bus == 0)
    {
        return false;
    }
    if (len > max || max - len < LCHeader::k_len)
    {
        return false;
    }
    LCHeader::write(out, bus, *tx_seq, is_fec);
    if (len > 0)
    {
        memcpy(out + LCHeader::k_len, data, len);
    }
    *tx_seq = static_cast<uint16_t>((*tx_seq + 1) & LCHeader::k_seq_mask);
    *out_len = len + LCHeader::k_len;
    return true;
}

bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen, bool* is_fec)
{
    if (stats == nullptr || data == nullptr || payload == nullptr ||
        plen == nullptr || is_fec == nullptr || len < LCHeader::k_len)
    {
        return false;
    }
    const uint16_t seq = LCHeader::read_seq(data);
    if (stats->air_rx_have && seq == stats->air_rx)
    {
        return false;
    }
    note_air_rx_seq(stats, seq);
    *is_fec = LCHeader::read_is_fec(data);
    *payload = data + LCHeader::k_len;
    *plen = len - LCHeader::k_len;
    return true;
}

}  // namespace winject
