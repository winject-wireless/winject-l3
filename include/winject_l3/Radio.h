#ifndef WINJECT_L3_PUBLIC_RADIO_H_
#define WINJECT_L3_PUBLIC_RADIO_H_

#include "winject_l3/Types.h"

#include <memory>
#include <netinet/in.h>
#include <optional>
#include <stdint.h>
#include <string>
#include <sys/socket.h>

namespace winject_l3
{

enum class RadioFcs
{
    auto_detect,
    signal,
    actual,
};

// Where the radio is. The m-plane is UDP. The d-plane is UDP (dplane) or
// in-process queues (dplane_tx, dplane_rx and reactor, all three set).
struct RadioDevice
{
    sockaddr_in mplane{};
    sockaddr_in dplane{};
    std::string dplane_tx;
    std::string dplane_rx;
    std::string reactor;
    RadioFcs fcs = RadioFcs::auto_detect;
};

struct RadioPhy
{
    uint8_t channel = 1;
    std::string modulation = "DSS_1M_L";
    int8_t power_dbm = 20;
    std::optional<bool> cca;  // unset: radio default
};

struct RadioPhyPatch
{
    std::optional<uint8_t> channel;
    std::optional<std::string> modulation;
    std::optional<int8_t> power_dbm;
    std::optional<bool> cca;
};

// The node's one radio.
struct RadioSpec
{
    RadioDevice device;
    RadioPhy phy;
    uint8_t config_slot = 0;
    uint16_t domain = 0;  // air domain (Addr3), non-zero; fixed at create
};

// stopped: created, not added. connecting: added, m-plane not up or the
// radio not programmed yet. up: connected and programmed.
// incompatible: the radio's protocol X.Y differs (radio calls: EPROTO).
enum class RadioState
{
    stopped,
    connecting,
    up,
    incompatible,
};

struct RadioInfo  // radio_info, and the radio_tx reply
{
    RadioPhy phy;  // what the radio runs now
    std::optional<int8_t> rssi;
    std::optional<Version> version;  // the radio's, cached at connect
    std::string text;                // the radio's reply body, verbatim
};

struct RadioCaps
{
    RadioFcs fcs = RadioFcs::auto_detect;  // signal or actual
    std::string text;                      // radio_caps_info fcs=...
};

struct RadioTxStats  // tx_info
{
    uint64_t queue_size = 0;
    uint64_t in_flight = 0;
    uint64_t dropped_invalid_frame = 0;
    uint64_t dropped_queue = 0;
    uint64_t dropped_wifi = 0;
    uint64_t ether_packets = 0;
    uint64_t air_packets = 0;
    uint64_t timestamp_us = 0;
};

struct RadioRxStats  // rx_info
{
    uint64_t queue_size = 0;
    uint64_t dropped_filter_mismatched = 0;
    uint64_t dropped_queue = 0;
    uint64_t dropped_no_peer = 0;
    uint64_t dropped_send_failed = 0;
    uint64_t ether_packets = 0;
    uint64_t air_packets = 0;
    uint64_t timestamp_us = 0;
};

// A reply that does not parse: EPROTO.
struct RadioStats
{
    RadioTxStats tx;
    RadioRxStats rx;
};

// A handle to the node's radio. Cheap to copy, safe from any thread. After
// remove its calls return ENOENT; once a radio is created again, the handle
// reaches the new one (as an Upstream handle does its id). A default handle
// is attached to nothing: calls return ESRCH.
class Radio
{
public:
    Radio() = default;

    bool valid() const;  // still created?

    Result<RadioState> state() const;
    Result<RadioSpec> spec() const;   // desired config
    Result<uint16_t> domain() const;  // fixed at create: remove + create

    Result<RadioDevice> device() const;
    // Takes the whole struct: read, patch, write. Reconnects the m-plane if
    // it changed, re-applies the d-plane if it changed. EINVAL: clearing an
    // address, or changing a queue d-plane.
    Status set_device(const RadioDevice& d);

    // Desired PHY, re-applied on reconnect.
    //  stopped: stored only, nothing on air.
    //  up: radio_tx, then saved to the config slot; stored when the radio
    //  says OK.
    //  connecting: ENODEV, incompatible: EPROTO; nothing stored.
    //  Any state: unknown modulation, or one the channel does not allow:
    //  EINVAL.
    Result<RadioPhy> phy() const;
    void set_phy(const RadioPhyPatch& p, Done<RadioInfo> done);
    Result<RadioInfo> set_phy(const RadioPhyPatch& p,
                              std::chrono::milliseconds t = k_default_timeout);
    Result<RadioInfo> set_channel(uint8_t ch);  // set_phy shorthands
    Result<RadioInfo> set_modulation(const std::string& m);
    Result<RadioInfo> set_power(int8_t dbm);
    Result<RadioInfo> set_cca(bool on);

    // Over-the-air queries: callback and blocking forms.
    void info(Done<RadioInfo> done);
    Result<RadioInfo> info(std::chrono::milliseconds t = k_default_timeout);
    // Asks the radio when up, else answers the last known FCS mode. ENODATA
    // if not known yet.
    void caps(Done<RadioCaps> done);
    Result<RadioCaps> caps(std::chrono::milliseconds t = k_default_timeout);
    void stats(Done<RadioStats> done);
    Result<RadioStats> stats(std::chrono::milliseconds t = k_default_timeout);
    // `config slot=`: saves to the slot, loads it, and makes it the slot later
    // PHY changes save to. Read it back from spec().config_slot.
    void set_config_slot(uint8_t slot, StatusDone done);
    Status set_config_slot(uint8_t slot,
                           std::chrono::milliseconds t = k_default_timeout);
    void reset(StatusDone done);
    Status reset(std::chrono::milliseconds t = k_default_timeout);
    // Transparent radio m-plane (console radio_mplane|rmp): sends one radio
    // command line unchanged and answers the radio's reply body verbatim, for
    // radio-specific commands l3 does not model (for example the C6's
    // radio_tx_cal). l3 does not track what it changes: a PHY field changed
    // this way can be overwritten by the PHY reconcile, so use set_phy for
    // those. EINVAL: empty line. The radio's NOK comes back as the status.
    void mplane(const std::string& line, Done<std::string> done);
    Result<std::string> mplane(const std::string& line,
                               std::chrono::milliseconds t = k_default_timeout);

private:
    friend struct detail::HandleAccess;
    std::shared_ptr<detail::NodeCore> core;
};

class RadioManager
{
public:
    RadioManager() = default;

    // Validates spec and builds the node's radio (UDP or queue d-plane). Not
    // on air yet. EINVAL. EEXIST if the node already has a radio, added or
    // not.
    Result<Radio> create(const RadioSpec& spec);
    // Attaches the created radio: m-plane connect, programming, d-plane into
    // the TX mux and RX demux. Returns once armed; watch state() for up.
    // ENOENT if none was created, EALREADY if already added.
    Status add(const Radio& radio);
    Result<Radio> get() const;  // ENOENT if no radio was created
    Status remove();            // added or only created; upstreams on it: EBUSY

private:
    friend struct detail::HandleAccess;
    std::shared_ptr<detail::NodeCore> core;
};

}  // namespace winject_l3

#endif  // WINJECT_L3_PUBLIC_RADIO_H_
