#ifndef WINJECT_L3_NODE_CORE_H_
#define WINJECT_L3_NODE_CORE_H_

#include "utils/IOReactor.h"
#include "winject_l3/Node.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <unordered_map>
#include <utility>

namespace winject
{
class NodeImpl;
}

namespace winject_l3::detail
{

inline constexpr const char k_esrch[] = "ESRCH";
inline constexpr const char k_etimedout[] = "ETIMEDOUT";
inline constexpr const char k_ecanceled[] = "ECANCELED";
inline constexpr const char k_edeadlk[] = "EDEADLK";

inline Status error_status(const char* error)
{
    Status s;
    s.error = error;
    return s;
}

template <class T>
Result<T> error_result(const std::string& error)
{
    Result<T> r;
    r.status.error = error;
    return r;
}

template <class T>
Result<T> value_result(T value)
{
    Result<T> r;
    r.value = std::move(value);
    return r;
}

// l3's callback thread: run()'s calling thread runs every Done and every
// Node::post here, in order.
class CallbackQueue
{
public:
    void open();
    // Lets run() return once the queue is empty.
    void close();
    // False when closed: the caller runs fn itself.
    bool push(std::function<void()> fn);
    // Runs callbacks until close() and the queue is empty.
    void run();
    static bool on_callback_thread();

private:
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    bool opened = false;
};

// One call in flight. The node keeps every one until it finishes, so a node
// that stops can cancel them: a bfc reactor never runs wake-ups queued after
// its loop exits.
struct Request
{
    virtual ~Request() = default;
    // Finishes with ECANCELED, once.
    virtual void cancel() = 0;
    // The blocking caller gave up (ETIMEDOUT). The node skips the request if
    // it has not started.
    std::atomic<bool> abandoned{false};
    uint64_t id = 0;
};

// The state every handle shares: the attached node, the pending requests and
// the callback queue. Outlives the node it attaches.
struct NodeCore
{
    NodeCore();
    ~NodeCore();
    NodeCore(const NodeCore&) = delete;
    NodeCore& operator=(const NodeCore&) = delete;

    static std::shared_ptr<NodeCore> create()
    {
        return std::make_shared<NodeCore>();
    }

    // The node's side, from NodeImpl. attach before the node's thread
    // starts; detach_and_cancel on the node's thread once its loop exited.
    void attach(winject::IOReactor* node_reactor, winject::NodeImpl* node);
    void detach_and_cancel();
    // Only valid on the node's thread.
    winject::NodeImpl* node_impl() const
    {
        return impl;
    }

    bool attached() const;
    bool on_node_thread() const;
    // Queues fn for the node's thread and keeps req pending until forget().
    // False when no node is attached (fn dropped).
    bool post_node(const std::shared_ptr<Request>& req,
                   std::function<void(winject::NodeImpl&)> fn);
    void forget(uint64_t id);

    void request_stop();
    void request_stop_from_signal();
    // Linux: read end of the pipe stop_from_signal writes, -1 elsewhere.
    int stop_fd() const
    {
        return stop_pipe[0];
    }

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> busy{false};
    CallbackQueue callbacks;

private:
    mutable std::mutex mu;
    winject::IOReactor* reactor = nullptr;
    winject::NodeImpl* impl = nullptr;
    std::unordered_map<uint64_t, std::shared_ptr<Request>> pending;
    uint64_t next_id = 1;
    int stop_pipe[2] = {-1, -1};
};

// Builds handles; the handles only have default constructors in public.
struct HandleAccess
{
    static Node node(std::shared_ptr<NodeCore> core)
    {
        Node n;
        n.core = std::move(core);
        return n;
    }
    static UpstreamManager upstreams(std::shared_ptr<NodeCore> core)
    {
        UpstreamManager m;
        m.core = std::move(core);
        return m;
    }
    static RadioManager radios(std::shared_ptr<NodeCore> core)
    {
        RadioManager m;
        m.core = std::move(core);
        return m;
    }
    static Upstream upstream(std::shared_ptr<NodeCore> core, uint8_t id)
    {
        Upstream u;
        u.core = std::move(core);
        u.upstream_id = id;
        return u;
    }
    static Radio radio(std::shared_ptr<NodeCore> core)
    {
        Radio r;
        r.core = std::move(core);
        return r;
    }
    template <class H>
    static const std::shared_ptr<NodeCore>& core(const H& handle)
    {
        return handle.core;
    }
};

template <class T>
struct SyncRequest : Request
{
    void set(Result<T> r)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (done)
        {
            return;
        }
        done = true;
        value = std::move(r);
        cv.notify_all();
    }
    void cancel() override
    {
        set(error_result<T>(k_ecanceled));
    }
    bool wait(std::chrono::milliseconds timeout, Result<T>* out)
    {
        std::unique_lock<std::mutex> lock(mu);
        if (!cv.wait_for(lock, timeout,
                         [this]
                         {
                             return done;
                         }))
        {
            return false;
        }
        *out = std::move(value);
        return true;
    }

private:
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    Result<T> value;
};

// Calls fn on the node's thread and waits for its result.
template <class T>
Result<T> call(const std::shared_ptr<NodeCore>& core,
               std::function<Result<T>(winject::NodeImpl&)> fn,
               std::chrono::milliseconds timeout = k_default_timeout)
{
    if (!core)
    {
        return error_result<T>(k_esrch);
    }
    if (core->on_node_thread())
    {
        return error_result<T>(k_edeadlk);
    }
    auto req = std::make_shared<SyncRequest<T>>();
    NodeCore* raw = core.get();
    const bool posted =
        core->post_node(req,
                        [raw, req, fn = std::move(fn)](winject::NodeImpl& node)
                        {
                            if (!req->abandoned)
                            {
                                req->set(fn(node));
                            }
                            raw->forget(req->id);
                        });
    if (!posted)
    {
        return error_result<T>(k_esrch);
    }
    Result<T> out;
    if (!req->wait(timeout, &out))
    {
        req->abandoned = true;
        return error_result<T>(k_etimedout);
    }
    return out;
}

inline Status call_status(const std::shared_ptr<NodeCore>& core,
                          std::function<Status(winject::NodeImpl&)> fn,
                          std::chrono::milliseconds timeout = k_default_timeout)
{
    struct None
    {
    };
    return call<None>(
               core,
               [fn = std::move(fn)](winject::NodeImpl& node)
               {
                   Result<None> r;
                   r.status = fn(node);
                   return r;
               },
               timeout)
        .status;
}

template <class T>
struct AsyncRequest : Request
{
    std::shared_ptr<NodeCore> core;
    std::function<void(Result<T>)> deliver;
    std::atomic<bool> finished{false};

    void finish(Result<T> r)
    {
        if (finished.exchange(true))
        {
            return;
        }
        core->forget(id);
        auto out = std::move(deliver);
        deliver = nullptr;
        core.reset();
        if (out)
        {
            out(std::move(r));
        }
    }
    void cancel() override
    {
        finish(error_result<T>(k_ecanceled));
    }
};

// Finishes the request with ECANCELED if the node drops its reply unanswered.
template <class T>
struct ReplyGuard
{
    std::shared_ptr<AsyncRequest<T>> req;
    ~ReplyGuard()
    {
        req->finish(error_result<T>(k_ecanceled));
    }
};

// start(node, reply) runs on the node's thread and calls reply exactly once,
// later or at once. deliver gets the result on the node's thread, or with
// ECANCELED wherever the node cancels it. False: no node.
template <class T>
bool start_async(
    const std::shared_ptr<NodeCore>& core,
    std::function<void(winject::NodeImpl&, std::function<void(Result<T>)>)>
        start,
    std::function<void(Result<T>)> deliver,
    std::shared_ptr<AsyncRequest<T>>* out_req = nullptr)
{
    auto req = std::make_shared<AsyncRequest<T>>();
    req->core = core;
    req->deliver = std::move(deliver);
    if (out_req != nullptr)
    {
        *out_req = req;
    }
    const bool posted =
        core->post_node(req,
                        [req, start = std::move(start)](winject::NodeImpl& node)
                        {
                            if (req->abandoned)
                            {
                                req->cancel();
                                return;
                            }
                            auto guard = std::make_shared<ReplyGuard<T>>();
                            guard->req = req;
                            start(node,
                                  [guard](Result<T> r)
                                  {
                                      guard->req->finish(std::move(r));
                                  });
                        });
    if (!posted)
    {
        req->deliver = nullptr;
        req->core.reset();
        req->finished = true;
    }
    return posted;
}

// Callback form: done runs on l3's callback thread; ESRCH at once without a
// node.
template <class T>
void call_async(
    const std::shared_ptr<NodeCore>& core,
    std::function<void(winject::NodeImpl&, std::function<void(Result<T>)>)>
        start,
    Done<T> done)
{
    if (!done)
    {
        done = [](Result<T>) {};
    }
    if (!core)
    {
        done(error_result<T>(k_esrch));
        return;
    }
    auto deliver = [core, done](Result<T> r)
    {
        auto run = [done, r]()
        {
            done(r);
        };
        if (!core->callbacks.push(run))
        {
            run();
        }
    };
    if (!start_async<T>(core, std::move(start), std::move(deliver)))
    {
        done(error_result<T>(k_esrch));
    }
}

// Blocking form: the node's thread wakes the caller directly, never through
// the callback thread, so it works from inside a Done.
template <class T>
Result<T> call_async_wait(
    const std::shared_ptr<NodeCore>& core,
    std::function<void(winject::NodeImpl&, std::function<void(Result<T>)>)>
        start,
    std::chrono::milliseconds timeout)
{
    if (!core)
    {
        return error_result<T>(k_esrch);
    }
    if (core->on_node_thread())
    {
        return error_result<T>(k_edeadlk);
    }
    auto waiter = std::make_shared<SyncRequest<T>>();
    std::shared_ptr<AsyncRequest<T>> req;
    if (!start_async<T>(
            core, std::move(start),
            [waiter](Result<T> r)
            {
                waiter->set(std::move(r));
            },
            &req))
    {
        return error_result<T>(k_esrch);
    }
    Result<T> out;
    if (!waiter->wait(timeout, &out))
    {
        req->abandoned = true;
        return error_result<T>(k_etimedout);
    }
    return out;
}

// run() for a given core; winject_l3::run uses the process's core.
int run_node(const std::shared_ptr<NodeCore>& core, const Config& cfg,
             std::function<void(const Node&)> started,
             ThreadSetup thread_setup);

}  // namespace winject_l3::detail

#endif  // WINJECT_L3_NODE_CORE_H_
