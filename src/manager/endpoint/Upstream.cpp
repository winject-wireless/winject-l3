#include "endpoint/Upstream.h"

namespace winject
{

bool Upstream::stamp_air(uint8_t bus, uint16_t* tx_seq, uint8_t* out,
                         size_t max, const uint8_t* data, size_t len,
                         size_t* out_len)
{
    return stamp_air_payload(tx_seq, bus, out, max, data, len, out_len);
}

bool Upstream::accept_air(const uint8_t* data, size_t len,
                          const uint8_t** payload, size_t* plen)
{
    return accept_air_payload(&stats_, data, len, payload, plen);
}

uint64_t Upstream::collect_air_seq_loss_delta()
{
    const uint64_t now = stats_.air_rx_gap_loss;
    const uint64_t delta = now - stats_.air_rx_gap_loss_seen;
    stats_.air_rx_gap_loss_seen = now;
    return delta;
}

}  // namespace winject
