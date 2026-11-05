#include "L3Core.h"

#include "L3Impl.h"
#include "utils/Log.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace winject::l3::detail
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

L3Core::L3Core()
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

L3Core::~L3Core()
{
    for (int fd : stop_pipe)
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
}

void L3Core::attach(winject::core::IOReactor* l3_reactor,
                    winject::l3::detail::L3Impl* l3)
{
    std::lock_guard<std::mutex> lock(mu);
    reactor = l3_reactor;
    impl = l3;
}

void L3Core::detach_and_cancel()
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

bool L3Core::attached() const
{
    std::lock_guard<std::mutex> lock(mu);
    return reactor != nullptr;
}

bool L3Core::on_l3_thread() const
{
    std::lock_guard<std::mutex> lock(mu);
    return reactor != nullptr && reactor->is_reactor_thread();
}

bool L3Core::post_l3(const std::shared_ptr<Request>& req,
                     std::function<void(winject::l3::detail::L3Impl&)> fn)
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
            // l3 detaches on this thread, after its loop: while a
            // wake-up runs, impl is set.
            if (impl != nullptr)
            {
                fn(*impl);
            }
        });
    return true;
}

void L3Core::forget(uint64_t id)
{
    std::lock_guard<std::mutex> lock(mu);
    pending.erase(id);
}

void L3Core::request_stop()
{
    stop_requested = true;
    std::lock_guard<std::mutex> lock(mu);
    if (impl != nullptr)
    {
        impl->stop();
    }
}

void L3Core::request_stop_from_signal()
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

}  // namespace winject::l3::detail
