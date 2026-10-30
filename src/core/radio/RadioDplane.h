#ifndef WINJECT_MANAGER_RADIO_DPLANE_H_
#define WINJECT_MANAGER_RADIO_DPLANE_H_

#include "radio/RadioDefs.h"
#include "radio/RxEvent.h"
#include "utils/IOReactor.h"

#include <atomic>
#include <bfc/shared_sized_buffer.hpp>
#include <functional>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

namespace winject
{

// Radio d-plane transport: inject MPDUs out, forward frames in. WifiUdp
// carries it over UDP (radio.dplane), WifiQueue over in-process queues
// (radio.dplane_tx / radio.dplane_rx / radio.reactor). on_rx runs on the
// transport's reactor thread: the IO reactor for UDP, the d-plane CV reactor
// for queues.
class RadioDplane
{
public:
    using rx = std::function<void(bfc::shared_sized_buffer mpdu)>;
    using idle = std::function<void()>;

    struct counters_s
    {
        uint64_t tx_byte = 0;
        uint64_t rx_byte = 0;
        uint64_t tx_pkt = 0;
        uint64_t rx_pkt = 0;
        uint64_t fcs_error_pkt = 0;
        uint64_t fcs_unknown_pkt = 0;
        // Oldest MPDUs evicted from a full TX queue (queue d-plane only).
        uint64_t tx_drops = 0;
    };

    virtual ~RadioDplane() = default;

    virtual bool open(IOReactor& reactor, rx on_rx, idle on_idle = {}) = 0;
    // UDP destination for inject MPDUs; a queue d-plane ignores it.
    virtual void set_tx_dplane(const sockaddr_in& dplane) = 0;
    virtual void post_rx_event(RxEvent ev) = 0;
    virtual bool register_forward() = 0;
    virtual void close() = 0;
    virtual bool send(const uint8_t* mpdu, size_t len) = 0;
    virtual counters_s peek_counters() const = 0;

    void set_fcs_mode(RadioFcsMode mode)
    {
        fcs_mode_acc.store(mode, std::memory_order_release);
    }
    RadioFcsMode fcs_mode() const
    {
        return fcs_mode_acc.load(std::memory_order_acquire);
    }

private:
    std::atomic<RadioFcsMode> fcs_mode_acc{RadioFcsMode::unknown};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_DPLANE_H_
