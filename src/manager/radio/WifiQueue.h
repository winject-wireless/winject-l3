#ifndef WINJECT_MANAGER_WIFI_QUEUE_H_
#define WINJECT_MANAGER_WIFI_QUEUE_H_

#include "radio/RadioDplane.h"

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
#include <thread>

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
//
// close() never waits for a reactor, which may be stopped or busy elsewhere.
// It waits out an rx/idle callback or posted RX event running on another
// thread, and none runs after it returns. It may be called from inside on_rx
// or on_idle. A callback must not destroy the WifiQueue.
struct WifiQueueDplaneBinding
{
    // Manager -> peer: inject MPDUs and 1-byte registration (same as UDP TX).
    std::string wifi_tx;
    // Peer -> manager: forward frames with 4-byte trailer (same as UDP RX).
    std::string wifi_rx;
    std::string reactor;  // wakes both ends on queue activity
};

class WifiQueue final : public RadioDplane
{
public:
    static constexpr size_t k_tx_mpdu_max = WIFI_RADIO_INJECT_MAX;
    static constexpr size_t k_rx_mpdu_max = WIFI_RADIO_RX_MAX;

    WifiQueue();
    ~WifiQueue() override;
    WifiQueue(const WifiQueue&) = delete;
    WifiQueue& operator=(const WifiQueue&) = delete;

    bool open(IOReactor& reactor, rx on_rx, idle on_idle = {}) override;
    bool bind_dplane(const WifiQueueDplaneBinding& binding);
    // No destination to set: the queues are the d-plane.
    void set_tx_dplane(const sockaddr_in& dplane) override;
    // EventCtrlRecvSocketChange re-sends the forward registration.
    void post_rx_event(RxEvent ev) override;
    bool register_forward() override;
    void close() override;
    bool send(const uint8_t* mpdu, size_t len) override;

    counters_s peek_counters() const override
    {
        counters_s c;
        c.tx_byte = tx_byte_acc.load(std::memory_order_relaxed);
        c.rx_byte = rx_byte_acc.load(std::memory_order_relaxed);
        c.tx_pkt = tx_pkt_acc.load(std::memory_order_relaxed);
        c.rx_pkt = rx_pkt_acc.load(std::memory_order_relaxed);
        c.fcs_error_pkt = fcs_error_pkt_acc.load(std::memory_order_relaxed);
        c.fcs_unknown_pkt = fcs_unknown_pkt_acc.load(std::memory_order_relaxed);
        c.tx_drops = wifi_tx_drops_acc.load(std::memory_order_relaxed);
        return c;
    }

private:
    using DplaneQueue =
        bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
    using DplaneReactor = bfc::cv_reactor<>;

    // Reactor callbacks reach the WifiQueue through this, never by raw this:
    // owner is cleared on destruction, and mu is held while a callback runs.
    struct Gate
    {
        std::mutex mu;
        WifiQueue* owner = nullptr;
        // Thread running a callback, so close() from inside one does not wait
        // on itself.
        std::atomic<std::thread::id> running{};
    };
    template <typename Fn>
    static void through_gate(const std::shared_ptr<Gate>& gate, Fn&& fn);

    bool push_wifi_tx(const uint8_t* data, size_t len);
    void on_wifi_rx();

    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    // Guarded by transport_mu.
    IOReactor* reactor = nullptr;
    // Guarded by gate->mu.
    rx on_rx;
    idle on_idle;
    std::atomic<uint64_t> tx_byte_acc{0};
    std::atomic<uint64_t> rx_byte_acc{0};
    std::atomic<uint64_t> tx_pkt_acc{0};
    std::atomic<uint64_t> rx_pkt_acc{0};
    std::atomic<uint64_t> fcs_error_pkt_acc{0};
    std::atomic<uint64_t> fcs_unknown_pkt_acc{0};
    std::atomic<uint64_t> wifi_tx_drops_acc{0};
    mutable std::mutex transport_mu;
    bool opened = false;
    bool dplane_bound = false;
    std::shared_ptr<DplaneQueue> wifi_tx;
    std::shared_ptr<DplaneQueue> wifi_rx;
    std::shared_ptr<DplaneReactor> cv_reactor;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_WIFI_QUEUE_H_
