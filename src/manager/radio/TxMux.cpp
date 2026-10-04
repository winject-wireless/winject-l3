#include "radio/TxMux.h"

#include "console/ConsoleParse.h"
#include "endpoint/Upstream.h"
#include "endpoint/UpstreamStats.h"
#include "frames/Mpdu.h"
#include "radio/WifiUdp.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <bfc/buffer.hpp>
#include <bfc/sized_buffer.hpp>
#include <cassert>
#include <string.h>
#include <utility>

namespace
{
struct TxMpduEntryPlan
{
    size_t entry_index = 0;
    bfc::sized_buffer sdu;
    size_t framed_bytes = 0;
};
}  // namespace

namespace winject
{

TxMux::TxMux(RadioUpstreamTable& table) : table_(table) {}

TxMux::~TxMux()
{
    stop();
}

void TxMux::configure(uint16_t domain, size_t max_data_per_tick,
                      size_t tx_burst_size, uint32_t tx_burst_interval_us)
{
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
    burst_data_sent_ = 0;
    burst_cooldown_until_ = {};
    next_tx_at_ = std::chrono::steady_clock::now();
}

void TxMux::set_phy_mode(const PhyMode& mode, uint32_t gap_us,
                         uint32_t rate_cap_kbps)
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    phy_ = mode;
    gap_us_ = gap_us == 0 ? phy_default_gap_us(mode) : gap_us;
    rate_cap_kbps_ = rate_cap_kbps;
    const uint32_t full_iv =
        phy_txtime_us(phy_, k_pacing_full_psdu_bytes) + gap_us_;
    pacing_credit_us_ = full_iv * 2;
    next_tx_at_ = std::chrono::steady_clock::now();
    LOG_INF("tx pacing: %uus+%uus per full frame (cap=%u kbps)",
            phy_txtime_us(phy_, k_pacing_full_psdu_bytes), gap_us_,
            rate_cap_kbps_);
}

uint32_t TxMux::pacing_txtime_full_us() const
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    return phy_txtime_us(phy_, k_pacing_full_psdu_bytes);
}

uint32_t TxMux::pacing_gap_us() const
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    return gap_us_;
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

bool TxMux::has_pending_tx_data() const
{
    const auto& entries = table_.entries();
    for (const auto& s : entries)
    {
        if (s.up != nullptr && s.up->get_tx_size() > 0)
        {
            return true;
        }
    }
    return false;
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
    const auto now = std::chrono::steady_clock::now();
    if (now < next_tx_at_ || *data_sent >= max_data_per_tick_)
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
        if (room < k_lc_header_len + 1)
        {
            return false;
        }
        const size_t room_payload = room - k_lc_header_len;

        size_t max_sdu = k_stream_payload_max;
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

        bfc::sized_buffer sdu = s.up->pull_tx(max_sdu);
        if (sdu.empty())
        {
            schedule_shares[i] = 0;
            return false;
        }
        const size_t pulled = sdu.size();
        const size_t framed = pulled + k_lc_header_len;
        if (framed > schedule_shares[i])
        {
            return false;
        }

        TxMpduEntryPlan entry;
        entry.entry_index = i;
        entry.sdu = std::move(sdu);
        entry.framed_bytes = framed;
        plan.push_back(std::move(entry));
        body_total += framed;
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
        const uint8_t* payload =
            reinterpret_cast<const uint8_t*>(entry_plan.sdu.data());
        const size_t pulled = entry_plan.sdu.size();
        size_t framed = 0;
        if (!stamp_air_payload(&bus_air_tx_[s.bus_tx], s.bus_tx, framed_buf[p],
                               sizeof(framed_buf[p]), payload, pulled, &framed))
        {
            return false;
        }
        assert(framed == entry_plan.framed_bytes);
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
        mpdu.set_slot_payload(static_cast<uint8_t>(p),
                              static_cast<uint16_t>(plan[p].framed_bytes));
    }
    assert(mpdu.rescan());
    for (size_t p = 0; p < plan.size(); p++)
    {
        bfc::buffer_view slot = mpdu.get_slot_payload(static_cast<uint8_t>(p));
        assert(!slot.empty() && slot.size() == plan[p].framed_bytes);
        memcpy(slot.data(), framed_buf[p], plan[p].framed_bytes);
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
        tx_send_fail_mpdu_.fetch_add(1, std::memory_order_relaxed);
        tx_send_fail_byt_.fetch_add(mpdu_len, std::memory_order_relaxed);
        return false;
    }
    const size_t psdu_bytes = mpdu_len + 4;
    uint32_t interval_us = phy_txtime_us(phy_, psdu_bytes) + gap_us_;
    if (rate_cap_kbps_ > 0)
    {
        const uint32_t cap_us = static_cast<uint32_t>(
            (psdu_bytes * 8000ULL + rate_cap_kbps_ - 1) / rate_cap_kbps_);
        if (cap_us > interval_us)
        {
            interval_us = cap_us;
        }
    }
    const auto floor_time = now - std::chrono::microseconds(pacing_credit_us_);
    next_tx_at_ = std::max(next_tx_at_, floor_time) +
                  std::chrono::microseconds(interval_us);
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
    std::vector<size_t> schedule_shares;
    init_schedule_shares(schedule_shares);

    size_t data_sent = 0;

    bool progress = true;
    while (progress && data_sent < max_data_per_tick_)
    {
        progress = false;
        for (size_t n = 0; n < entries.size() && data_sent < max_data_per_tick_;
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

uint64_t TxMux::tx_send_fail_mpdu() const
{
    return tx_send_fail_mpdu_.load(std::memory_order_relaxed);
}

uint64_t TxMux::tx_send_fail_byt() const
{
    return tx_send_fail_byt_.load(std::memory_order_relaxed);
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

bool TxMux::pop_event(TxEvent* out)
{
    return tx_events_.pop(out);
}

void TxMux::handle_event(TxEvent& ev)
{
    if (std::holds_alternative<EventData>(ev))
    {
        return;
    }
    auto& ctrl = std::get<EventCtrlSendSocketChange>(ev);
    if (ctrl.radio)
    {
        std::lock_guard<std::mutex> lock(table_.mutex());
        ctrl.radio->set_tx_dplane(ctrl.dplane);
        LOG_INF("tx dplane -> %s",
                console_format_ipv4_port(ctrl.dplane).c_str());
    }
}

void TxMux::apply_queued_socket_changes()
{
    tx_events_.drop_data();
    TxEvent ev;
    while (pop_event(&ev))
    {
        handle_event(ev);
    }
}

void TxMux::post_send_socket_change(std::shared_ptr<WifiUdp> radio,
                                    const sockaddr_in& dplane)
{
    EventCtrlSendSocketChange ctrl;
    ctrl.radio = std::move(radio);
    ctrl.dplane = dplane;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        tx_events_.push(std::move(ctrl));
    }
    wake_cv_.notify_one();
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
        apply_queued_socket_changes();
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
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        apply_queued_socket_changes();
    }
}

void TxMux::request_tick()
{
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        tx_events_.push_data();
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
    auto drain = [this](std::unique_lock<std::mutex>& wlock) -> bool
    {
        bool any = false;
        TxEvent ev;
        while (pop_event(&ev))
        {
            wlock.unlock();
            handle_event(ev);
            wlock.lock();
            any = true;
        }
        return any;
    };
    while (!tx_stop_)
    {
        {
            std::unique_lock<std::mutex> lock(wake_mu_);
            wake_cv_.wait_for(lock,
                              std::chrono::microseconds(k_tx_tick_interval_us),
                              [this]()
                              {
                                  return !tx_events_.empty() || tx_stop_.load();
                              });
            if (tx_stop_)
            {
                break;
            }
            drain(lock);
        }
        for (;;)
        {
            bool pending = false;
            std::chrono::steady_clock::time_point pacing_until;
            {
                std::lock_guard<std::mutex> lock(table_.mutex());
                tick_impl();
                pending = has_pending_tx_data();
                pacing_until = next_tx_at_;
            }
            std::unique_lock<std::mutex> wlock(wake_mu_);
            if (drain(wlock))
            {
                break;
            }
            if (!pending || std::chrono::steady_clock::now() >= pacing_until)
            {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto until = std::min(
                pacing_until,
                now + std::chrono::microseconds(k_tx_tick_interval_us));
            wake_cv_.wait_until(wlock, until,
                                [this]()
                                {
                                    return !tx_events_.empty() ||
                                           tx_stop_.load();
                                });
            if (tx_stop_)
            {
                break;
            }
            if (drain(wlock))
            {
                break;
            }
        }
    }
}

}  // namespace winject
