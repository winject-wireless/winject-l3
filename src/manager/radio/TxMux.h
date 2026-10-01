#ifndef WINJECT_MANAGER_RADIO_TX_MUX_H_
#define WINJECT_MANAGER_RADIO_TX_MUX_H_

#include "Config.h"
#include "radio/PhyAirtime.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RadioDefs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <thread>
#include <vector>

namespace winject
{

inline constexpr uint32_t k_tx_tick_interval_us = 250;
inline constexpr uint8_t k_tx_work_tick = 1;
inline constexpr size_t k_pacing_full_psdu_bytes = 1504;

class TxMux
{
public:
    explicit TxMux(RadioUpstreamTable& table);
    ~TxMux();

    void configure(uint16_t domain, size_t max_data_per_tick = 4,
                   size_t tx_burst_size = k_default_tx_burst_size,
                   uint32_t tx_burst_interval_us = k_default_tx_burst_interval_us);
    void set_phy_mode(const PhyMode& mode, uint32_t gap_us,
                      uint32_t rate_cap_kbps);
    bool set_max_data_per_tick(size_t max_data_per_tick);
    bool set_tx_burst_pacing(size_t tx_burst_size,
                             uint32_t tx_burst_interval_us);
    size_t max_data_per_tick() const
    {
        return max_data_per_tick_;
    }
    size_t tx_burst_size() const
    {
        return tx_burst_size_;
    }
    uint32_t tx_burst_interval_us() const
    {
        return tx_burst_interval_us_;
    }
    uint32_t pacing_txtime_full_us() const;
    uint32_t pacing_gap_us() const;
    void start();
    void stop();
    void request_tick();
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
    bool emit_mpdu(size_t primary, std::vector<size_t>& schedule_shares,
                   size_t* data_sent);
    bool data_burst_allows() const;
    void note_data_burst_emit();

    RadioUpstreamTable& table_;
    PhyMode phy_{};
    uint32_t gap_us_ = 96;
    uint32_t rate_cap_kbps_ = 0;
    uint32_t pacing_credit_us_ = 0;
    uint16_t domain_ = 0;
    size_t max_data_per_tick_ = 4;
    size_t tx_burst_size_ = k_default_tx_burst_size;
    uint32_t tx_burst_interval_us_ = k_default_tx_burst_interval_us;
    size_t burst_data_sent_ = 0;
    std::chrono::steady_clock::time_point burst_cooldown_until_{};
    std::chrono::steady_clock::time_point next_tx_at_{};
    size_t next = 0;
    uint8_t framed_buf[5][2048]{};
    uint8_t mpdu_buf[1500]{};
    uint64_t air_bytes_interval = 0;
    uint16_t bus_air_tx_[256] = {};
    std::atomic<uint64_t> tx_send_fail_mpdu_{0};
    std::atomic<uint64_t> tx_send_fail_byt_{0};

    std::thread tx_thread_;
    std::mutex wake_mu_;
    std::condition_variable wake_cv_;
    std::deque<uint8_t> tx_work_;
    std::atomic<bool> tx_stop_{false};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_TX_MUX_H_
