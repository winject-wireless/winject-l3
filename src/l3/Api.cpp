// The public handles: each call posts to the node's thread (NodeCore.h).

#include "NodeCore.h"
#include "NodeImpl.h"
#include "mplane/MplaneErrno.h"
#include "winject_l3/Node.h"

#include <utility>

namespace winject_l3
{

using detail::call;
using detail::call_async;
using detail::call_async_wait;
using detail::call_status;
using detail::error_result;
using detail::HandleAccess;
using detail::value_result;
using winject::NodeImpl;

namespace
{

using Empty = NodeImpl::Empty;

Status to_status(const Result<Empty>& r)
{
    return r.status;
}

StatusDone status_done(StatusDone done)
{
    return done ? done : [](Status) {};
}

}  // namespace

// ---- Node

bool Node::running() const
{
    return core && core->attached();
}

UpstreamManager Node::upstreams() const
{
    return HandleAccess::upstreams(core);
}

RadioManager Node::radios() const
{
    return HandleAccess::radios(core);
}

Result<std::vector<Metric>> Node::metrics(
    const std::vector<std::string>& keys) const
{
    return call<std::vector<Metric>>(core,
                                     [keys](NodeImpl& node)
                                     {
                                         return node.metrics(keys);
                                     });
}

Status Node::post(std::function<void(const Node&)> fn) const
{
    if (!core || !core->attached())
    {
        return detail::error_status(detail::k_esrch);
    }
    auto shared = core;
    if (!core->callbacks.push(
            [shared, fn = std::move(fn)]()
            {
                if (fn)
                {
                    fn(HandleAccess::node(shared));
                }
            }))
    {
        return detail::error_status(detail::k_esrch);
    }
    return {};
}

// ---- UpstreamManager

Result<Upstream> UpstreamManager::add(const UpstreamSpec& spec)
{
    const Status s = call_status(core,
                                 [spec](NodeImpl& node)
                                 {
                                     return node.add_upstream(spec);
                                 });
    if (!s.ok())
    {
        Result<Upstream> r;
        r.status = s;
        return r;
    }
    return value_result(HandleAccess::upstream(core, spec.id));
}

Result<Upstream> UpstreamManager::get(uint8_t id) const
{
    auto shared = core;
    return call<Upstream>(
        core,
        [shared, id](NodeImpl& node)
        {
            if (!node.has_upstream(id))
            {
                return error_result<Upstream>(winject::k_enoent);
            }
            return value_result(HandleAccess::upstream(shared, id));
        });
}

Status UpstreamManager::remove(uint8_t id)
{
    return call_status(core,
                       [id](NodeImpl& node)
                       {
                           return node.remove_upstream(id);
                       });
}

Result<std::vector<Upstream>> UpstreamManager::list() const
{
    auto shared = core;
    return call<std::vector<Upstream>>(
        core,
        [shared](NodeImpl& node)
        {
            std::vector<Upstream> out;
            for (uint8_t id : node.upstream_ids())
            {
                out.push_back(HandleAccess::upstream(shared, id));
            }
            return value_result(std::move(out));
        });
}

// ---- Upstream

bool Upstream::valid() const
{
    const uint8_t id = upstream_id;
    return call<bool>(core,
                      [id](NodeImpl& node)
                      {
                          return value_result(node.has_upstream(id));
                      })
        .value;
}

Result<UpstreamSpec> Upstream::spec() const
{
    const uint8_t id = upstream_id;
    return call<UpstreamSpec>(core,
                              [id](NodeImpl& node)
                              {
                                  return node.upstream_spec(id);
                              });
}

Result<Fec> Upstream::fec() const
{
    const auto s = spec();
    if (!s.ok())
    {
        return error_result<Fec>(s.status.error);
    }
    return value_result(s.value.fec);
}

Status Upstream::set_fec(const Fec& fec)
{
    UpstreamPatch patch;
    patch.fec = fec;
    return update(patch).status;
}

Result<size_t> Upstream::quanta() const
{
    const auto s = spec();
    if (!s.ok())
    {
        return error_result<size_t>(s.status.error);
    }
    return value_result(s.value.quanta);
}

Status Upstream::set_quanta(size_t quanta)
{
    UpstreamPatch patch;
    patch.quanta = quanta;
    return update(patch).status;
}

Result<UpstreamSpec> Upstream::update(const UpstreamPatch& patch)
{
    const uint8_t id = upstream_id;
    return call<UpstreamSpec>(core,
                              [id, patch](NodeImpl& node)
                              {
                                  return node.update_upstream(id, patch);
                              });
}

Result<UpstreamRxStats> Upstream::rx_stats() const
{
    const uint8_t id = upstream_id;
    return call<UpstreamRxStats>(core,
                                 [id](NodeImpl& node)
                                 {
                                     return node.upstream_rx_stats(id);
                                 });
}

Result<UpstreamTxStats> Upstream::tx_stats() const
{
    const uint8_t id = upstream_id;
    return call<UpstreamTxStats>(core,
                                 [id](NodeImpl& node)
                                 {
                                     return node.upstream_tx_stats(id);
                                 });
}

// ---- RadioManager

Result<Radio> RadioManager::create(const RadioSpec& spec)
{
    const Status s = call_status(core,
                                 [spec](NodeImpl& node)
                                 {
                                     return node.create_radio(spec);
                                 });
    if (!s.ok())
    {
        Result<Radio> r;
        r.status = s;
        return r;
    }
    return value_result(HandleAccess::radio(core));
}

Status RadioManager::add(const Radio& radio)
{
    if (HandleAccess::core(radio) != core)
    {
        return detail::error_status(winject::k_einval);
    }
    return call_status(core,
                       [](NodeImpl& node)
                       {
                           return node.add_radio();
                       });
}

Result<Radio> RadioManager::get() const
{
    auto shared = core;
    return call<Radio>(core,
                       [shared](NodeImpl& node)
                       {
                           if (!node.has_radio())
                           {
                               return error_result<Radio>(winject::k_enoent);
                           }
                           return value_result(HandleAccess::radio(shared));
                       });
}

Status RadioManager::remove()
{
    return call_status(core,
                       [](NodeImpl& node)
                       {
                           return node.remove_radio();
                       });
}

// ---- Radio

bool Radio::valid() const
{
    return call<bool>(core,
                      [](NodeImpl& node)
                      {
                          return value_result(node.has_radio());
                      })
        .value;
}

Result<RadioState> Radio::state() const
{
    return call<RadioState>(core,
                            [](NodeImpl& node)
                            {
                                return node.radio_state();
                            });
}

Result<RadioSpec> Radio::spec() const
{
    return call<RadioSpec>(core,
                           [](NodeImpl& node)
                           {
                               return node.radio_spec();
                           });
}

Result<uint16_t> Radio::domain() const
{
    const auto s = spec();
    if (!s.ok())
    {
        return error_result<uint16_t>(s.status.error);
    }
    return value_result(s.value.domain);
}

Result<RadioDevice> Radio::device() const
{
    return call<RadioDevice>(core,
                             [](NodeImpl& node)
                             {
                                 return node.radio_device();
                             });
}

Status Radio::set_device(const RadioDevice& d)
{
    return call_status(core,
                       [d](NodeImpl& node)
                       {
                           return node.set_radio_device(d);
                       });
}

Result<RadioPhy> Radio::phy() const
{
    const auto s = spec();
    if (!s.ok())
    {
        return error_result<RadioPhy>(s.status.error);
    }
    return value_result(s.value.phy);
}

void Radio::set_phy(const RadioPhyPatch& p, Done<RadioInfo> done)
{
    call_async<RadioInfo>(
        core,
        [p](NodeImpl& node, std::function<void(Result<RadioInfo>)> reply)
        {
            node.set_radio_phy(p, std::move(reply));
        },
        std::move(done));
}

Result<RadioInfo> Radio::set_phy(const RadioPhyPatch& p,
                                 std::chrono::milliseconds t)
{
    return call_async_wait<RadioInfo>(
        core,
        [p](NodeImpl& node, std::function<void(Result<RadioInfo>)> reply)
        {
            node.set_radio_phy(p, std::move(reply));
        },
        t);
}

Result<RadioInfo> Radio::set_channel(uint8_t ch)
{
    RadioPhyPatch p;
    p.channel = ch;
    return set_phy(p);
}

Result<RadioInfo> Radio::set_modulation(const std::string& m)
{
    RadioPhyPatch p;
    p.modulation = m;
    return set_phy(p);
}

Result<RadioInfo> Radio::set_power(int8_t dbm)
{
    RadioPhyPatch p;
    p.power_dbm = dbm;
    return set_phy(p);
}

Result<RadioInfo> Radio::set_cca(bool on)
{
    RadioPhyPatch p;
    p.cca = on;
    return set_phy(p);
}

void Radio::info(Done<RadioInfo> done)
{
    call_async<RadioInfo>(
        core,
        [](NodeImpl& node, std::function<void(Result<RadioInfo>)> reply)
        {
            node.radio_info(std::move(reply));
        },
        std::move(done));
}

Result<RadioInfo> Radio::info(std::chrono::milliseconds t)
{
    return call_async_wait<RadioInfo>(
        core,
        [](NodeImpl& node, std::function<void(Result<RadioInfo>)> reply)
        {
            node.radio_info(std::move(reply));
        },
        t);
}

void Radio::caps(Done<RadioCaps> done)
{
    call_async<RadioCaps>(
        core,
        [](NodeImpl& node, std::function<void(Result<RadioCaps>)> reply)
        {
            node.radio_caps(std::move(reply));
        },
        std::move(done));
}

Result<RadioCaps> Radio::caps(std::chrono::milliseconds t)
{
    return call_async_wait<RadioCaps>(
        core,
        [](NodeImpl& node, std::function<void(Result<RadioCaps>)> reply)
        {
            node.radio_caps(std::move(reply));
        },
        t);
}

void Radio::stats(Done<RadioStats> done)
{
    call_async<RadioStats>(
        core,
        [](NodeImpl& node, std::function<void(Result<RadioStats>)> reply)
        {
            node.radio_stats(std::move(reply));
        },
        std::move(done));
}

Result<RadioStats> Radio::stats(std::chrono::milliseconds t)
{
    return call_async_wait<RadioStats>(
        core,
        [](NodeImpl& node, std::function<void(Result<RadioStats>)> reply)
        {
            node.radio_stats(std::move(reply));
        },
        t);
}

void Radio::set_config_slot(uint8_t slot, StatusDone done)
{
    auto finish = status_done(std::move(done));
    call_async<Empty>(
        core,
        [slot](NodeImpl& node, std::function<void(Result<Empty>)> reply)
        {
            node.set_config_slot(slot, std::move(reply));
        },
        [finish](Result<Empty> r)
        {
            finish(to_status(r));
        });
}

Status Radio::set_config_slot(uint8_t slot, std::chrono::milliseconds t)
{
    return to_status(call_async_wait<Empty>(
        core,
        [slot](NodeImpl& node, std::function<void(Result<Empty>)> reply)
        {
            node.set_config_slot(slot, std::move(reply));
        },
        t));
}

void Radio::reset(StatusDone done)
{
    auto finish = status_done(std::move(done));
    call_async<Empty>(
        core,
        [](NodeImpl& node, std::function<void(Result<Empty>)> reply)
        {
            node.radio_reset(std::move(reply));
        },
        [finish](Result<Empty> r)
        {
            finish(to_status(r));
        });
}

Status Radio::reset(std::chrono::milliseconds t)
{
    return to_status(call_async_wait<Empty>(
        core,
        [](NodeImpl& node, std::function<void(Result<Empty>)> reply)
        {
            node.radio_reset(std::move(reply));
        },
        t));
}

void Radio::mplane(const std::string& line, Done<std::string> done)
{
    call_async<std::string>(
        core,
        [line](NodeImpl& node, std::function<void(Result<std::string>)> reply)
        {
            node.radio_mplane(line, std::move(reply));
        },
        std::move(done));
}

Result<std::string> Radio::mplane(const std::string& line,
                                  std::chrono::milliseconds t)
{
    return call_async_wait<std::string>(
        core,
        [line](NodeImpl& node, std::function<void(Result<std::string>)> reply)
        {
            node.radio_mplane(line, std::move(reply));
        },
        t);
}

}  // namespace winject_l3
