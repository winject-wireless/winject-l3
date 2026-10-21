#ifndef WINJECT_MANAGER_WIFI_QUEUE_H_
#define WINJECT_MANAGER_WIFI_QUEUE_H_

#include "radio/RadioDefs.h"
#include "radio/RxEvent.h"
#include "utils/IOReactor.h"

#include <atomic>
#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <sys/socket.h>

namespace winject
{

// In-process d-plane transport between winject-l3 and a radio peer (for example
// winject_proxy on the same chip). Payloads are the same bytes as UDP port
// 9000: inject MPDUs (24–1472 bytes), 1-byte forward registration, or a
// forward datagram (MPDU + 4-byte trailer).
//
// The host registers two distinct lossy queues on
// bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer> and
// a running bfc::cv_reactor<> before bind_dplane(). This class does not create
// them. Registry names are conventionally wifi_tx and wifi_rx; the peer
// consumes wifi_tx and produces wifi_rx.
//
// m-plane (UDP console) stays on WifiUdp / sockets; only the d-plane uses
// queues until a queue m-plane exists.
struct WifiQueueDplaneBinding
{
    // Manager -> peer: inject MPDUs and 1-byte registration (same as UDP TX).
    std::string wifi_tx;
    // Peer -> manager: forward frames with 4-byte trailer (same as UDP RX).
    std::string wifi_rx;
    std::string reactor;  // wakes both ends on queue activity
};

class WifiQueue
{
public:
    using rx = std::function<void(bfc::shared_sized_buffer mpdu)>;
    using idle = std::function<void()>;

    static constexpr size_t k_tx_mpdu_max = WIFI_RADIO_INJECT_MAX;
    static constexpr size_t k_rx_mpdu_max = WIFI_RADIO_RX_MAX;

    WifiQueue() = default;
    ~WifiQueue();
    WifiQueue(const WifiQueue&) = delete;
    WifiQueue& operator=(const WifiQueue&) = delete;

    bool open(IOReactor& reactor, rx on_rx, idle on_idle = {});
    bool bind_dplane(const WifiQueueDplaneBinding& binding);
    // UDP compat: no-op once bound; triggers register_forward when bound.
    void set_tx_dplane(const sockaddr_in& dplane);
    void post_rx_event(RxEvent ev);
    bool register_forward();
    void close();
    bool send(const uint8_t* mpdu, size_t len);

    void set_fcs_mode(RadioFcsMode mode)
    {
        fcs_mode_.store(mode, std::memory_order_release);
    }
    RadioFcsMode fcs_mode() const
    {
        return fcs_mode_.load(std::memory_order_acquire);
    }

    struct counters_s
    {
        uint64_t tx_byte = 0;
        uint64_t rx_byte = 0;
        uint64_t tx_pkt = 0;
        uint64_t rx_pkt = 0;
        uint64_t fcs_error_pkt = 0;
        uint64_t fcs_unknown_pkt = 0;
        uint64_t wifi_tx_drops = 0;
    };
    counters_s peek_counters() const
    {
        counters_s c;
        c.tx_byte = tx_byte_.load(std::memory_order_relaxed);
        c.rx_byte = rx_byte_.load(std::memory_order_relaxed);
        c.tx_pkt = tx_pkt_.load(std::memory_order_relaxed);
        c.rx_pkt = rx_pkt_.load(std::memory_order_relaxed);
        c.fcs_error_pkt = fcs_error_pkt_.load(std::memory_order_relaxed);
        c.fcs_unknown_pkt = fcs_unknown_pkt_.load(std::memory_order_relaxed);
        c.wifi_tx_drops = wifi_tx_drops_.load(std::memory_order_relaxed);
        return c;
    }

private:
    using DplaneQueue =
        bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
    using DplaneReactor = bfc::cv_reactor<>;

    bool push_wifi_tx(const uint8_t* data, size_t len);
    void on_wifi_rx();

    IOReactor* reactor_ = nullptr;
    rx on_rx_;
    idle on_idle_;
    std::atomic<RadioFcsMode> fcs_mode_{RadioFcsMode::unknown};
    std::atomic<uint64_t> tx_byte_{0};
    std::atomic<uint64_t> rx_byte_{0};
    std::atomic<uint64_t> tx_pkt_{0};
    std::atomic<uint64_t> rx_pkt_{0};
    std::atomic<uint64_t> fcs_error_pkt_{0};
    std::atomic<uint64_t> fcs_unknown_pkt_{0};
    std::atomic<uint64_t> wifi_tx_drops_{0};
    mutable std::mutex transport_mu_;
    bool opened_ = false;
    bool dplane_bound_ = false;
    std::shared_ptr<DplaneQueue> wifi_tx_;
    std::shared_ptr<DplaneQueue> wifi_rx_;
    std::shared_ptr<DplaneReactor> cv_reactor_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_WIFI_QUEUE_H_
