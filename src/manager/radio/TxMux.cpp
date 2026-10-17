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
#include <cinttypes>
#include <string.h>
#include <utility>

#if defined(ESP_PLATFORM)
#include "esp_pthread.h"
#endif

namespace
{
struct TxMpduEntryPlan
{
    size_t entry_index = 0;
    bfc::sized_buffer sdu;
    bool is_fec = false;
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

void TxMux::configure(uint16_t domain)
{
    domain_ = domain;
    next_tx_at_ = std::chrono::steady_clock::now();
}

void TxMux::set_phy_mode(const PhyMode& mode)
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    phy_ = mode;
    gap_us_ = phy_default_gap_us(mode);
    pacing_credit_us_ = frame_interval_us(k_pacing_full_psdu_bytes) * 2;
    next_tx_at_ = std::chrono::steady_clock::now();
    LOG_INF("tx pacing: (%" PRIu32 "us+%" PRIu32 "us)/%" PRIu32 "%% = %" PRIu32
            "us per full frame",
            phy_txtime_us(phy_, k_pacing_full_psdu_bytes), gap_us_,
            k_tx_airtime_share_pct,
            frame_interval_us(k_pacing_full_psdu_bytes));
}

uint32_t TxMux::frame_interval_us(size_t psdu_bytes) const
{
    const uint64_t air_us = phy_txtime_us(phy_, psdu_bytes) + gap_us_;
    return static_cast<uint32_t>((air_us * 100 + k_tx_airtime_share_pct - 1) /
                                 k_tx_airtime_share_pct);
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

uint32_t TxMux::pacing_interval_full_us() const
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    return frame_interval_us(k_pacing_full_psdu_bytes);
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

bool TxMux::note_build_fail(size_t body_bytes)
{
    // SDUs were already pulled from their queues; count them as lost so the
    // drop shows up in tx_send_fail stats instead of vanishing.
    tx_send_fail_mpdu_.fetch_add(1, std::memory_order_relaxed);
    tx_send_fail_byt_.fetch_add(body_bytes, std::memory_order_relaxed);
    return false;
}

bool TxMux::emit_mpdu(size_t primary, std::vector<size_t>& schedule_shares)
{
    auto& entries = table_.entries();
    if (primary >= entries.size() || domain_ == 0)
    {
        return false;
    }
    auto& s0 = entries[primary];
    if (s0.up == nullptr || s0.radio == nullptr || s0.bus_tx == 0)
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
    const auto now = std::chrono::steady_clock::now();
    if (now < next_tx_at_)
    {
        pacing_blocked_ = true;
        return false;
    }

    size_t body_total = 0;
    uint8_t n = 0;

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
        if (room_payload < max_sdu)
        {
            max_sdu = room_payload;
        }
        // The share counts framed bytes (SDU + LC header). An entry that has
        // not sent yet this pass may send one SDU larger than its share, so a
        // budget below the SDU size slows the upstream instead of blocking it.
        if (schedule_shares[i] < s.budget)
        {
            const size_t share_payload =
                schedule_shares[i] > k_lc_header_len
                    ? schedule_shares[i] - k_lc_header_len
                    : 0;
            if (share_payload < max_sdu)
            {
                max_sdu = share_payload;
            }
        }
        if (max_sdu == 0 || first_sdu > max_sdu)
        {
            return false;
        }

        // pull_tx returns at most max_sdu bytes, so the SDU always fits here.
        bool is_fec = false;
        bfc::sized_buffer sdu = s.up->pull_tx(max_sdu, &is_fec);
        if (sdu.empty())
        {
            schedule_shares[i] = 0;
            return false;
        }
        const size_t pulled = sdu.size();
        const size_t framed = pulled + k_lc_header_len;

        TxMpduEntryPlan entry;
        entry.entry_index = i;
        entry.sdu = std::move(sdu);
        entry.is_fec = is_fec;
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
        if (!stamp_air_payload(&bus_air_tx_[s.bus_tx], s.bus_tx,
                               entry_plan.is_fec, framed_buf[p],
                               sizeof(framed_buf[p]), payload, pulled,
                               &framed) ||
            framed != entry_plan.framed_bytes)
        {
            return note_build_fail(body_total);
        }
    }

    const size_t mpdu_len = WIFI_HDR_LEN + body_total;
    if (mpdu_len > sizeof(mpdu_buf))
    {
        return note_build_fail(body_total);
    }
    Mpdu mpdu(mpdu_buf, mpdu_len);
    for (size_t p = 0; p < plan.size(); p++)
    {
        mpdu.set_slot_payload(static_cast<uint8_t>(p),
                              static_cast<uint16_t>(plan[p].framed_bytes));
    }
    if (!mpdu.rescan())
    {
        return note_build_fail(body_total);
    }
    for (size_t p = 0; p < plan.size(); p++)
    {
        bfc::buffer_view slot = mpdu.get_slot_payload(static_cast<uint8_t>(p));
        if (slot.empty() || slot.size() != plan[p].framed_bytes)
        {
            return note_build_fail(body_total);
        }
        memcpy(slot.data(), framed_buf[p], plan[p].framed_bytes);
    }
    ieee_802_11::SeqControl* seq = mpdu.ieee().seq_ctl;
    if (seq == nullptr)
    {
        return note_build_fail(body_total);
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
    // Whole frame on air: MPDU + 4-byte FCS.
    const uint32_t interval_us = frame_interval_us(mpdu_len + 4);
    // Up to two full-frame intervals missed by a late wakeup are made up.
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
    return true;
}

bool TxMux::tick_impl()
{
    auto& entries = table_.entries();
    pacing_blocked_ = false;
    if (entries.empty())
    {
        return false;
    }
    std::vector<size_t> schedule_shares;
    // Each round gives every upstream its scheduler budget; budgets weight
    // upstreams against each other and do not cap the rate. Rounds repeat
    // until pacing closes or a round sends nothing.
    for (;;)
    {
        init_schedule_shares(schedule_shares);
        pacing_blocked_ = false;
        bool sent = false;
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (size_t n = 0; n < entries.size(); n++)
            {
                const size_t i = (next + n) % entries.size();
                if (emit_mpdu(i, schedule_shares))
                {
                    progress = true;
                    sent = true;
                    next = (i + 1) % entries.size();
                }
            }
        }
        if (!sent || pacing_blocked_)
        {
            return pacing_blocked_;
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
#if defined(ESP_PLATFORM)
    // std::thread gets CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT (3 KB by
    // default), too small for the TX path; restore the caller's config after.
    esp_pthread_cfg_t prev_cfg;
    const bool had_cfg = esp_pthread_get_cfg(&prev_cfg) == ESP_OK;
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = k_tx_thread_stack_bytes;
    cfg.thread_name = "winject_tx";
    esp_pthread_set_cfg(&cfg);
#endif
    tx_thread_ = std::thread(
        [this]()
        {
            tx_thread_main();
        });
#if defined(ESP_PLATFORM)
    if (had_cfg)
    {
        esp_pthread_set_cfg(&prev_cfg);
    }
    else
    {
        const esp_pthread_cfg_t def = esp_pthread_get_default_config();
        esp_pthread_set_cfg(&def);
    }
#endif
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

// Sleeps until an event (data queued on an idle upstream, d-plane change,
// stop), until the next frame may go out while data waits on pacing, or for
// at most k_tx_max_sleep.
void TxMux::tx_thread_main()
{
    auto woken = [this]()
    {
        return !tx_events_.empty() || tx_stop_.load();
    };
    auto wake_at = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> wlock(wake_mu_);
    while (!tx_stop_)
    {
        wake_cv_.wait_until(wlock, wake_at, woken);
        if (tx_stop_)
        {
            break;
        }
        TxEvent ev;
        while (pop_event(&ev))
        {
            wlock.unlock();
            handle_event(ev);
            wlock.lock();
        }
        wlock.unlock();
        bool paced = false;
        std::chrono::steady_clock::time_point pacing_until;
        {
            std::lock_guard<std::mutex> lock(table_.mutex());
            paced = tick_impl();
            pacing_until = next_tx_at_;
        }
        wlock.lock();
        // Data held back by pacing goes at the next slot (at once if the slot
        // passed meanwhile). Data no upstream could send (e.g. no d-plane
        // yet) waits for a wake or the cap, not a spin.
        wake_at = std::chrono::steady_clock::now() + k_tx_max_sleep;
        if (paced && pacing_until < wake_at)
        {
            wake_at = pacing_until;
        }
    }
}

}  // namespace winject
