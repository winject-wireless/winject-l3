#ifndef WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
#define WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_

#include <cstddef>
#include <cstdint>

namespace winject
{

struct UpstreamStats
{
    // Last LC seq received (15-bit ring, see LCHeader).
    uint16_t air_rx = 0;
    bool air_rx_have = false;
    uint64_t air_rx_gap_loss = 0;
    uint64_t air_rx_gap_loss_seen = 0;
};

// Writes the LC header (bus, is_fec, *tx_seq) and the SDU; *tx_seq advances on
// the 15-bit ring.
bool stamp_air_payload(uint16_t* tx_seq, uint8_t bus, bool is_fec, uint8_t* out,
                       size_t max, const uint8_t* data, size_t len,
                       size_t* out_len);
// Drops a repeat of the last seq, counts gaps, and returns the SDU and its
// is_fec flag.
bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen, bool* is_fec);

}  // namespace winject

#endif  // WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
