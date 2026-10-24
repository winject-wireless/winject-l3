#include "radio/TxMux.h"

#include "console/ConsoleParse.h"
#include "endpoint/Upstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/WifiUdp.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <bfc/buffer.hpp>
#include <bfc/sized_buffer.hpp>
#include <cinttypes>
#include <string.h>
#include <utility>

namespace
{
struct TxMpduEntryPlan
{
    size_t entry_index = 0;
    bfc::shared_sized_buffer slot_payload;
    size_t slot_bytes = 0;
};
}  // namespace

namespace winject
{

struct TxMux::QueueWake
{
    std::mutex mutex;
    TxMux* owner = nullptr;
    void wake_up()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (owner)
        {
            owner->request_tick();
        }
    }
};
TxMux::TxMux(RadioUpstreamTable& table)
    : queue_wake_(std::make_shared<QueueWake>()), table_(table)
{
    queue_wake_->owner = this;
}
std::shared_ptr<BoundedLossyQueue> TxMux::make_tx_queue(size_t capacity)
{
    return std::make_shared<BoundedLossyQueue>(
        capacity, [wake = queue_wake_] { wake->wake_up(); });
}

TxMux::~TxMux()
{
    {
        std::lock_guard<std::mutex> lock(queue_wake_->mutex);
        queue_wake_->owner = nullptr;
    }
    stop();
}

void TxMux::configure(uint16_t domain)
{
    {
        std::lock_guard<std::mutex> lock(table_.mutex());
        domain_ = domain;
        next_tx_at_ = std::chrono::steady_clock::now();
    }
    request_tick();
}

void TxMux::set_phy_mode(const PhyMode& mode)
{
    {
        std::lock_guard<std::mutex> lock(table_.mutex());
        phy_ = mode;
        gap_us_ = phy_default_gap_us(mode);
        pacing_credit_us_ = frame_interval_us(k_pacing_full_psdu_bytes) * 2;
        next_tx_at_ = std::chrono::steady_clock::now();
        LOG_INF("tx pacing: (%" PRIu32 "us+%" PRIu32 "us)/%" PRIu32
                "%% = %" PRIu32 "us per full frame",
                phy_txtime_us(phy_, k_pacing_full_psdu_bytes), gap_us_,
                k_tx_airtime_share_pct,
                frame_interval_us(k_pacing_full_psdu_bytes));
    }
    request_tick();
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
        if (s.tx_queue == nullptr || s.tx_queue->head_size() == 0)
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
    if (s0.tx_queue == nullptr || s0.radio == nullptr || s0.bus_tx == 0)
    {
        return false;
    }
    if (schedule_shares[primary] == 0)
    {
        return false;
    }
    if (s0.tx_queue->head_size() == 0)
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
        if (s.tx_queue == nullptr || s.bus_tx == 0)
        {
            return false;
        }
        if (schedule_shares[i] == 0)
        {
            return false;
        }

        const size_t first_sdu = s.tx_queue->head_size();
        if (first_sdu == 0)
        {
            schedule_shares[i] = 0;
            return false;
        }

        const size_t room = k_wifi_payload_max - body_total;
        if (room < k_slot_prefix_len + 1)
        {
            return false;
        }

        size_t max_slot = k_wifi_payload_max;
        if (room < max_slot)
        {
            max_slot = room;
        }
        // Shares count whole slot payloads. An entry that has not sent yet this
        // pass may send one slot larger than its share, so a budget below the
        // slot size slows the upstream instead of blocking it.
        if (schedule_shares[i] < s.budget && schedule_shares[i] < max_slot)
        {
            max_slot = schedule_shares[i];
        }
        if (max_slot < k_slot_prefix_len + 1 || first_sdu > max_slot)
        {
            return false;
        }

        // Pop atomically checks the head against the available frame space.
        auto item = s.tx_queue->pop(max_slot);
        if (!item || item->empty())
        {
            schedule_shares[i] = 0;
            return false;
        }
        auto slot_payload = std::move(*item);
        const size_t pulled = slot_payload.size();
        if (pulled < k_slot_prefix_len ||
            RouteHeader::read_bus(
                reinterpret_cast<const uint8_t*>(slot_payload.data())) !=
                s.bus_tx)
        {
            schedule_shares[i] = 0;
            return false;
        }

        TxMpduEntryPlan entry;
        entry.entry_index = i;
        entry.slot_payload = std::move(slot_payload);
        entry.slot_bytes = pulled;
        plan.push_back(std::move(entry));
        body_total += pulled;
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

    const size_t mpdu_len = WIFI_HDR_LEN + body_total;
    if (mpdu_len > sizeof(mpdu_buf))
    {
        return note_build_fail(body_total);
    }
    Mpdu mpdu(mpdu_buf, mpdu_len);
    for (size_t p = 0; p < plan.size(); p++)
    {
        mpdu.set_slot_payload(static_cast<uint8_t>(p),
                              static_cast<uint16_t>(plan[p].slot_bytes));
    }
    if (!mpdu.rescan())
    {
        return note_build_fail(body_total);
    }
    for (size_t p = 0; p < plan.size(); p++)
    {
        bfc::buffer_view slot = mpdu.get_slot_payload(static_cast<uint8_t>(p));
        if (slot.empty() || slot.size() != plan[p].slot_bytes)
        {
            return note_build_fail(body_total);
        }
        memcpy(slot.data(),
               reinterpret_cast<const uint8_t*>(plan[p].slot_payload.data()),
               plan[p].slot_bytes);
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
        // Failed nonblocking submission consumed these SDUs. Pace further
        // attempts instead of stalling the remaining backlog or spinning.
        next_tx_at_ =
            now + std::chrono::microseconds(frame_interval_us(mpdu_len + 4));
        pacing_blocked_ = true;
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
        if (entry_plan.slot_bytes <= schedule_shares[entry_plan.entry_index])
        {
            schedule_shares[entry_plan.entry_index] -= entry_plan.slot_bytes;
        }
        else
        {
            schedule_shares[entry_plan.entry_index] = 0;
        }
        if (s.tx_queue == nullptr || s.tx_queue->head_size() == 0)
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
    size_t emitted = 0;
    constexpr size_t k_max_frames_per_turn = 8;
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
                    if (++emitted == k_max_frames_per_turn)
                    {
                        return true;
                    }
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

struct TxMux::ReactorState
{
    // A cancelled callback can already have been extracted by the reactor.
    // The shared guard fences active callbacks and invalidates stale ones.
    std::recursive_mutex gate;
    TxMux* owner = nullptr;
    Wake wake;
    Arm arm;
    Cancel cancel;
    std::optional<TimerId> timer;
    uint64_t generation = 0;
    // Protected by TxMux::wake_mu_, independently of the callback gate.
    bool wake_pending = false;
};

TxEventQueue TxMux::take_events()
{
    std::lock_guard<std::mutex> lock(wake_mu_);
    TxEventQueue events = std::move(tx_events_);
    tx_events_ = {};
    return events;
}

void TxMux::handle_event(TxEvent& ev)
{
    if (auto* ctrl = std::get_if<EventCtrlSendSocketChange>(&ev))
    {
        if (ctrl->radio)
        {
            std::lock_guard<std::mutex> lock(table_.mutex());
            ctrl->radio->set_tx_dplane(ctrl->dplane);
            LOG_INF("tx dplane -> %s",
                    console_format_ipv4_port(ctrl->dplane).c_str());
        }
    }
}

void TxMux::apply_events(TxEventQueue events)
{
    TxEvent event;
    while (events.pop(&event))
    {
        handle_event(event);
    }
}

void TxMux::post_send_socket_change(std::shared_ptr<WifiUdp> radio,
                                    const sockaddr_in& dplane)
{
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        tx_events_.push(EventCtrlSendSocketChange{std::move(radio), dplane});
    }
    notify_reactor();
}

void TxMux::start_on(Wake wake, Arm arm, Cancel cancel)
{
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        if (reactor_state_)
        {
            return;
        }
        auto state = std::make_shared<ReactorState>();
        state->owner = this;
        state->wake = std::move(wake);
        state->arm = std::move(arm);
        state->cancel = std::move(cancel);
        reactor_state_ = std::move(state);
    }
    request_tick();  // also discovers data queued before the reactor was bound
}

void TxMux::stop()
{
    std::shared_ptr<ReactorState> state;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        state = reactor_state_;
    }
    if (state)
    {
        std::lock_guard<std::recursive_mutex> guard(state->gate);
        state->owner = nullptr;
        ++state->generation;
        if (state->timer)
        {
            state->cancel(*state->timer);
        }
        state->timer.reset();
        std::lock_guard<std::mutex> lock(wake_mu_);
        if (reactor_state_ == state)
        {
            reactor_state_.reset();
        }
    }
    auto events = take_events();
    events.drop_data();
    apply_events(std::move(events));
}

void TxMux::request_tick()
{
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        tx_events_.push_data();
    }
    notify_reactor();
}

void TxMux::notify_reactor()
{
    std::shared_ptr<ReactorState> state;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        state = reactor_state_;
        if (!state || state->wake_pending)
        {
            return;
        }
        state->wake_pending = true;
    }
    // Fence posting against stop too: no wake can be posted after stop returns.
    std::lock_guard<std::recursive_mutex> guard(state->gate);
    if (!state->owner)
    {
        return;
    }
    std::weak_ptr<ReactorState> weak = state;
    state->wake(
        [weak]()
        {
            if (auto current = weak.lock())
            {
                std::lock_guard<std::recursive_mutex> guard(current->gate);
                if (current->owner)
                {
                    current->owner->on_reactor(current, true);
                }
            }
        });
}

bool TxMux::has_pending_tx_locked() const
{
    if (domain_ == 0)
    {
        return false;
    }
    for (const auto& entry : table_.entries())
    {
        if (entry.tx_queue && entry.radio && entry.bus_tx && entry.budget &&
            entry.tx_queue->head_size())
        {
            return true;
        }
    }
    return false;
}

void TxMux::on_reactor(const std::shared_ptr<ReactorState>& state, bool wake)
{
    if (wake)
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        state->wake_pending = false;
    }
    ++state->generation;
    if (state->timer)
    {
        state->cancel(*state->timer);
    }
    state->timer.reset();
    apply_events(take_events());
    bool again;
    std::chrono::steady_clock::time_point deadline;
    {
        std::lock_guard<std::mutex> lock(table_.mutex());
        again = tick_impl() && has_pending_tx_locked();
        deadline = next_tx_at_;
    }
    if (!again)
    {
        return;  // no periodic polling and no timer while idle
    }
    const auto delay = std::chrono::duration_cast<std::chrono::microseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count();
    const auto generation = state->generation;
    std::weak_ptr<ReactorState> weak = state;
    state->timer = state->arm(
        std::max<int64_t>(0, delay),
        [weak, generation]()
        {
            if (auto current = weak.lock())
            {
                std::lock_guard<std::recursive_mutex> guard(current->gate);
                if (current->owner && current->generation == generation)
                {
                    current->timer.reset();
                    current->owner->on_reactor(current, false);
                }
            }
        });
}

}  // namespace winject
