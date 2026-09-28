#include "endpoint/UpstreamStats.h"

#include "frames/LCSequence.h"

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
    const uint16_t expected = static_cast<uint16_t>(stats->air_rx + 1);
    const uint16_t ahead = static_cast<uint16_t>(seq - expected);
    if (ahead < 0x8000)
    {
        stats->air_rx_gap_loss += ahead;
        stats->air_rx = seq;
    }
}

}  // namespace

bool stamp_air_payload(UpstreamStats* stats, uint8_t* out, size_t max,
                       const uint8_t* data, size_t len, size_t* out_len)
{
    if (stats == nullptr || out == nullptr || out_len == nullptr)
    {
        return false;
    }
    if (len > 0 && data == nullptr)
    {
        return false;
    }
    if (len > max || max - len < LCSequence::k_len)
    {
        return false;
    }
    LCSequence::write(out, stats->air_tx);
    if (len > 0)
    {
        memcpy(out + LCSequence::k_len, data, len);
    }
    stats->air_tx = static_cast<uint16_t>(stats->air_tx + 1);
    *out_len = len + LCSequence::k_len;
    return true;
}

bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen)
{
    if (stats == nullptr || data == nullptr || payload == nullptr ||
        plen == nullptr || len < LCSequence::k_len)
    {
        return false;
    }
    const uint16_t seq = LCSequence::read(data);
    if (stats->air_rx_have && seq == stats->air_rx)
    {
        return false;
    }
    note_air_rx_seq(stats, seq);
    *payload = data + LCSequence::k_len;
    *plen = len - LCSequence::k_len;
    return true;
}

}  // namespace winject
