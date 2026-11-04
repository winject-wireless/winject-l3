#include "NodeCore.h"

#include "NodeImpl.h"
#include "utils/Log.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace winject_l3::detail
{

namespace
{
thread_local bool t_on_callback_thread = false;
}  // namespace

void CallbackQueue::open()
{
    std::lock_guard<std::mutex> lock(mu);
    opened = true;
}

void CallbackQueue::close()
{
    std::lock_guard<std::mutex> lock(mu);
    opened = false;
    cv.notify_all();
}

bool CallbackQueue::push(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return false;
    }
    queue.push_back(std::move(fn));
    cv.notify_one();
    return true;
}

void CallbackQueue::run()
{
    const bool was = t_on_callback_thread;
    t_on_callback_thread = true;
    for (;;)
    {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock,
                    [this]
                    {
                        return !queue.empty() || !opened;
                    });
            if (queue.empty())
            {
                break;
            }
            fn = std::move(queue.front());
            queue.pop_front();
        }
        fn();
    }
    t_on_callback_thread = was;
}

bool CallbackQueue::on_callback_thread()
{
    return t_on_callback_thread;
}

NodeCore::NodeCore()
{
#if !defined(ESP_PLATFORM)
    if (pipe(stop_pipe) == 0)
    {
        for (int fd : stop_pipe)
        {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    else
    {
        stop_pipe[0] = stop_pipe[1] = -1;
    }
#endif
}

NodeCore::~NodeCore()
{
    for (int fd : stop_pipe)
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
}

void NodeCore::attach(winject::IOReactor* node_reactor, winject::NodeImpl* node)
{
    std::lock_guard<std::mutex> lock(mu);
    reactor = node_reactor;
    impl = node;
}

void NodeCore::detach_and_cancel()
{
    std::vector<std::shared_ptr<Request>> live;
    {
        std::lock_guard<std::mutex> lock(mu);
        reactor = nullptr;
        impl = nullptr;
        live.reserve(pending.size());
        for (auto& entry : pending)
        {
            live.push_back(std::move(entry.second));
        }
        pending.clear();
    }
    for (auto& req : live)
    {
        req->cancel();
    }
}

bool NodeCore::attached() const
{
    std::lock_guard<std::mutex> lock(mu);
    return reactor != nullptr;
}

bool NodeCore::on_node_thread() const
{
    std::lock_guard<std::mutex> lock(mu);
    return reactor != nullptr && reactor->is_reactor_thread();
}

bool NodeCore::post_node(const std::shared_ptr<Request>& req,
                         std::function<void(winject::NodeImpl&)> fn)
{
    std::lock_guard<std::mutex> lock(mu);
    if (reactor == nullptr)
    {
        return false;
    }
    req->id = next_id++;
    pending.emplace(req->id, req);
    reactor->wake_up(
        [this, fn = std::move(fn)]()
        {
            // The node detaches on this thread, after its loop: while a
            // wake-up runs, impl is set.
            if (impl != nullptr)
            {
                fn(*impl);
            }
        });
    return true;
}

void NodeCore::forget(uint64_t id)
{
    std::lock_guard<std::mutex> lock(mu);
    pending.erase(id);
}

void NodeCore::request_stop()
{
    stop_requested = true;
    std::lock_guard<std::mutex> lock(mu);
    if (impl != nullptr)
    {
        impl->stop();
    }
}

void NodeCore::request_stop_from_signal()
{
    stop_requested = true;
    if (stop_pipe[1] < 0)
    {
        // No signals here (ESP-IDF): an ordinary stop.
        request_stop();
        return;
    }
    const char byte = 1;
    const ssize_t n = ::write(stop_pipe[1], &byte, 1);
    (void)n;
}

}  // namespace winject_l3::detail
