#ifndef WINJECT_MANAGER_WIFI_UDP_H_
#define WINJECT_MANAGER_WIFI_UDP_H_

#include "radio/RadioDplane.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <bfc/shared_sized_buffer.hpp>
#include <functional>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>

namespace winject
{

// Opaque full-MPDU UDP transport to/from the ESP32 radio.
class WifiUdp final : public RadioDplane
{
public:
    static constexpr size_t k_tx_mpdu_max = WIFI_RADIO_INJECT_MAX;
    static constexpr size_t k_rx_mpdu_max = WIFI_RADIO_RX_MAX;

    WifiUdp() = default;
    ~WifiUdp() override;
    WifiUdp(const WifiUdp&) = delete;
    WifiUdp& operator=(const WifiUdp&) = delete;

    bool open(IOReactor& reactor, rx on_rx, idle on_idle = {}) override;
    void set_tx_dplane(const sockaddr_in& dplane) override;
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
        return c;
    }

private:
    void on_forward();
    bool forward_trailer_ok(const uint8_t* frame, size_t len) const;
    void handle_rx_event(const RxEvent& ev);
    bool set_rx_dplane(const sockaddr_in& dplane);

    IOReactor* reactor = nullptr;
    bfc::socket sock;
    // TxMux reactor only.
    sockaddr_in tx_dplane{};
    bool have_tx_dplane = false;
    // Reactor thread only.
    sockaddr_in rx_dplane{};
    bool have_rx_dplane = false;
    rx on_rx;
    idle on_idle;
    std::atomic<uint64_t> tx_byte_acc{0};
    std::atomic<uint64_t> rx_byte_acc{0};
    std::atomic<uint64_t> tx_pkt_acc{0};
    std::atomic<uint64_t> rx_pkt_acc{0};
    std::atomic<uint64_t> fcs_error_pkt_acc{0};
    std::atomic<uint64_t> fcs_unknown_pkt_acc{0};
    static constexpr size_t k_recv_capacity = 2048;
    bfc::shared_sized_buffer rx_buf;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_WIFI_UDP_H_
