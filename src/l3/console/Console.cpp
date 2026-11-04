// winject_l3::console: execute, format and the UDP Service over the public
// API (include/winject_l3/Console.h).

#include "winject_l3/Console.h"

#include "console/ConsoleCommands.h"
#include "console/ConsoleParse.h"
#include "console/ConsoleService.h"
#include "mplane/MplaneErrno.h"
#include "utils/NetUtil.h"

#include <atomic>
#include <condition_variable>
#include <errno.h>
#include <future>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace winject_l3::console
{

namespace
{

using winject::ConsoleCommands;

std::string nok(const std::string& error)
{
    return "NOK " + error;
}

// The checks the UDP console makes before a line reaches a command: an
// over-long line is EINVAL, a blank or comment line has no command.
enum class LineKind
{
    command,
    empty,
    too_long,
};

LineKind prepare_line(const std::string& line, std::string* text)
{
    if (line.size() >= ConsoleCommands::k_line_max - 1)
    {
        return LineKind::too_long;
    }
    char trimmed[ConsoleCommands::k_line_max];
    snprintf(trimmed, sizeof(trimmed), "%s", line.c_str());
    winject::console_trim_line(trimmed);
    if (trimmed[0] == '\0' || trimmed[0] == '#')
    {
        return LineKind::empty;
    }
    *text = trimmed;
    return LineKind::command;
}

std::string fec_text(const Fec& fec)
{
    char buf[32];
    winject::console_format_fec_display(fec.type, fec.k, fec.n, buf,
                                        sizeof(buf));
    return buf;
}

using ull = unsigned long long;

}  // namespace

void execute(const Node& node, const std::string& line,
             std::function<void(const std::string& reply)> done)
{
    if (!done)
    {
        done = [](const std::string&) {};
    }
    std::string text;
    switch (prepare_line(line, &text))
    {
        case LineKind::too_long:
            done(nok(winject::k_einval));
            return;
        case LineKind::empty:
            return;
        case LineKind::command:
            break;
    }
    const Status posted = node.post(
        [text, done](const Node& n)
        {
            ConsoleCommands::handle(n, text.c_str(), done, false,
                                    k_default_timeout);
        });
    if (!posted.ok())
    {
        done(nok(posted.error));
    }
}

std::string execute(const Node& node, const std::string& line,
                    std::chrono::milliseconds t)
{
    std::string text;
    switch (prepare_line(line, &text))
    {
        case LineKind::too_long:
            return nok(winject::k_einval);
        case LineKind::empty:
            return std::string();
        case LineKind::command:
            break;
    }
    if (!node.running())
    {
        return nok("ESRCH");
    }
    // Every call blocks here, on the caller's thread, so this also works
    // from inside a Done on l3's callback thread.
    std::string out;
    ConsoleCommands::handle(
        node, text.c_str(),
        [&out](const std::string& reply)
        {
            out = reply;
        },
        true, t);
    return out;
}

std::string format(const RadioStats& s)
{
    char buf[512];
    snprintf(
        buf, sizeof(buf),
        "tx_info tx_queue_sz=%llu in_flight=%llu dropped_invalid_frame=%llu "
        "dropped_tx_queue=%llu dropped_wifi=%llu ether_pkt=%llu "
        "air_pkt=%llu ts=%llu\n"
        "rx_info rx_queue_sz=%llu dropped_filter_mismatched=%llu "
        "dropped_rx_queue=%llu dropped_no_peer=%llu "
        "dropped_send_failed=%llu ether_pkt=%llu air_pkt=%llu ts=%llu",
        ull(s.tx.queue_size), ull(s.tx.in_flight),
        ull(s.tx.dropped_invalid_frame), ull(s.tx.dropped_queue),
        ull(s.tx.dropped_wifi), ull(s.tx.ether_packets), ull(s.tx.air_packets),
        ull(s.tx.timestamp_us), ull(s.rx.queue_size),
        ull(s.rx.dropped_filter_mismatched), ull(s.rx.dropped_queue),
        ull(s.rx.dropped_no_peer), ull(s.rx.dropped_send_failed),
        ull(s.rx.ether_packets), ull(s.rx.air_packets), ull(s.rx.timestamp_us));
    return buf;
}

std::string format(uint8_t id, const UpstreamRxStats& s)
{
    char line[512];
    snprintf(line, sizeof(line),
             "upstream_rx_stat id=%u rxbyt=%llu rxpkt=%llu "
             "rx_oversize=%llu rxgap=%llu "
             "fec=%s fec_rec=%llu fec_lost=%llu fec_rxbyt=%llu "
             "fec_rxpkt=%llu fec_rxgap=%llu rx_push_drop=%llu",
             static_cast<unsigned>(id), ull(s.bytes), ull(s.packets),
             ull(s.oversize), ull(s.air_gap), fec_text(s.fec).c_str(),
             ull(s.fec_recovered), ull(s.fec_lost), ull(s.fec_bytes),
             ull(s.fec_packets), ull(s.fec_gap), ull(s.push_drops));
    return line;
}

std::string format(uint8_t id, const UpstreamTxStats& s)
{
    char line[512];
    snprintf(line, sizeof(line),
             "upstream_tx_stat id=%u txbyt=%llu txpkt=%llu fec=%s "
             "fec_txbyt=%llu fec_txpkt=%llu tx_pending_byt=%llu "
             "tx_pending_pkt=%llu",
             static_cast<unsigned>(id), ull(s.bytes), ull(s.packets),
             fec_text(s.fec).c_str(), ull(s.fec_bytes), ull(s.fec_packets),
             ull(s.pending_bytes), ull(s.pending_packets));
    return line;
}

// ---- Service

struct Service::Impl
{
    std::mutex mu;
    std::shared_ptr<Reactor> reactor;  // set while started
    // Touched only on reactor's thread.
    winject::ConsoleService service;
};

namespace
{

// Runs fn on reactor's thread and waits for it: inline when already there.
// False on timeout (the reactor is not running).
bool on_reactor(Reactor& reactor, const std::function<void()>& fn,
                std::chrono::milliseconds timeout)
{
    if (reactor.is_reactor_thread())
    {
        fn();
        return true;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    reactor.wake_up(
        [fn, done]()
        {
            fn();
            done->set_value();
        });
    return finished.wait_for(timeout) == std::future_status::ready;
}

const char* bind_error(int err)
{
    switch (err)
    {
        case EADDRINUSE:
            return "EADDRINUSE";
        case EACCES:
            return "EACCES";
        case EADDRNOTAVAIL:
            return "EADDRNOTAVAIL";
        default:
            return winject::k_eio;
    }
}

Status error(const char* code)
{
    Status s;
    s.error = code;
    return s;
}

}  // namespace

Service::Service() : impl(std::make_shared<Impl>()) {}

Service::~Service()
{
    stop();
}

Status Service::start(const Node& node, const std::string& bind_host_port,
                      const std::string& reactor_name)
{
    sockaddr_in addr{};
    if (!winject::parse_host_port(bind_host_port, &addr))
    {
        return error(winject::k_einval);
    }
    auto reactor = ReactorRegistry::get_instance().get(reactor_name);
    if (!reactor)
    {
        return error(winject::k_enoent);
    }
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->reactor)
        {
            return error(winject::k_ealready);
        }
        impl->reactor = reactor;
    }
    // Shared with the reactor: a start that runs after this gave up must not
    // bind (nobody would stop it) or write to this frame.
    struct Attempt
    {
        std::atomic<bool> cancelled{false};
        bool ok = false;
        int bind_errno = 0;
    };
    auto attempt = std::make_shared<Attempt>();
    auto self = impl;
    const bool ran = on_reactor(
        *reactor,
        [self, reactor, addr, node, attempt]()
        {
            if (attempt->cancelled)
            {
                return;
            }
            std::string err;
            errno = 0;
            attempt->ok = self->service.start(reactor, addr, node, &err);
            attempt->bind_errno = errno;
        },
        k_default_timeout);
    if (!ran)
    {
        attempt->cancelled = true;
    }
    if (!ran || !attempt->ok)
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        impl->reactor.reset();
        return error(!ran ? "ETIMEDOUT" : bind_error(attempt->bind_errno));
    }
    return {};
}

Status Service::stop()
{
    std::shared_ptr<Reactor> reactor;
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        reactor = impl->reactor;
    }
    if (!reactor)
    {
        return {};
    }
    if (reactor->is_reactor_thread())
    {
        return error("EDEADLK");
    }
    auto self = impl;
    if (!on_reactor(
            *reactor,
            [self]()
            {
                self->service.stop();
            },
            k_default_timeout))
    {
        return error("ETIMEDOUT");
    }
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->reactor.reset();
    return {};
}

}  // namespace winject_l3::console
