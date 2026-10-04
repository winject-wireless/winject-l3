#ifndef WINJECT_MANAGER_WIFI_UDP_H_
#define WINJECT_MANAGER_WIFI_UDP_H_

#include "radio/RadioDefs.h"
#include "radio/RxEvent.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <bfcext/shared_sized_buffer.hpp>
#include <functional>
#include <netinet/in.h>
#include <stdint.h>

namespace winject
{

// Opaque full-MPDU UDP transport to/from the ESP32 radio.
class WifiUdp
{
public:
    using rx = std::function<void(bfcext::shared_sized_buffer mpdu)>;
    using idle = std::function<void()>;

    static constexpr size_t k_tx_mpdu_max = WIFI_RADIO_INJECT_MAX;
    static constexpr size_t k_rx_mpdu_max = WIFI_RADIO_RX_MAX;

    WifiUdp() = default;
    ~WifiUdp();
    WifiUdp(const WifiUdp&) = delete;
    WifiUdp& operator=(const WifiUdp&) = delete;

    bool open(IOReactor& reactor, rx on_rx, idle on_idle = {});
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
        return c;
    }

private:
    void on_forward();
    bool forward_trailer_ok(const uint8_t* frame, size_t len) const;
    void handle_rx_event(const RxEvent& ev);
    bool set_rx_dplane(const sockaddr_in& dplane);

    IOReactor* reactor = nullptr;
    bfc::socket sock;
    // TxMux TX thread only.
    sockaddr_in tx_dplane_{};
    bool have_tx_dplane_ = false;
    // Reactor thread only.
    sockaddr_in rx_dplane_{};
    bool have_rx_dplane_ = false;
    rx on_rx;
    idle on_idle;
    std::atomic<RadioFcsMode> fcs_mode_{RadioFcsMode::unknown};
    std::atomic<uint64_t> tx_byte_{0};
    std::atomic<uint64_t> rx_byte_{0};
    std::atomic<uint64_t> tx_pkt_{0};
    std::atomic<uint64_t> rx_pkt_{0};
    std::atomic<uint64_t> fcs_error_pkt_{0};
    std::atomic<uint64_t> fcs_unknown_pkt_{0};
    static constexpr size_t k_recv_capacity = 2048;
    bfcext::shared_sized_buffer rx_buf_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_WIFI_UDP_H_
