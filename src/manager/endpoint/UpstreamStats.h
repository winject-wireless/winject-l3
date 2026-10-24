#ifndef WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
#define WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_

#include <cstddef>
#include <cstdint>

namespace winject
{

struct UpstreamStats
{
    // Last LC seq received (15-bit ring, see UpstreamAirHeader).
    uint16_t air_rx = 0;
    bool air_rx_have = false;
    uint64_t air_rx_gap_loss = 0;
    uint64_t air_rx_gap_loss_seen = 0;
    // Next TX seq for upstream air header (15-bit ring).
    uint16_t air_tx = 0;
};

// Builds a full slot payload: route bus + upstream air header + SDU; advances
// air_tx on success.
bool stamp_slot_payload(UpstreamStats* stats, uint8_t route_bus, bool is_fec,
                        const uint8_t* sdu, size_t sdu_len, uint8_t* out,
                        size_t out_max, size_t* out_len);
// Drops a repeat of the last seq, counts gaps, and returns the SDU and its
// is_fec flag.
bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen, bool* is_fec);

}  // namespace winject

#endif  // WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
