#ifndef WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
#define WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_

#include "frames/LCHeader.h"

#include <cstddef>
#include <cstdint>

namespace winject::core
{

struct UpstreamStats
{
    // Last LC sn received (16-bit ring, see UpstreamAirHeader).
    uint16_t air_rx = 0;
    bool air_rx_have = false;
    uint64_t air_rx_gap_loss = 0;
    uint64_t air_rx_gap_loss_seen = 0;
    // Backward jumps past the reorder window taken as a peer restart.
    uint64_t air_rx_resyncs = 0;
    // Slots dropped for an LC type this build does not know.
    uint64_t air_rx_lc_type_unknown = 0;
    // Next TX sn for the upstream air header (16-bit ring).
    uint16_t air_tx = 0;
};

// Writes the LC prefix (bus, type, sn) into the k_slot_prefix_len bytes at
// out, in front of a body already in place; advances air_tx.
bool stamp_slot_prefix(UpstreamStats* stats, uint8_t route_bus, LcType type,
                       uint8_t* out);
// Drops a repeat of the last sn and an unknown LC type, counts gaps, and
// returns the body and its type. An sn up to k_air_rx_reorder_window behind
// the last is late and leaves the reference alone; further behind, it is a
// restarted sender and becomes the new reference without counting loss. Any
// forward jump counts as loss: a long outage and a restart that lands ahead
// look the same.
constexpr uint16_t k_air_rx_reorder_window = 64;
bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen, LcType* type);

}  // namespace winject::core

#endif  // WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
