#ifndef WINJECT_MANAGER_RADIO_TX_MUX_H_
#define WINJECT_MANAGER_RADIO_TX_MUX_H_

#include "Config.h"
#include "radio/PhyAirtime.h"
#include "radio/RadioDefs.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/TxEvent.h"

#include <atomic>
#include <bfc/function.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace winject
{

inline constexpr size_t k_pacing_full_psdu_bytes = 1504;
// Share of the modulation's airtime (frame airtime + inter-frame gap) that
// TX pacing uses.
inline constexpr uint32_t k_tx_airtime_share_pct = 90;
class RadioDplane;

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
    // Owner calls start/stop on the chosen reactor thread or while stopped.
    // Producers request work after releasing their queue/table locks.
    template <typename Reactor>
    void start(Reactor& reactor)
    {
        start_on(
            [&reactor](Callback fn)
            {
                reactor.wake_up(std::move(fn));
            },
            [&reactor](int64_t us, Callback fn)
            {
                return reactor.get_timer().wait_us(us, std::move(fn));
            },
            [&reactor](TimerId id)
            {
                reactor.get_timer().cancel(id);
            });
    }
    // The caller passes this queue to the upstream constructor. Push wakes TX.
    std::shared_ptr<BoundedLossyQueue> make_tx_queue(size_t capacity = 1024);
    void stop();
    void request_tick();
    void post_send_socket_change(std::shared_ptr<RadioDplane> radio,
                                 const sockaddr_in& dplane);
    void log_stats(double interval_sec);
    uint64_t take_air_bytes();
    uint64_t tx_send_fail_mpdu() const;
    uint64_t tx_send_fail_byt() const;

private:
    // True when pacing or the bounded work budget requires another turn.
    bool tick_impl();
    using TimerId = std::pair<int64_t, uint64_t>;
    using Callback = bfc::light_function<void()>;
    using Wake = std::function<void(Callback)>;
    using Arm = std::function<TimerId(int64_t, Callback)>;
    using Cancel = std::function<void(TimerId)>;
    struct ReactorState;
    void start_on(Wake wake, Arm arm, Cancel cancel);
    void notify_reactor();
    void on_reactor(const std::shared_ptr<ReactorState>& state, bool wake);
    bool has_pending_tx_locked() const;
    void init_schedule_shares(std::vector<size_t>& shares) const;
    bool emit_mpdu(size_t primary, std::vector<size_t>& schedule_shares);
    bool note_build_fail(size_t body_bytes);
    uint32_t frame_interval_us(size_t psdu_bytes) const;
    void handle_event(TxEvent& ev);
    TxEventQueue take_events();
    void apply_events(TxEventQueue events);

    struct QueueWake;
    std::shared_ptr<QueueWake> queue_wake;
    RadioUpstreamTable& table;
    PhyMode phy{};
    uint32_t gap_us = 96;
    uint32_t pacing_credit_us = 0;
    // Set by emit_mpdu when pacing (next_tx_at) refused a frame.
    bool pacing_blocked = false;
    uint16_t domain = 0;
    std::chrono::steady_clock::time_point next_tx_at{};
    size_t next = 0;
    uint8_t mpdu_buf[WIFI_RADIO_INJECT_MAX]{};
    uint64_t air_bytes_interval = 0;
    std::atomic<uint64_t> tx_send_fail_mpdu_total{0};
    std::atomic<uint64_t> tx_send_fail_byt_total{0};

    std::mutex wake_mu;
    TxEventQueue tx_events;
    std::shared_ptr<ReactorState> reactor_state;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_TX_MUX_H_
