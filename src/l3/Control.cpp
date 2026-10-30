#include "Control.h"

#include "console/ConsoleParse.h"
#include "ControlNode.h"
#include "mplane/MplaneErrno.h"

#include <atomic>
#include <condition_variable>
#include <stdio.h>
#include <utility>

namespace winject_l3
{

namespace
{

constexpr const char k_esrch[] = "ESRCH";
constexpr const char k_edeadlk[] = "EDEADLK";

using winject::ConsoleCommands;
using winject::ControlOps;
using winject::ControlReply;
using winject::IOReactor;

// One result handed from the node's thread to a waiting caller.
template <typename T>
class Waiter
{
public:
    void set(T v)
    {
        std::lock_guard<std::mutex> lg(mutex);
        if (done)
        {
            return;
        }
        value = std::move(v);
        done = true;
        cv.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout, T* out)
    {
        std::unique_lock<std::mutex> lk(mutex);
        if (!cv.wait_for(lk, timeout,
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
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    T value{};
};

// Calls fn once: with the first result, or with the cancel value when the
// last copy of the request is dropped unanswered (node stopped).
template <typename T>
class Once
{
public:
    Once(std::function<void(const T&)> fn, T cancel)
        : fn(std::move(fn)), cancel(std::move(cancel))
    {
    }

    ~Once()
    {
        call(cancel);
    }

    void call(const T& v)
    {
        if (!called.exchange(true) && fn)
        {
            fn(v);
        }
    }

private:
    std::function<void(const T&)> fn;
    T cancel;
    std::atomic<bool> called{false};
};

RadioReply radio_nok(const char* error)
{
    RadioReply r;
    r.status.error = error;
    return r;
}

std::string command_nok(const char* error)
{
    return std::string("NOK ") + error;
}

// Runs fn on the node's reactor thread: inline when already on it, else
// posted. Returns false (fn dropped) when no node is attached. A posted fn
// that never runs is destroyed with the reactor.
bool run_on_node(const std::shared_ptr<Control::Impl>& d,
                 std::function<void(Control::Impl&)> fn)
{
    IOReactor* reactor = nullptr;
    {
        std::lock_guard<std::mutex> lg(d->mutex);
        reactor = d->reactor;
        if (reactor == nullptr)
        {
            return false;
        }
        if (!reactor->is_reactor_thread())
        {
            reactor->wake_up(
                [d, fn = std::move(fn)]()
                {
                    // Detach only happens on this thread, after the loop.
                    if (d->reactor != nullptr)
                    {
                        fn(*d);
                    }
                });
            return true;
        }
    }
    fn(*d);
    return true;
}

// A synchronous op: op(ops, &value, &error) -> bool.
template <typename T, typename Op>
Result<T> call_sync(const std::shared_ptr<Control::Impl>& d, Op op)
{
    auto waiter = std::make_shared<Waiter<Result<T>>>();
    Result<T> cancel;
    cancel.status.error = winject::k_ecanceled;
    auto once = std::make_shared<Once<Result<T>>>(
        [waiter](const Result<T>& r)
        {
            waiter->set(r);
        },
        cancel);
    const bool posted =
        run_on_node(d,
                    [once, op](Control::Impl& node)
                    {
                        Result<T> r;
                        if (!op(node.ops, &r.value, &r.status.error) &&
                            r.status.error.empty())
                        {
                            r.status.error = winject::k_eio;
                        }
                        once->call(r);
                    });
    if (!posted)
    {
        Result<T> r;
        r.status.error = k_esrch;
        return r;
    }
    once.reset();
    Result<T> out;
    if (!waiter->wait(Control::k_default_timeout, &out))
    {
        out = Result<T>{};
        out.status.error = winject::k_etimedout;
    }
    return out;
}

struct Empty
{
};

// A radio forward: op(ops, reply).
template <typename Op>
void call_radio(const std::shared_ptr<Control::Impl>& d, Op op,
                Control::RadioDone done)
{
    auto once = std::make_shared<Once<RadioReply>>(
        std::move(done), radio_nok(winject::k_ecanceled));
    const bool posted = run_on_node(
        d,
        [once, op](Control::Impl& node)
        {
            ControlReply cr;
            cr.send_text = [once](const std::string& text)
            {
                RadioReply r;
                r.body = text;
                once->call(r);
            };
            cr.send_nok = [once](const char* msg)
            {
                once->call(radio_nok(msg != nullptr ? msg : winject::k_eio));
            };
            op(node.ops, cr);
        });
    if (!posted)
    {
        once->call(radio_nok(k_esrch));
    }
}

template <typename Op>
RadioReply wait_radio(const std::shared_ptr<Control::Impl>& d, Op op,
                      std::chrono::milliseconds timeout)
{
    {
        std::lock_guard<std::mutex> lg(d->mutex);
        if (d->reactor != nullptr && d->reactor->is_reactor_thread())
        {
            return radio_nok(k_edeadlk);
        }
    }
    auto waiter = std::make_shared<Waiter<RadioReply>>();
    call_radio(d, op,
               [waiter](const RadioReply& r)
               {
                   waiter->set(r);
               });
    RadioReply out;
    if (!waiter->wait(timeout, &out))
    {
        return radio_nok(winject::k_etimedout);
    }
    return out;
}

}  // namespace

void Control::Impl::attach(IOReactor* node_reactor, ControlOps node_ops)
{
    std::lock_guard<std::mutex> lg(mutex);
    reactor = node_reactor;
    ops = std::move(node_ops);
    commands = ConsoleCommands(ops);
}

void Control::Impl::detach()
{
    {
        std::lock_guard<std::mutex> lg(mutex);
        reactor = nullptr;
    }
    ops = {};
    commands = {};
}

Control::Control() : d(std::make_shared<Impl>()) {}

bool Control::running() const
{
    std::lock_guard<std::mutex> lg(d->mutex);
    return d->reactor != nullptr;
}

Status Control::add_upstream(const UpstreamView& spec)
{
    return call_sync<Empty>(
               d,
               [spec](const ControlOps& ops, Empty*, std::string* err)
               {
                   return ops.add_upstream(spec, err);
               })
        .status;
}

Status Control::remove_upstream(uint8_t id)
{
    return call_sync<Empty>(
               d,
               [id](const ControlOps& ops, Empty*, std::string* err)
               {
                   return ops.remove_upstream(id, err);
               })
        .status;
}

Result<std::vector<UpstreamView>> Control::list_upstreams(
    const std::vector<uint8_t>& ids)
{
    return call_sync<std::vector<UpstreamView>>(
        d,
        [ids](const ControlOps& ops, std::vector<UpstreamView>* out,
              std::string* err)
        {
            return ops.list_upstream(ids, out, err);
        });
}

Result<UpstreamView> Control::update_upstream(const UpstreamUpdate& patch)
{
    return call_sync<UpstreamView>(
        d,
        [patch](const ControlOps& ops, UpstreamView* out, std::string* err)
        {
            return ops.update_upstream(patch, out, err);
        });
}

Result<std::vector<UpstreamRxStatView>> Control::upstream_rx_stats(
    const std::vector<uint8_t>& ids)
{
    return call_sync<std::vector<UpstreamRxStatView>>(
        d,
        [ids](const ControlOps& ops, std::vector<UpstreamRxStatView>* out,
              std::string* err)
        {
            return ops.list_upstream_rx_stat(ids, out, err);
        });
}

Result<std::vector<UpstreamTxStatView>> Control::upstream_tx_stats(
    const std::vector<uint8_t>& ids)
{
    return call_sync<std::vector<UpstreamTxStatView>>(
        d,
        [ids](const ControlOps& ops, std::vector<UpstreamTxStatView>* out,
              std::string* err)
        {
            return ops.list_upstream_tx_stat(ids, out, err);
        });
}

Result<std::vector<MetricView>> Control::metrics(
    const std::vector<std::string>& keys)
{
    return call_sync<std::vector<MetricView>>(
        d,
        [keys](const ControlOps& ops, std::vector<MetricView>* out,
               std::string* err)
        {
            return ops.get_metrics(keys, out, err);
        });
}

Result<RadioDeviceView> Control::radio_device(const RadioDeviceUpdate& patch)
{
    return call_sync<RadioDeviceView>(
        d,
        [patch](const ControlOps& ops, RadioDeviceView* out, std::string* err)
        {
            return ops.radio_device(patch, out, err);
        });
}

void Control::radio_info(RadioDone done)
{
    call_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_info(cr);
        },
        std::move(done));
}

void Control::radio_caps_info(RadioDone done)
{
    call_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_caps_info(cr);
        },
        std::move(done));
}

void Control::radio_stats(RadioDone done)
{
    call_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_stats(cr);
        },
        std::move(done));
}

void Control::radio_tx(const RadioUpdate& patch, RadioDone done)
{
    call_radio(
        d,
        [patch](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_tx(patch, cr);
        },
        std::move(done));
}

void Control::radio_reset(RadioDone done)
{
    call_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_reset(cr);
        },
        std::move(done));
}

void Control::config_slot(uint8_t slot, RadioDone done)
{
    call_radio(
        d,
        [slot](const ControlOps& ops, ControlReply cr)
        {
            ops.config_slot(slot, cr);
        },
        std::move(done));
}

RadioReply Control::radio_info(std::chrono::milliseconds timeout)
{
    return wait_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_info(cr);
        },
        timeout);
}

RadioReply Control::radio_caps_info(std::chrono::milliseconds timeout)
{
    return wait_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_caps_info(cr);
        },
        timeout);
}

RadioReply Control::radio_stats(std::chrono::milliseconds timeout)
{
    return wait_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_stats(cr);
        },
        timeout);
}

RadioReply Control::radio_tx(const RadioUpdate& patch,
                             std::chrono::milliseconds timeout)
{
    return wait_radio(
        d,
        [patch](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_tx(patch, cr);
        },
        timeout);
}

RadioReply Control::radio_reset(std::chrono::milliseconds timeout)
{
    return wait_radio(
        d,
        [](const ControlOps& ops, ControlReply cr)
        {
            ops.radio_reset(cr);
        },
        timeout);
}

RadioReply Control::config_slot(uint8_t slot, std::chrono::milliseconds timeout)
{
    return wait_radio(
        d,
        [slot](const ControlOps& ops, ControlReply cr)
        {
            ops.config_slot(slot, cr);
        },
        timeout);
}

void Control::command(const std::string& line, CommandDone done)
{
    // As the UDP console: an over-long line is EINVAL, and a blank or
    // comment line gets no reply (here: an empty one).
    if (line.size() >= ConsoleCommands::k_line_max - 1)
    {
        if (done)
        {
            done(command_nok(winject::k_einval));
        }
        return;
    }
    char trimmed[ConsoleCommands::k_line_max];
    snprintf(trimmed, sizeof(trimmed), "%s", line.c_str());
    winject::console_trim_line(trimmed);
    if (trimmed[0] == '\0' || trimmed[0] == '#')
    {
        if (done)
        {
            done(std::string());
        }
        return;
    }
    auto once = std::make_shared<Once<std::string>>(
        std::move(done), command_nok(winject::k_ecanceled));
    const bool posted =
        run_on_node(d,
                    [once, text = std::string(trimmed)](Control::Impl& node)
                    {
                        node.commands.handle(text.c_str(),
                                             [once](const std::string& text)
                                             {
                                                 once->call(text);
                                             });
                    });
    if (!posted)
    {
        once->call(command_nok(k_esrch));
    }
}

std::string Control::command(const std::string& line,
                             std::chrono::milliseconds timeout)
{
    {
        std::lock_guard<std::mutex> lg(d->mutex);
        if (d->reactor != nullptr && d->reactor->is_reactor_thread())
        {
            return command_nok(k_edeadlk);
        }
    }
    auto waiter = std::make_shared<Waiter<std::string>>();
    command(line,
            [waiter](const std::string& text)
            {
                waiter->set(text);
            });
    std::string out;
    if (!waiter->wait(timeout, &out))
    {
        return command_nok(winject::k_etimedout);
    }
    return out;
}

Control control()
{
    static Control instance;
    return instance;
}

}  // namespace winject_l3
