#include "radio/TxMux.h"

#include "endpoint/Upstream.h"
#include "frames/Mpdu.h"
#include "radio/WifiUdp.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <bfc/buffer.hpp>
#include <bfc/sized_buffer.hpp>
#include <string.h>
#include <utility>

namespace
{
struct TxMpduEntryPlan
{
    size_t entry_index = 0;
    size_t sdu_bytes = 0;
    size_t framed_bytes = 0;
};
}  // namespace

namespace winject
{

TxMux::TxMux(RadioUpstreamTable& table) : table_(table) {}

void TxMux::configure(uint32_t max_rate_kbps, uint16_t domain,
                      size_t max_data_per_tick, size_t tx_burst_size,
                      uint32_t tx_burst_interval_us)
{
    rate_kbps = max_rate_kbps < 64 ? 64 : max_rate_kbps;
    domain_ = domain;
    max_data_per_tick_ = max_data_per_tick < 1 ? 1 : max_data_per_tick;
    if (max_data_per_tick_ > 32)
    {
        max_data_per_tick_ = 32;
    }
    tx_burst_size_ = tx_burst_size < 1 ? 1 : tx_burst_size;
    if (tx_burst_size_ > k_radio_tx_queue_depth)
    {
        tx_burst_size_ = k_radio_tx_queue_depth;
    }
    tx_burst_interval_us_ = tx_burst_interval_us;
    burst = k_wifi_payload_max * 2;
    tokens = burst;
    burst_data_sent_ = 0;
    burst_cooldown_until_ = {};
    last_refill = std::chrono::steady_clock::now();
}

bool TxMux::set_max_rate_kbps(uint32_t max_rate_kbps)
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    rate_kbps = max_rate_kbps < 64 ? 64 : max_rate_kbps;
    return true;
}

bool TxMux::set_max_data_per_tick(size_t max_data_per_tick)
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    max_data_per_tick_ = max_data_per_tick < 1 ? 1 : max_data_per_tick;
    if (max_data_per_tick_ > 32)
    {
        max_data_per_tick_ = 32;
    }
    return true;
}

bool TxMux::set_tx_burst_pacing(size_t tx_burst_size,
                                uint32_t tx_burst_interval_us)
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    tx_burst_size_ = tx_burst_size < 1 ? 1 : tx_burst_size;
    if (tx_burst_size_ > k_radio_tx_queue_depth)
    {
        tx_burst_size_ = k_radio_tx_queue_depth;
    }
    tx_burst_interval_us_ = tx_burst_interval_us;
    burst_data_sent_ = 0;
    burst_cooldown_until_ = {};
    return true;
}

bool TxMux::data_burst_allows() const
{
    const auto now = std::chrono::steady_clock::now();
    if (now < burst_cooldown_until_)
    {
        return false;
    }
    return burst_data_sent_ < tx_burst_size_;
}

void TxMux::note_data_burst_emit()
{
    burst_data_sent_++;
    if (burst_data_sent_ < tx_burst_size_)
    {
        return;
    }
    burst_cooldown_until_ = std::chrono::steady_clock::now() +
                            std::chrono::microseconds(tx_burst_interval_us_);
    burst_data_sent_ = 0;
}

void TxMux::refill()
{
    const auto now = std::chrono::steady_clock::now();
    auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - last_refill)
            .count();
    if (us < 0)
    {
        us = 0;
    }
    last_refill = now;
    tokens +=
        (static_cast<uint64_t>(rate_kbps) * static_cast<uint64_t>(us)) / 8000;
    if (tokens > burst)
    {
        tokens = burst;
    }
}

void TxMux::init_schedule_shares(std::vector<size_t>& shares) const
{
    const auto& entries = table_.entries();
    shares.resize(entries.size());
    for (size_t i = 0; i < entries.size(); i++)
    {
        const auto& s = entries[i];
        if (s.up == nullptr || s.up->get_tx_size() == 0)
        {
            shares[i] = 0;
            continue;
        }
        shares[i] = s.budget;
    }
}

bool TxMux::emit_mpdu(size_t primary, std::vector<size_t>& schedule_shares,
                      size_t* data_sent)
{
    auto& entries = table_.entries();
    if (primary >= entries.size() || data_sent == nullptr || domain_ == 0)
    {
        return false;
    }
    auto& s0 = entries[primary];
    if (s0.up == nullptr || s0.radio == nullptr || s0.bus_tx == 0)
    {
        return false;
    }
    if (!data_burst_allows())
    {
        return false;
    }
    if (tokens == 0 || *data_sent >= max_data_per_tick_)
    {
        return false;
    }
    if (schedule_shares[primary] == 0)
    {
        return false;
    }
    if (s0.up->get_tx_size() == 0)
    {
        schedule_shares[primary] = 0;
        return false;
    }

    size_t body_total = 0;
    uint8_t n = 0;
    bool any_data = false;

    std::vector<TxMpduEntryPlan> plan;

    auto try_plan = [&](size_t i) -> bool
    {
        if (n >= WIFI_PDU_SLOTS)
        {
            return false;
        }
        auto& s = entries[i];
        if (s.up == nullptr || s.bus_tx == 0)
        {
            return false;
        }
        if (schedule_shares[i] == 0)
        {
            return false;
        }

        const size_t first_sdu = s.up->get_tx_size();
        if (first_sdu == 0)
        {
            schedule_shares[i] = 0;
            return false;
        }

        const size_t room = k_wifi_payload_max - body_total;
        if (room < k_lc_sequence_len + 1)
        {
            return false;
        }
        const size_t room_payload = room - k_lc_sequence_len;

        size_t max_sdu = k_stream_payload_max;
        if (tokens < max_sdu)
        {
            max_sdu = static_cast<size_t>(tokens);
        }
        if (schedule_shares[i] < max_sdu)
        {
            max_sdu = schedule_shares[i];
        }
        if (room_payload < max_sdu)
        {
            max_sdu = room_payload;
        }
        if (max_sdu == 0 || first_sdu > max_sdu)
        {
            return false;
        }

        const size_t framed = first_sdu + k_lc_sequence_len;
        if (framed > schedule_shares[i])
        {
            return false;
        }

        plan.push_back(TxMpduEntryPlan{i, first_sdu, framed});
        body_total += framed;
        if (framed <= tokens)
        {
            tokens -= framed;
        }
        else
        {
            tokens = 0;
        }
        n++;
        return true;
    };

    if (!try_plan(primary))
    {
        return false;
    }
    for (size_t step = 1; step < entries.size() && n < WIFI_PDU_SLOTS; step++)
    {
        const size_t i = (primary + step) % entries.size();
        try_plan(i);
    }

    for (size_t p = 0; p < plan.size(); p++)
    {
        const TxMpduEntryPlan& entry_plan = plan[p];
        auto& s = entries[entry_plan.entry_index];
        size_t max = entry_plan.sdu_bytes;

        bfc::sized_buffer sdu = s.up->pull_tx(max);
        if (sdu.empty() || sdu.size() != max)
        {
            return false;
        }
        const uint8_t* payload = reinterpret_cast<const uint8_t*>(sdu.data());
        const size_t pulled = sdu.size();
        size_t framed = 0;
        if (!s.up->stamp_air(framed_buf[p], sizeof(framed_buf[p]), payload,
                             pulled, &framed))
        {
            return false;
        }
        if (framed != entry_plan.framed_bytes)
        {
            return false;
        }
        any_data = true;
    }

    const size_t mpdu_len = WIFI_HDR_LEN + body_total;
    if (mpdu_len > sizeof(mpdu_buf))
    {
        return false;
    }
    Mpdu mpdu(mpdu_buf, mpdu_len);
    for (size_t p = 0; p < plan.size(); p++)
    {
        const TxMpduEntryPlan& entry_plan = plan[p];
        mpdu.set_slot_payload(
            static_cast<uint8_t>(entry_plan.entry_index),
            static_cast<uint16_t>(entry_plan.framed_bytes));
    }
    if (!mpdu.rescan())
    {
        return false;
    }
    for (size_t p = 0; p < plan.size(); p++)
    {
        const TxMpduEntryPlan& entry_plan = plan[p];
        bfc::buffer_view slot = mpdu.get_slot_payload(
            static_cast<uint8_t>(entry_plan.entry_index));
        if (slot.empty() || slot.size() != entry_plan.framed_bytes)
        {
            return false;
        }
        memcpy(slot.data(), framed_buf[p], entry_plan.framed_bytes);
    }
    ieee_802_11::SeqControl* seq = mpdu.ieee().seq_ctl;
    if (seq == nullptr)
    {
        return false;
    }
    seq->set_seq_num(next_tx_sequence());
    seq->set_fragment_num(0);
    mpdu.set_domain(domain_);
    if (!s0.radio->send(mpdu_buf, mpdu_len))
    {
        return false;
    }
    for (const TxMpduEntryPlan& entry_plan : plan)
    {
        auto& s = entries[entry_plan.entry_index];
        if (entry_plan.framed_bytes <= schedule_shares[entry_plan.entry_index])
        {
            schedule_shares[entry_plan.entry_index] -= entry_plan.framed_bytes;
        }
        else
        {
            schedule_shares[entry_plan.entry_index] = 0;
        }
        if (s.up == nullptr || s.up->get_tx_size() == 0)
        {
            schedule_shares[entry_plan.entry_index] = 0;
        }
    }
    air_bytes_interval += body_total;
    if (any_data)
    {
        (*data_sent)++;
        note_data_burst_emit();
    }
    return true;
}

void TxMux::tick_impl()
{
    auto& entries = table_.entries();
    if (entries.empty())
    {
        return;
    }
    refill();

    std::vector<size_t> schedule_shares;
    init_schedule_shares(schedule_shares);

    size_t data_sent = 0;

    bool progress = true;
    while (progress && tokens > 0 && data_sent < max_data_per_tick_)
    {
        progress = false;
        for (size_t n = 0;
             n < entries.size() && tokens > 0 && data_sent < max_data_per_tick_;
             n++)
        {
            const size_t i = (next + n) % entries.size();
            if (emit_mpdu(i, schedule_shares, &data_sent))
            {
                progress = true;
                next = (i + 1) % entries.size();
            }
        }
    }
}

uint64_t TxMux::take_air_bytes()
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    const uint64_t n = air_bytes_interval;
    air_bytes_interval = 0;
    return n;
}

uint64_t TxMux::peek_air_bytes() const
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    return air_bytes_interval;
}

void TxMux::log_stats(double interval_sec)
{
    auto kbps = [interval_sec](uint64_t bytes) -> double
    {
        return interval_sec > 0.0 ? (bytes * 8.0) / interval_sec / 1000.0 : 0.0;
    };

    const uint64_t total_tx = take_air_bytes();
    const uint64_t total_lost = table_.collect_seq_loss_delta();

    LOG_INF("STREAM TOTAL TX=%6.0f LOST=%llu", kbps(total_tx),
            static_cast<unsigned long long>(total_lost));
}

void TxMux::start()
{
    if (tx_thread_.joinable())
    {
        return;
    }
    tx_stop_ = false;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        tx_work_.clear();
    }
    tx_thread_ = std::thread(
        [this]()
        {
            tx_thread_main();
        });
}

void TxMux::stop()
{
    tx_stop_ = true;
    wake_cv_.notify_all();
    if (tx_thread_.joinable())
    {
        tx_thread_.join();
    }
}

void TxMux::request_tick()
{
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        if (tx_work_.empty())
        {
            tx_work_.push_back(k_tx_work_tick);
        }
    }
    wake_cv_.notify_one();
}

void TxMux::sync_tick()
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    tick_impl();
}

void TxMux::tx_thread_main()
{
    while (!tx_stop_)
    {
        {
            std::unique_lock<std::mutex> lock(wake_mu_);
            wake_cv_.wait_for(lock,
                              std::chrono::microseconds(k_tx_tick_interval_us),
                              [this]()
                              {
                                  return !tx_work_.empty() || tx_stop_.load();
                              });
            if (tx_stop_)
            {
                break;
            }
            tx_work_.clear();
        }
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(table_.mutex());
                tick_impl();
            }
            std::lock_guard<std::mutex> lock(wake_mu_);
            if (tx_work_.empty())
            {
                break;
            }
            tx_work_.clear();
        }
    }
}

}  // namespace winject
