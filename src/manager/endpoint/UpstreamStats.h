#ifndef WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
#define WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_

#include <cstddef>
#include <cstdint>

namespace winject
{

struct UpstreamStats
{
    uint16_t air_rx = 0;
    bool air_rx_have = false;
    uint64_t air_rx_gap_loss = 0;
    uint64_t air_rx_gap_loss_seen = 0;
};

bool stamp_air_payload(uint16_t* tx_seq, uint8_t bus, uint8_t* out, size_t max,
                       const uint8_t* data, size_t len, size_t* out_len);
bool accept_air_payload(UpstreamStats* stats, const uint8_t* data, size_t len,
                        const uint8_t** payload, size_t* plen);

}  // namespace winject

#endif  // WINJECT_MANAGER_ENDPOINT_UPSTREAM_STATS_H_
