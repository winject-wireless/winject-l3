#ifndef WINJECT_CORE_RADIO_RADIO_CONFIG_H_
#define WINJECT_CORE_RADIO_RADIO_CONFIG_H_

#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <sys/socket.h>

namespace winject
{

// radio.fcs config (runtime mode is RadioFcsMode on the radio d-plane).
enum class RadioFcsConfig
{
    auto_detect,
    signal,
    actual,
};

// One radio the manager drives (docs/mplane.md § radio_device). The m-plane
// is always UDP. The d-plane is UDP (radio.dplane) or, with all of
// radio.dplane_tx, radio.dplane_rx and radio.reactor, in-process queues
// (WifiQueue). A radio_device command can change a UDP address but not
// clear it, and cannot change a queue d-plane.
struct RadioDeviceConfig
{
    sockaddr_in mplane{};  // radio.mplane host:port (ESP32 :22, Realtek :2201)
    sockaddr_in dplane{};  // radio.dplane host:port (ESP32 :9000)
    // Queue d-plane: registry names of the inject (manager -> radio) and
    // forward (radio -> manager) queues, and the host's running cv_reactor.
    std::string dplane_tx;
    std::string dplane_rx;
    std::string reactor;
    RadioFcsConfig fcs = RadioFcsConfig::auto_detect;

    bool dplane_is_queue() const
    {
        return !dplane_tx.empty();
    }
};

// The radio PHY the node programs (docs/mplane.md § radio_tx).
struct RadioConfig
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
};

}  // namespace winject

#endif  // WINJECT_CORE_RADIO_RADIO_CONFIG_H_
