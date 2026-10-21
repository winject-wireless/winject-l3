#ifndef WINJECT_MANAGER_CONFIG_H_
#define WINJECT_MANAGER_CONFIG_H_

#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace winject
{

enum class UpstreamMode
{
    udp_static,
    udp_client,
    udp_server,
    // In-process datagrams via named lossy queues. See QueueEndpoint.
    queue,
};

// UDP peer endpoints: both set = static; rx only = server bind; tx only =
// client connect.
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

// radio.fcs config (runtime mode is RadioFcsMode on WifiUdp).
enum class RadioFcsConfig
{
    auto_detect,
    signal,
    actual,
};

struct UpstreamConfig
{
    size_t index = 0;
    UpstreamMode mode = UpstreamMode::udp_static;
    UdpPeerEndpoint endpoint;
    // QUEUE_FORWARDING: registry names. queue_in is consumed (app toward
    // air); queue_out is produced (air toward app). reactor is the host's
    // running cv_reactor. Ignored for UDP modes.
    std::string queue_in;
    std::string queue_out;
    std::string reactor;
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    size_t scheduler_budget = 256;
    FecType fec_type = FecType::none;
    int fec_k = 0;
    int fec_n = 0;
    int fec_timeout_ms = 20;
};

// One radio the manager drives (docs/mplane.md § radio_device). Both
// addresses are required. A radio_device command can change either one
// and cannot clear them.
struct RadioDeviceConfig
{
    sockaddr_in mplane{};  // radio m-plane (ESP32 :22, Realtek :2201)
    sockaddr_in dplane{};  // radio d-plane: inject, registration, forward
    RadioFcsConfig fcs = RadioFcsConfig::auto_detect;
};

struct Config
{
    RadioDeviceConfig radio_device;
    uint8_t channel = 1;
    std::string modulation = "DSS_1M_L";
    int8_t power_dbm = 20;
    // Radio CCA, sent with the PHY when cca_explicit (radio.cca present).
    bool cca = false;
    bool cca_explicit = false;
    // Shared air domain (Addr3); 0 = unset / invalid.
    uint16_t domain = 0;
    // Radio config slot for save/load (0 = scratch pad until config slot=).
    uint8_t config_slot = 0;
    // Local UDP management console. Empty = disabled.
    // console_in = bind address for m-plane commands; replies go to the sender.
    std::string manager_console_in;
    std::vector<UpstreamConfig> upstreams;

    bool load(const std::string& path, std::string* error);
    // Checks a Config already in memory (a parsed file or one filled in by
    // the host). load() runs this after parsing.
    bool validate(std::string* error) const;

    static bool validate_upstreams(const std::vector<UpstreamConfig>& upstreams,
                                   std::string* error);

    // PHY air rate (kbps) for a modulation name, or 0 if unknown.
    static uint32_t phy_rate_kbps(const std::string& modulation);
    static bool validate_upstream_update(
        const UpstreamConfig& current, FecType fec_type, int fec_k, int fec_n,
        int fec_timeout_ms, size_t quanta, bool have_fec, bool have_k,
        bool have_n, bool have_fec_timeout, bool have_quanta,
        const std::vector<UpstreamConfig>& all, UpstreamConfig* out,
        std::string* error);
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
