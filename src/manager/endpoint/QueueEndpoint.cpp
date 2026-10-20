#include "endpoint/QueueEndpoint.h"

#include "utils/Log.h"

#include <chrono>
#include <future>
#include <utility>

namespace winject
{

namespace
{

using AppQueueRegistry =
    bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

constexpr int k_max_datagrams_per_wake = 32;

}  // namespace

QueueEndpoint::~QueueEndpoint()
{
    close();
}

bool QueueEndpoint::open(IOReactor& io_reactor, const UpstreamConfig& cfg,
                         std::function<void()> tx_wake)
{
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (opened_)
        {
            LOG_ERR("queue upstream already open");
            return false;
        }
    }
    if (cfg.queue_in.empty() || cfg.queue_out.empty() || cfg.reactor.empty())
    {
        LOG_ERR("queue upstream needs queue_in, queue_out, and reactor");
        return false;
    }
    if (cfg.queue_in == cfg.queue_out)
    {
        LOG_ERR("queue upstream queue_in and queue_out must differ");
        return false;
    }

    auto in_queue = AppQueueRegistry::get_instance().get(cfg.queue_in);
    auto out_queue = AppQueueRegistry::get_instance().get(cfg.queue_out);
    if (!in_queue)
    {
        LOG_ERR("queue upstream queue_in %s: not registered",
                cfg.queue_in.c_str());
        return false;
    }
    if (!out_queue)
    {
        LOG_ERR("queue upstream queue_out %s: not registered",
                cfg.queue_out.c_str());
        return false;
    }
    if (in_queue == out_queue)
    {
        LOG_ERR("queue upstream in and out are the same queue");
        return false;
    }
    if (in_queue->capacity() == 0)
    {
        LOG_ERR("queue upstream queue_in %s capacity is 0",
                cfg.queue_in.c_str());
        return false;
    }
    if (out_queue->capacity() == 0)
    {
        LOG_ERR("queue upstream queue_out %s capacity is 0",
                cfg.queue_out.c_str());
        return false;
    }

    auto cv = bfc::cv_reactor_registry::get_instance().get(cfg.reactor);
    if (!cv)
    {
        LOG_ERR("queue upstream reactor %s: not registered",
                cfg.reactor.c_str());
        return false;
    }

    io_reactor_ = &io_reactor;
    queue_in_name_ = cfg.queue_in;
    queue_out_name_ = cfg.queue_out;
    if (!start_air(
            std::move(tx_wake), &io_reactor.get_timer(),
            [this]()
            {
                if (io_reactor_ != nullptr)
                {
                    io_reactor_->wake_up([]() {});
                }
            },
            cfg, "queue"))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        queue_in_ = std::move(in_queue);
        queue_out_ = std::move(out_queue);
        reactor_ = std::move(cv);
        opened_ = true;
    }
    reactor_->add_read_rdy(*queue_in_,
                           [this]()
                           {
                               on_queue_in();
                           });
    // Drain datagrams pushed before the read handler was registered.
    reactor_->wake_up();
    LOG_INF("queue upstream in=%s out=%s reactor=%s", queue_in_name_.c_str(),
            queue_out_name_.c_str(), cfg.reactor.c_str());
    return true;
}

void QueueEndpoint::close()
{
    std::shared_ptr<AppQueue> in_queue;
    std::shared_ptr<AppReactor> reactor;
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (!opened_)
        {
            return;
        }
        opened_ = false;
        in_queue = queue_in_;
        reactor = reactor_;
    }
    if (reactor && in_queue)
    {
        reactor->remove_read_rdy(*in_queue);
        // The reactor may already have copied the callback for this wake.
        // A following wake runs only after that callback returns.
        if (!reactor->is_reactor_thread())
        {
            auto done = std::make_shared<std::promise<void>>();
            auto finished = done->get_future();
            reactor->wake_up(
                [done]()
                {
                    done->set_value();
                });
            if (finished.wait_for(std::chrono::seconds(2)) !=
                std::future_status::ready)
            {
                LOG_ERR("queue upstream close: reactor did not ack detach");
            }
        }
    }
    stop_air();
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        queue_in_.reset();
        queue_out_.reset();
        reactor_.reset();
    }
}

bool QueueEndpoint::can_deliver_app() const
{
    std::lock_guard<std::mutex> lock(transport_mu_);
    return opened_ && queue_out_ != nullptr && reactor_ != nullptr;
}

void QueueEndpoint::deliver_app(const uint8_t* data, size_t len)
{
    std::shared_ptr<AppQueue> out_queue;
    std::shared_ptr<AppReactor> reactor;
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (!opened_)
        {
            return;
        }
        out_queue = queue_out_;
        reactor = reactor_;
    }
    if (!out_queue || !reactor || data == nullptr || len == 0)
    {
        return;
    }
    const size_t before =
        out_queue->push(bfc::shared_sized_buffer::copy_from(data, len));
    const size_t capacity = out_queue->capacity();
    if (capacity != 0 && before == capacity)
    {
        const uint64_t drops =
            queue_out_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (drops == 1 || (drops & 1023u) == 0)
        {
            LOG_WRN("queue out dropped oldest (drops=%llu)",
                    static_cast<unsigned long long>(drops));
        }
    }
    reactor->wake_up();
}

void QueueEndpoint::on_queue_in()
{
    for (int drained = 0; drained < k_max_datagrams_per_wake; ++drained)
    {
        std::shared_ptr<AppQueue> in_queue;
        {
            std::lock_guard<std::mutex> lock(transport_mu_);
            if (!opened_)
            {
                return;
            }
            in_queue = queue_in_;
        }
        if (!in_queue)
        {
            return;
        }
        auto pkt = in_queue->pop();
        if (!pkt)
        {
            return;
        }
        if (pkt->empty())
        {
            continue;
        }
        accept_app_datagram(pkt->u8(), pkt->size());
    }
    std::shared_ptr<AppQueue> in_queue;
    std::shared_ptr<AppReactor> reactor;
    {
        std::lock_guard<std::mutex> lock(transport_mu_);
        if (!opened_)
        {
            return;
        }
        in_queue = queue_in_;
        reactor = reactor_;
    }
    if (in_queue && reactor && in_queue->has_data())
    {
        reactor->wake_up();
    }
}

}  // namespace winject
