#ifndef WINJECT_L3_CONTROL_TYPES_H_
#define WINJECT_L3_CONTROL_TYPES_H_

#include "Config.h"
#include "radio/RadioPhyView.h"

#include <functional>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace winject
{

// Fields to change on one upstream (update_upstream); have_* marks the
// fields present.
struct UpstreamUpdate
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

// One upstream: the add_upstream spec, and the list / OK snapshot
// (docs/mplane.md).
struct UpstreamView
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

struct UpstreamRxStatView
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
    // Queue mode: deliveries that evicted the oldest datagram from the app's
    // RX queue (the app did not drain it in time). 0 for other modes.
    uint64_t rx_push_drop = 0;
};

struct UpstreamTxStatView
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

struct RadioDeviceUpdate
{
    uint8_t id = 0;
    bool have_mplane = false;
    sockaddr_in mplane{};
    bool have_dplane = false;
    sockaddr_in dplane{};
    bool have_fcs = false;
    RadioFcsConfig fcs = RadioFcsConfig::auto_detect;
};

struct RadioDeviceView
{
    uint8_t id = 0;
    RadioDeviceConfig device;
};

struct RadioUpdate
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

struct MetricView
{
    std::string key;
    std::string value;
};

// Deferred reply for radio forwards (radio_info, radio_tx, ...). Exactly one
// of the two is called: send_text with the radio's reply body, or send_nok
// with an errno name.
struct ControlReply
{
    std::function<void(const std::string& text)> send_text;
    std::function<void(const char* nok)> send_nok;
};

// The node's operations behind every m-plane command. App binds them; they
// run on the node's reactor thread only. ConsoleCommands (text) and
// winject_l3::Control (any thread) both call through here.
struct ControlOps
{
    std::function<bool(const UpstreamView& spec, std::string* error)>
        add_upstream;
    std::function<bool(uint8_t id, std::string* error)> remove_upstream;
    std::function<bool(const std::vector<uint8_t>& ids,
                       std::vector<UpstreamView>* out, std::string* error)>
        list_upstream;
    std::function<bool(const UpstreamUpdate& patch, UpstreamView* out,
                       std::string* error)>
        update_upstream;
    std::function<bool(const std::vector<uint8_t>& ids,
                       std::vector<UpstreamRxStatView>* out,
                       std::string* error)>
        list_upstream_rx_stat;
    std::function<bool(const std::vector<uint8_t>& ids,
                       std::vector<UpstreamTxStatView>* out,
                       std::string* error)>
        list_upstream_tx_stat;
    std::function<bool(const std::vector<std::string>& keys,
                       std::vector<MetricView>* out, std::string* error)>
        get_metrics;
    std::function<bool(const RadioDeviceUpdate& patch, RadioDeviceView* out,
                       std::string* error)>
        radio_device;
    std::function<void(ControlReply reply)> radio_info;
    std::function<void(ControlReply reply)> radio_caps_info;
    std::function<void(ControlReply reply)> radio_stats;
    std::function<void(const RadioUpdate& patch, ControlReply reply)> radio_tx;
    // Optional (ENOSYS when unset): a radio m-plane command line, forwarded
    // unchanged (radio_mplane).
    std::function<void(const std::string& line, ControlReply reply)>
        radio_mplane;
    std::function<void(ControlReply reply)> radio_reset;
    std::function<void(uint8_t slot, ControlReply reply)> config_slot;
};

}  // namespace winject

#endif  // WINJECT_L3_CONTROL_TYPES_H_
