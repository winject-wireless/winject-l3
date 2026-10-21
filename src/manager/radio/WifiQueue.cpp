#include "radio/WifiQueue.h"

#include "radio/WifiFcs.h"
#include "utils/Log.h"

#include <chrono>
#include <future>
#include <utility>

namespace winject
{

namespace
{

using DplaneQueueRegistry =
    bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

constexpr int k_max_datagrams_per_wake = 8;

}  // namespace

WifiQueue::~WifiQueue()
{
    close();
}

bool WifiQueue::open(IOReactor& reactor, rx on_rx, idle on_idle)
{
    close();
    reactor_ = &reactor;
    on_rx_ = std::move(on_rx);
    on_idle_ = std::move(on_idle);
    opened_ = true;
    return true;
}

bool WifiQueue::bind_dplane(const WifiQueueDplaneBinding& binding)
{
    if (!opened_ || reactor_ == nullptr)
    {
        LOG_ERR("wifi queue: open before bind_dplane");
        return false;
    }
    if (binding.inject_tx.empty() || binding.forward_rx.empty() ||
        binding.reactor.empty())
    {
        LOG_ERR("wifi queue dplane needs inject_tx, forward_rx, and reactor");
        return false;
    }
    if (binding.inject_tx == binding.forward_rx)
    {
        LOG_ERR("wifi queue inject_tx and forward_rx must differ");
        return false;
    }

    auto inject = DplaneQueueRegistry::get_instance().get(binding.inject_tx);
    auto forward =
        DplaneQueueRegistry::get_instance().get(binding.forward_rx);
    if (!inject)
    {
        LOG_ERR("wifi queue inject_tx %s: not registered",
                binding.inject_tx.c_str());
        return false;
    }
    if (!forward)
    {
        LOG_ERR("wifi queue forward_rx %s: not registered",
                binding.forward_rx.c_str());
        return false;
    }
    if (inject == forward)
    {
        LOG_ERR("wifi queue inject_tx and forward_rx are the same queue");
        return false;
    }
    if (inject->capacity() == 0)
    {
        LOG_ERR("wifi queue inject_tx %s capacity is 0",
                binding.inject_tx.c_str());
        return false;
    }
    if (forward->capacity() == 0)
    {
        LOG_ERR("wifi queue forward_rx %s capacity is 0",
                binding.forward_rx.c_str());
        return false;
    }

    auto cv = bfc::cv_reactor_registry::get_instance().get(binding.reactor);
    if (!cv)
    {
        LOG_ERR("wifi queue reactor %s: not registered",
                binding.reactor.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (dplane_bound_)
        {
            LOG_ERR("wifi queue dplane already bound");
            return false;
        }
        inject_tx_ = std::move(inject);
        forward_rx_ = std::move(forward);
        cv_reactor_ = std::move(cv);
        dplane_bound_ = true;
    }

    cv_reactor_->add_read_rdy(
        *forward_rx_,
        [this]()
        {
            on_forward_rx();
        });
    cv_reactor_->wake_up();
    LOG_INF("wifi queue dplane inject_tx=%s forward_rx=%s reactor=%s",
            binding.inject_tx.c_str(), binding.forward_rx.c_str(),
            binding.reactor.c_str());
    return true;
}

void WifiQueue::set_tx_dplane(const sockaddr_in& dplane)
{
    (void)dplane;
}

void WifiQueue::post_rx_event(RxEvent ev)
{
    if (!opened_ || reactor_ == nullptr)
    {
        return;
    }
    reactor_->wake_up(
        [this, ev = std::move(ev)]()
        {
            std::visit(
                [this](const auto& e)
                {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, EventCtrlRecvSocketChange>)
                    {
                        (void)e;
                        register_forward();
                    }
                },
                ev);
        });
}

bool WifiQueue::register_forward()
{
    static const uint8_t k_reg = 0;
    return push_inject(&k_reg, 1);
}

void WifiQueue::close()
{
    std::shared_ptr<DplaneQueue> forward;
    std::shared_ptr<DplaneReactor> cv;
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (!opened_)
        {
            return;
        }
        opened_ = false;
        dplane_bound_ = false;
        forward = forward_rx_;
        cv = cv_reactor_;
        inject_tx_.reset();
        forward_rx_.reset();
        cv_reactor_.reset();
    }
    if (cv && forward)
    {
        cv->remove_read_rdy(*forward);
        if (!cv->is_reactor_thread())
        {
            auto done = std::make_shared<std::promise<void>>();
            auto finished = done->get_future();
            cv->wake_up(
                [done]()
                {
                    done->set_value();
                });
            if (finished.wait_for(std::chrono::seconds(2)) !=
                std::future_status::ready)
            {
                LOG_ERR("wifi queue close: reactor did not ack detach");
            }
        }
    }
    reactor_ = nullptr;
    on_rx_ = nullptr;
    on_idle_ = nullptr;
}

bool WifiQueue::push_inject(const uint8_t* data, size_t len)
{
    std::shared_ptr<DplaneQueue> inject;
    std::shared_ptr<DplaneReactor> cv;
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (!dplane_bound_)
        {
            return false;
        }
        inject = inject_tx_;
        cv = cv_reactor_;
    }
    if (!inject || !cv || data == nullptr || len == 0)
    {
        return false;
    }
    const size_t before =
        inject->push(bfc::shared_sized_buffer::copy_from(data, len));
    const size_t capacity = inject->capacity();
    if (capacity != 0 && before == capacity)
    {
        inject_tx_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    cv->wake_up();
    return true;
}

bool WifiQueue::send(const uint8_t* mpdu, size_t len)
{
    if (!dplane_bound_ || mpdu == nullptr || len == 0)
    {
        return false;
    }
    if (len < 24 || len > k_tx_mpdu_max)
    {
        return false;
    }
    if (!push_inject(mpdu, len))
    {
        return false;
    }
    tx_pkt_.fetch_add(1, std::memory_order_relaxed);
    tx_byte_.fetch_add(len, std::memory_order_relaxed);
    return true;
}

void WifiQueue::on_forward_rx()
{
    bool any = false;
    for (int drained = 0; drained < k_max_datagrams_per_wake; ++drained)
    {
        std::shared_ptr<DplaneQueue> forward;
        {
            std::lock_guard<std::mutex> lock(transport_mu_);
            if (!opened_ || !dplane_bound_)
            {
                return;
            }
            forward = forward_rx_;
        }
        if (!forward)
        {
            return;
        }
        auto pkt_opt = forward->pop();
        if (!pkt_opt)
        {
            break;
        }
        auto pkt = std::move(*pkt_opt);
        if (pkt.empty())
        {
            continue;
        }
        any = true;
        const size_t len = pkt.size();
        rx_pkt_.fetch_add(1, std::memory_order_relaxed);
        rx_byte_.fetch_add(len, std::memory_order_relaxed);
        if (len < 28 || len > k_rx_mpdu_max + 4)
        {
            continue;
        }
        const uint8_t* frame = pkt.u8();
        if (fcs_mode() == RadioFcsMode::unknown)
        {
            fcs_unknown_pkt_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!wifi_forward_trailer_ok(frame, len, fcs_mode()))
        {
            fcs_error_pkt_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        pkt.resize(len - 4);
        if (on_rx_)
        {
            on_rx_(std::move(pkt));
        }
    }
    if (any && on_idle_)
    {
        on_idle_();
    }
    std::shared_ptr<DplaneQueue> forward;
    std::shared_ptr<DplaneReactor> cv;
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (!opened_ || !dplane_bound_)
        {
            return;
        }
        forward = forward_rx_;
        cv = cv_reactor_;
    }
    if (forward && cv && forward->has_data())
    {
        cv->wake_up();
    }
}

}  // namespace winject
