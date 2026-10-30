#ifndef WINJECT_MANAGER_CONFIG_H_
#define WINJECT_MANAGER_CONFIG_H_

#include "endpoint/UpstreamConfig.h"
#include "radio/RadioConfig.h"

#include <functional>
#include <memory>
#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace winject
{

class UpstreamQueue;

// The l3 node's configuration: the radio (RadioConfig, core) plus upstreams
// and the l3 console. Parsed from a file (load) or filled in by the host.
struct Config : RadioConfig
{
    // winject.console_in. Empty disables the manager m-plane.
    // Bind address for commands; replies go to the sender.
    std::string console_in;
    std::vector<UpstreamConfig> upstreams;
    // Empty queue endpoint names: L3 creates the app transport and publishes
    // it.
    std::function<void(size_t, std::shared_ptr<UpstreamQueue>)>
        on_upstream_queue;

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
