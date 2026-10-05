#include "endpoint/Upstream.h"

namespace winject
{

bool Upstream::accept_air(const uint8_t* data, size_t len,
                          const uint8_t** payload, size_t* plen, bool* is_fec)
{
    return accept_air_payload(&stats_, data, len, payload, plen, is_fec);
}

uint64_t Upstream::collect_air_seq_loss_delta()
{
    const uint64_t now = stats_.air_rx_gap_loss;
    const uint64_t delta = now - stats_.air_rx_gap_loss_seen;
    stats_.air_rx_gap_loss_seen = now;
    return delta;
}

}  // namespace winject
