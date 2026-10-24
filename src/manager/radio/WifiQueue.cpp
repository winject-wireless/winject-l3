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
    this->reactor = &reactor;
    this->on_rx = std::move(on_rx);
    this->on_idle = std::move(on_idle);
    opened = true;
    return true;
}

bool WifiQueue::bind_dplane(const WifiQueueDplaneBinding& binding)
{
    if (!opened || reactor == nullptr)
    {
        LOG_ERR("wifi queue: open before bind_dplane");
        return false;
    }
    if (binding.wifi_tx.empty() || binding.wifi_rx.empty() ||
        binding.reactor.empty())
    {
        LOG_ERR("wifi queue dplane needs wifi_tx, wifi_rx, and reactor");
        return false;
    }
    if (binding.wifi_tx == binding.wifi_rx)
    {
        LOG_ERR("wifi queue wifi_tx and wifi_rx must differ");
        return false;
    }

    auto tx_queue = DplaneQueueRegistry::get_instance().get(binding.wifi_tx);
    auto rx_queue = DplaneQueueRegistry::get_instance().get(binding.wifi_rx);
    if (!tx_queue)
    {
        LOG_ERR("wifi queue wifi_tx %s: not registered", binding.wifi_tx.c_str());
        return false;
    }
    if (!rx_queue)
    {
        LOG_ERR("wifi queue wifi_rx %s: not registered", binding.wifi_rx.c_str());
        return false;
    }
    if (tx_queue == rx_queue)
    {
        LOG_ERR("wifi queue wifi_tx and wifi_rx are the same queue");
        return false;
    }
    if (tx_queue->capacity() == 0)
    {
        LOG_ERR("wifi queue wifi_tx %s capacity is 0", binding.wifi_tx.c_str());
        return false;
    }
    if (rx_queue->capacity() == 0)
    {
        LOG_ERR("wifi queue wifi_rx %s capacity is 0", binding.wifi_rx.c_str());
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
        std::lock_guard<std::mutex> lock(transport_mu);
        if (dplane_bound)
        {
            LOG_ERR("wifi queue dplane already bound");
            return false;
        }
        wifi_tx = std::move(tx_queue);
        wifi_rx = std::move(rx_queue);
        cv_reactor = std::move(cv);
        dplane_bound = true;
    }

    cv_reactor->add_read_rdy(
        *wifi_rx,
        [this]()
        {
            on_wifi_rx();
        });
    cv_reactor->wake_up();
    LOG_INF("wifi queue dplane wifi_tx=%s wifi_rx=%s reactor=%s",
            binding.wifi_tx.c_str(), binding.wifi_rx.c_str(),
            binding.reactor.c_str());
    return true;
}

void WifiQueue::set_tx_dplane(const sockaddr_in& dplane)
{
    (void)dplane;
}

void WifiQueue::post_rx_event(RxEvent ev)
{
    if (!opened || reactor == nullptr)
    {
        return;
    }
    reactor->wake_up(
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
    return push_wifi_tx(&k_reg, 1);
}

void WifiQueue::close()
{
    std::shared_ptr<DplaneQueue> rx_queue;
    std::shared_ptr<DplaneReactor> cv;
    {
        std::lock_guard<std::mutex> lock(transport_mu);
        if (!opened)
        {
            return;
        }
        opened = false;
        dplane_bound = false;
        rx_queue = wifi_rx;
        cv = cv_reactor;
        wifi_tx.reset();
        wifi_rx.reset();
        cv_reactor.reset();
    }
    if (cv && rx_queue)
    {
        cv->remove_read_rdy(*rx_queue);
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
    reactor = nullptr;
    on_rx = nullptr;
    on_idle = nullptr;
}

bool WifiQueue::push_wifi_tx(const uint8_t* data, size_t len)
{
    std::shared_ptr<DplaneQueue> tx_queue;
    std::shared_ptr<DplaneReactor> cv;
    {
        std::lock_guard<std::mutex> lock(transport_mu);
        if (!dplane_bound)
        {
            return false;
        }
        tx_queue = wifi_tx;
        cv = cv_reactor;
    }
    if (!tx_queue || !cv || data == nullptr || len == 0)
    {
        return false;
    }
    const size_t before =
        tx_queue->push(bfc::shared_sized_buffer::copy_from(data, len));
    const size_t capacity = tx_queue->capacity();
    if (capacity != 0 && before == capacity)
    {
        wifi_tx_drops_acc.fetch_add(1, std::memory_order_relaxed);
    }
    cv->wake_up();
    return true;
}

bool WifiQueue::send(const uint8_t* mpdu, size_t len)
{
    if (!dplane_bound || mpdu == nullptr || len == 0)
    {
        return false;
    }
    if (len < 24 || len > k_tx_mpdu_max)
    {
        return false;
    }
    if (!push_wifi_tx(mpdu, len))
    {
        return false;
    }
    tx_pkt_acc.fetch_add(1, std::memory_order_relaxed);
    tx_byte_acc.fetch_add(len, std::memory_order_relaxed);
    return true;
}

void WifiQueue::on_wifi_rx()
{
    bool any = false;
    for (int drained = 0; drained < k_max_datagrams_per_wake; ++drained)
    {
        std::shared_ptr<DplaneQueue> rx_queue;
        {
            std::lock_guard<std::mutex> lock(transport_mu);
            if (!opened || !dplane_bound)
            {
                return;
            }
            rx_queue = wifi_rx;
        }
        if (!rx_queue)
        {
            return;
        }
        auto pkt_opt = rx_queue->pop();
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
        rx_pkt_acc.fetch_add(1, std::memory_order_relaxed);
        rx_byte_acc.fetch_add(len, std::memory_order_relaxed);
        if (len < 28 || len > k_rx_mpdu_max + 4)
        {
            continue;
        }
        const uint8_t* frame = pkt.u8();
        if (fcs_mode() == RadioFcsMode::unknown)
        {
            fcs_unknown_pkt_acc.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!wifi_forward_trailer_ok(frame, len, fcs_mode()))
        {
            fcs_error_pkt_acc.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        pkt.resize(len - 4);
        if (on_rx)
        {
            on_rx(std::move(pkt));
        }
    }
    if (any && on_idle)
    {
        on_idle();
    }
    std::shared_ptr<DplaneQueue> rx_queue;
    std::shared_ptr<DplaneReactor> cv;
    {
        std::lock_guard<std::mutex> lock(transport_mu);
        if (!opened || !dplane_bound)
        {
            return;
        }
        rx_queue = wifi_rx;
        cv = cv_reactor;
    }
    if (rx_queue && cv && rx_queue->has_data())
    {
        cv->wake_up();
    }
}

}  // namespace winject
