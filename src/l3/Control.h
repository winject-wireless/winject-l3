#ifndef WINJECT_L3_CONTROL_H_
#define WINJECT_L3_CONTROL_H_

#include "ControlTypes.h"

#include <chrono>
#include <functional>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

namespace winject_l3
{

using winject::MetricView;
using winject::RadioDeviceUpdate;
using winject::RadioDeviceView;
using winject::RadioUpdate;
using winject::UpstreamRxStatView;
using winject::UpstreamTxStatView;
using winject::UpstreamUpdate;
using winject::UpstreamView;

// Outcome of a Control call: ok, or an errno name as the m-plane replies it
// (docs/mplane.md § Reply conventions). Control adds ESRCH (no node is
// running), EDEADLK (a blocking call from the node's own thread) and
// ETIMEDOUT (no answer within the timeout).
struct Status
{
    std::string error;

    bool ok() const
    {
        return error.empty();
    }
};

template <typename T>
struct Result
{
    Status status;
    T value{};

    bool ok() const
    {
        return status.ok();
    }
};

// A command forwarded to the radio m-plane. body is the radio's reply
// (key=value text, as radio_info / radio_stats print it).
struct RadioReply
{
    Status status;
    std::string body;

    bool ok() const
    {
        return status.ok();
    }
};

// The manager m-plane as function calls: every console command
// (docs/mplane.md) has a typed call here. A handle is cheap to copy; all
// copies reach the same node. Calls are safe from any thread: they run on
// the node's reactor thread and the caller waits for the result (radio
// forwards also have a callback form). Use winject_l3::control() for the
// node winject_l3::main runs.
class Control
{
public:
    using RadioDone = std::function<void(const RadioReply& reply)>;
    using CommandDone = std::function<void(const std::string& text)>;

    // Radio forwards wait for the radio m-plane's own deadline; this only
    // bounds a wedged node.
    static constexpr std::chrono::milliseconds k_default_timeout{5000};

    // A handle no node is attached to yet: calls return ESRCH.
    Control();

    // A node is attached and its reactor is (or is about to be) running.
    bool running() const;

    // Upstreams (add_upstream, remove_upstream, list_upstream,
    // update_upstream, list_upstream_rx_stat, list_upstream_tx_stat). An
    // empty id list means every upstream.
    Status add_upstream(const UpstreamView& spec);
    Status remove_upstream(uint8_t id);
    Result<std::vector<UpstreamView>> list_upstreams(
        const std::vector<uint8_t>& ids = {});
    Result<UpstreamView> update_upstream(const UpstreamUpdate& patch);
    Result<std::vector<UpstreamRxStatView>> upstream_rx_stats(
        const std::vector<uint8_t>& ids = {});
    Result<std::vector<UpstreamTxStatView>> upstream_tx_stats(
        const std::vector<uint8_t>& ids = {});

    // get_metrics; no keys means all.
    Result<std::vector<MetricView>> metrics(
        const std::vector<std::string>& keys = {});

    // radio_device: the manager's radio addresses and FCS mode. An update
    // with no have_* field reads them.
    Result<RadioDeviceView> radio_device(const RadioDeviceUpdate& patch = {});

    // Forwarded to the radio (radio_info, radio_caps_info, radio_stats,
    // radio_tx, reset, config). done runs exactly once: on the node's
    // thread, or with ECANCELED on whichever thread drops the request when
    // the node stops first.
    void radio_info(RadioDone done);
    void radio_caps_info(RadioDone done);
    void radio_stats(RadioDone done);
    void radio_tx(const RadioUpdate& patch, RadioDone done);
    void radio_reset(RadioDone done);
    void config_slot(uint8_t slot, RadioDone done);

    // Blocking forms of the radio forwards.
    RadioReply radio_info(
        std::chrono::milliseconds timeout = k_default_timeout);
    RadioReply radio_caps_info(
        std::chrono::milliseconds timeout = k_default_timeout);
    RadioReply radio_stats(
        std::chrono::milliseconds timeout = k_default_timeout);
    RadioReply radio_tx(const RadioUpdate& patch,
                        std::chrono::milliseconds timeout = k_default_timeout);
    RadioReply radio_reset(
        std::chrono::milliseconds timeout = k_default_timeout);
    RadioReply config_slot(
        uint8_t slot, std::chrono::milliseconds timeout = k_default_timeout);

    // One m-plane text line (docs/mplane.md, without the cmd:<u8> tag),
    // answered as the UDP console would: "OK ...", "NOK <errno>" or a
    // listing. For a host console that serves l3 next to other apps. With no
    // node the reply is "NOK ESRCH".
    void command(const std::string& line, CommandDone done);
    std::string command(const std::string& line,
                        std::chrono::milliseconds timeout = k_default_timeout);

    // The node side (App): binds a reactor and its ops, then detaches on
    // shutdown. Not for hosts.
    struct Impl;
    const std::shared_ptr<Impl>& impl() const
    {
        return d;
    }

private:
    std::shared_ptr<Impl> d;
};

// The process's node: the one winject_l3::main runs. Valid before main()
// starts (calls return ESRCH until the node is up) and after it returns.
Control control();

}  // namespace winject_l3

#endif  // WINJECT_L3_CONTROL_H_
