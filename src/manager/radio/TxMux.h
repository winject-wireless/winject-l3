#ifndef WINJECT_MANAGER_RADIO_TX_MUX_H_
#define WINJECT_MANAGER_RADIO_TX_MUX_H_

#include "Config.h"
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

class TxMux
{
public:
    explicit TxMux(RadioUpstreamTable& table);

    void configure(uint32_t max_rate_kbps, uint16_t domain,
                   size_t max_data_per_tick = 4,
                   size_t tx_burst_size = k_default_tx_burst_size,
                   uint32_t tx_burst_interval_us = k_default_tx_burst_interval_us);
    bool set_max_rate_kbps(uint32_t max_rate_kbps);
    bool set_max_data_per_tick(size_t max_data_per_tick);
    bool set_tx_burst_pacing(size_t tx_burst_size,
                             uint32_t tx_burst_interval_us);
    uint32_t max_rate_kbps() const
    {
        return rate_kbps;
    }
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
    void start();
    void stop();
    void request_tick();
    void sync_tick();
    void log_stats(double interval_sec);
    uint64_t take_air_bytes();
    uint64_t peek_air_bytes() const;

private:
    void tick_impl();
    void tx_thread_main();
    void refill();
    void init_schedule_shares(std::vector<size_t>& shares) const;
    bool emit_mpdu(size_t primary, std::vector<size_t>& schedule_shares,
                   size_t* data_sent);
    bool data_burst_allows() const;
    void note_data_burst_emit();

    RadioUpstreamTable& table_;
    uint32_t rate_kbps = 10000;
    uint16_t domain_ = 0;
    size_t max_data_per_tick_ = 4;
    size_t tx_burst_size_ = k_default_tx_burst_size;
    uint32_t tx_burst_interval_us_ = k_default_tx_burst_interval_us;
    size_t burst_data_sent_ = 0;
    std::chrono::steady_clock::time_point burst_cooldown_until_{};
    uint64_t tokens = 0;
    uint64_t burst = 0;
    size_t next = 0;
    std::chrono::steady_clock::time_point last_refill{};
    uint8_t framed_buf[5][2048]{};
    uint8_t mpdu_buf[1500]{};
    uint64_t air_bytes_interval = 0;

    std::thread tx_thread_;
    std::mutex wake_mu_;
    std::condition_variable wake_cv_;
    std::deque<uint8_t> tx_work_;
    std::atomic<bool> tx_stop_{false};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_TX_MUX_H_
