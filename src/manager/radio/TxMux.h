#ifndef WINJECT_MANAGER_RADIO_TX_MUX_H_
#define WINJECT_MANAGER_RADIO_TX_MUX_H_

#include "Config.h"
#include "radio/PhyAirtime.h"
#include "radio/RadioDefs.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/TxEvent.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace winject
{

#if defined(ESP_PLATFORM)
inline constexpr size_t k_tx_thread_stack_bytes = 8192;
#endif
inline constexpr size_t k_pacing_full_psdu_bytes = 1504;
// Share of the modulation's airtime (frame airtime + inter-frame gap) that
// TX pacing uses.
inline constexpr uint32_t k_tx_airtime_share_pct = 90;
// Longest the TX thread sleeps without a wake: producers wake it only when a
// queue goes from empty to non-empty, so this bounds any missed wake.
inline constexpr std::chrono::milliseconds k_tx_max_sleep{5};

class WifiUdp;

class TxMux
{
public:
    explicit TxMux(RadioUpstreamTable& table);
    ~TxMux();

    void configure(uint16_t domain);
    void set_phy_mode(const PhyMode& mode);
    uint32_t pacing_txtime_full_us() const;
    uint32_t pacing_gap_us() const;
    // Spacing between full-size frames: (airtime + gap) / 90 %.
    uint32_t pacing_interval_full_us() const;
    void start();
    void stop();
    void request_tick();
    void post_send_socket_change(std::shared_ptr<WifiUdp> radio,
                                 const sockaddr_in& dplane);
    void sync_tick();
    void log_stats(double interval_sec);
    uint64_t take_air_bytes();
    uint64_t tx_send_fail_mpdu() const;
    uint64_t tx_send_fail_byt() const;

private:
    void tick_impl();
    void tx_thread_main();
    void init_schedule_shares(std::vector<size_t>& shares) const;
    bool has_pending_tx_data() const;
    bool emit_mpdu(size_t primary, std::vector<size_t>& schedule_shares);
    bool note_build_fail(size_t body_bytes);
    uint32_t frame_interval_us(size_t psdu_bytes) const;
    bool pop_event(TxEvent* out);
    void handle_event(TxEvent& ev);
    void apply_queued_socket_changes();

    RadioUpstreamTable& table_;
    PhyMode phy_{};
    uint32_t gap_us_ = 96;
    uint32_t pacing_credit_us_ = 0;
    uint16_t domain_ = 0;
    std::chrono::steady_clock::time_point next_tx_at_{};
    size_t next = 0;
    uint8_t framed_buf[5][2048]{};
    uint8_t mpdu_buf[WIFI_RADIO_INJECT_MAX]{};
    uint64_t air_bytes_interval = 0;
    uint16_t bus_air_tx_[256] = {};
    std::atomic<uint64_t> tx_send_fail_mpdu_{0};
    std::atomic<uint64_t> tx_send_fail_byt_{0};

    std::thread tx_thread_;
    std::mutex wake_mu_;
    std::condition_variable wake_cv_;
    TxEventQueue tx_events_;
    std::atomic<bool> tx_stop_{false};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_TX_MUX_H_
