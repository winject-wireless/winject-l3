#ifndef WINJECT_MANAGER_CONFIG_H_
#define WINJECT_MANAGER_CONFIG_H_

#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace winject
{

enum class UpstreamMode
{
    udp_static,
    udp_client,
    udp_server,
};

// UDP peer endpoints: both set = static; rx only = server bind; tx only = client
// connect.
struct UdpPeerEndpoint
{
    std::string rx;
    std::string tx;
};

inline UpstreamMode upstream_mode(const UdpPeerEndpoint& endpoint)
{
    const bool has_rx = !endpoint.rx.empty();
    const bool has_tx = !endpoint.tx.empty();
    if (has_rx && has_tx)
    {
        return UpstreamMode::udp_static;
    }
    if (has_rx)
    {
        return UpstreamMode::udp_server;
    }
    if (has_tx)
    {
        return UpstreamMode::udp_client;
    }
    return UpstreamMode::udp_static;
}

enum class FecType
{
    none,
    RsBlockErasure,
};

struct UpstreamConfig
{
    size_t index = 0;
    UdpPeerEndpoint endpoint;
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    size_t scheduler_budget = 256;
    FecType fec_type = FecType::none;
    int fec_k = 0;
    int fec_n = 0;
    int fec_timeout_ms = 20;
};

// Matches firmware WIFI_RADIO_TX_QUEUE (manager has no shared header).
static constexpr size_t k_radio_tx_queue_depth = 20;
// Burst gate: optional micro-batch cap per scheduler pass. interval_us=0
// disables the post-burst cooldown (token bucket + max_data_per_tick remain).
static constexpr size_t k_default_tx_burst_size = k_radio_tx_queue_depth;
static constexpr uint32_t k_default_tx_burst_interval_us = 0;

struct Config
{
    std::string device;
    uint16_t console_port = 22;
    uint8_t channel = 1;
    std::string modulation = "DSS_1M_L";
    int8_t power_dbm = 20;
    // Shared air domain (Addr3); 0 = unset / invalid.
    uint16_t domain = 0;
    uint32_t max_rate_kbps = 10000;
    // Max DATA MPDUs emitted per 250 us scheduler tick (1–32).
    size_t max_data_per_tick = 4;
    std::string local_ip;
    uint16_t inject_port = 9000;
    uint16_t forward_port = 9210;
    uint16_t forward_base = 9210;  // legacy alias for forward_port
    bool skip_console = false;
    // DATA MPDUs per inject burst (~half radio tx queue); gap before next
    // burst.
    size_t tx_burst_size = k_default_tx_burst_size;
    uint32_t tx_burst_interval_us = k_default_tx_burst_interval_us;
    // Local UDP management console. Empty = disabled. Both required together.
    // console_in = bind (recv commands); console_out = dest (send replies).
    std::string manager_console_in;
    std::string manager_console_out;
    std::vector<UpstreamConfig> upstreams;

    bool load(const std::string& path, std::string* error);

    // PHY air rate (kbps) for a modulation name, or 0 if unknown.
    static uint32_t phy_rate_kbps(const std::string& modulation);
    static uint32_t derive_max_rate_kbps(const std::string& modulation);
    // Canonical firmware name (e.g. OFDM_24M), or empty if unknown.
    static std::string canonical_modulation(const std::string& modulation);
    // Channel 14 is DSSS/CCK only. Unknown names are not ok.
    static bool modulation_ok_for_channel(const std::string& modulation,
                                          uint8_t channel);

    // Periodic stats to stderr (0 = disabled). Also WINJECT_STATS_SEC env.
    unsigned stats_sec = 0;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_CONFIG_H_
