#ifndef WINJECT_MANAGER_CONSOLE_MANAGER_CONSOLE_TYPES_H_
#define WINJECT_MANAGER_CONSOLE_MANAGER_CONSOLE_TYPES_H_

#include "Config.h"

#include <functional>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace winject
{

// Snapshot of one upstream for list / OK responses (mplane.md).
struct ManagerUpstreamUpdate
{
    uint8_t id = 0;
    bool have_fec = false;
    FecType fec = FecType::none;
    bool have_k = false;
    int fec_k = 0;
    bool have_n = false;
    int fec_n = 0;
    bool have_fec_timeout = false;
    int fec_timeout_ms = 0;
    bool have_quanta = false;
    size_t quanta = 0;
};

struct ManagerUpstreamView
{
    uint8_t id = 0;
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    std::string type = "UDP";
    std::string rx;
    std::string tx;
    FecType fec = FecType::none;
    int fec_k = 0;
    int fec_n = 0;
    int fec_timeout_ms = 0;
    size_t quanta = 0;
};

struct ManagerUpstreamRxStatView
{
    uint8_t id = 0;
    FecType fec = FecType::none;
    int fec_k = 0;
    int fec_n = 0;
    uint64_t rxbyt = 0;
    uint64_t rxpkt = 0;
    uint64_t rx_oversize = 0;
    uint64_t rxgap = 0;
    uint64_t fec_rec = 0;
    uint64_t fec_lost = 0;
    uint64_t fec_rxbyt = 0;
    uint64_t fec_rxpkt = 0;
    uint64_t fec_rxgap = 0;
};

struct ManagerUpstreamTxStatView
{
    uint8_t id = 0;
    FecType fec = FecType::none;
    int fec_k = 0;
    int fec_n = 0;
    uint64_t txbyt = 0;
    uint64_t txpkt = 0;
    uint64_t fec_txbyt = 0;
    uint64_t fec_txpkt = 0;
    uint64_t tx_pending_byt = 0;
    uint64_t tx_pending_pkt = 0;
};

struct ManagerRadioView
{
    uint16_t channel = 0;
    int tx_power = 0;
    std::string modulation;
    int8_t rssi = 0;
    bool rssi_valid = false;
    bool cca = false;
    bool cca_valid = false;
};

struct ManagerRadioDeviceUpdate
{
    uint8_t id = 0;
    bool have_mplane = false;
    sockaddr_in mplane{};
    bool have_dplane = false;
    sockaddr_in dplane{};
    bool have_fcs = false;
    RadioFcsConfig fcs = RadioFcsConfig::auto_detect;
};

struct ManagerRadioDeviceView
{
    uint8_t id = 0;
    RadioDeviceConfig device;
};

struct ManagerRadioUpdate
{
    bool have_channel = false;
    uint16_t channel = 0;
    bool have_tx_power = false;
    int tx_power = 0;
    bool have_modulation = false;
    std::string modulation;
    bool have_cca = false;
    bool cca = false;
};

struct ManagerMetricView
{
    std::string key;
    std::string value;
};

// Deferred reply for async radio forwards (manager console ri/rt/r).
struct ManagerConsoleReply
{
    std::function<void(const std::string& text)> send_text;
    std::function<void(const char* nok)> send_nok;
};

struct ManagerConsoleHandlers
{
    std::function<bool(const ManagerUpstreamView& spec, std::string* error)>
        add_upstream;
    std::function<bool(uint8_t id, std::string* error)> remove_upstream;
    std::function<bool(const std::vector<uint8_t>& ids,
                       std::vector<ManagerUpstreamView>* out,
                       std::string* error)>
        list_upstream;
    std::function<bool(const ManagerUpstreamUpdate& patch,
                       ManagerUpstreamView* out, std::string* error)>
        update_upstream;
    std::function<bool(const std::vector<uint8_t>& ids,
                       std::vector<ManagerUpstreamRxStatView>* out,
                       std::string* error)>
        list_upstream_rx_stat;
    std::function<bool(const std::vector<uint8_t>& ids,
                       std::vector<ManagerUpstreamTxStatView>* out,
                       std::string* error)>
        list_upstream_tx_stat;
    std::function<bool(const std::vector<std::string>& keys,
                       std::vector<ManagerMetricView>* out, std::string* error)>
        get_metrics;
    std::function<bool(const ManagerRadioDeviceUpdate& patch,
                       ManagerRadioDeviceView* out, std::string* error)>
        radio_device;
    std::function<void(ManagerConsoleReply reply)> radio_info;
    std::function<void(ManagerConsoleReply reply)> radio_caps_info;
    std::function<void(ManagerConsoleReply reply)> radio_stats;
    std::function<void(const ManagerRadioUpdate& patch,
                       ManagerConsoleReply reply)>
        radio_tx;
    std::function<void(ManagerConsoleReply reply)> radio_reset;
    std::function<void(uint8_t slot, ManagerConsoleReply reply)> config_slot;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_MANAGER_CONSOLE_TYPES_H_
