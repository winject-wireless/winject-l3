#include "NodeCore.h"
#include "NodeImpl.h"
#include "WinjectL3.h"
#include "mplane/MplaneErrno.h"
#include "utils/Log.h"
#include "utils/Version.h"

#include <memory>
#include <string>

namespace winject_l3
{

namespace
{

const std::shared_ptr<detail::NodeCore>& process_core()
{
    static const std::shared_ptr<detail::NodeCore> core =
        detail::NodeCore::create();
    return core;
}

RadioFcs api_fcs(winject::RadioFcsConfig fcs)
{
    switch (fcs)
    {
        case winject::RadioFcsConfig::signal:
            return RadioFcs::signal;
        case winject::RadioFcsConfig::actual:
            return RadioFcs::actual;
        case winject::RadioFcsConfig::auto_detect:
        default:
            return RadioFcs::auto_detect;
    }
}

RadioSpec radio_spec_of(const Config& cfg)
{
    RadioSpec spec;
    const winject::RadioDeviceConfig& d = cfg.radio_device;
    spec.device.mplane = d.mplane;
    spec.device.dplane = d.dplane;
    spec.device.dplane_tx = d.dplane_tx;
    spec.device.dplane_rx = d.dplane_rx;
    spec.device.reactor = d.reactor;
    spec.device.fcs = api_fcs(d.fcs);
    spec.phy.channel = cfg.channel;
    spec.phy.modulation = cfg.modulation;
    spec.phy.power_dbm = cfg.power_dbm;
    if (cfg.cca_explicit)
    {
        spec.phy.cca = cfg.cca;
    }
    spec.config_slot = cfg.config_slot;
    spec.domain = cfg.domain;
    return spec;
}

UpstreamSpec upstream_spec_of(const Config& cfg,
                              const winject::UpstreamConfig& u)
{
    UpstreamSpec spec;
    spec.id = static_cast<uint8_t>(u.index);
    spec.bus_tx = u.bus_tx;
    spec.bus_rx = u.bus_rx;
    spec.transport = u.mode == winject::UpstreamMode::queue ? Transport::queue
                                                            : Transport::udp;
    spec.rx = u.endpoint.rx;
    spec.tx = u.endpoint.tx;
    spec.reactor = u.reactor;
    spec.fec.type = u.fec_type == winject::FecType::RsBlockErasure
                        ? FecType::rs_block
                        : FecType::none;
    spec.fec.k = u.fec_k;
    spec.fec.n = u.fec_n;
    spec.fec.timeout = std::chrono::milliseconds(u.fec_timeout_ms);
    spec.quanta = u.scheduler_budget;
    spec.rx_queue_capacity = u.rx_queue_capacity;
    if (cfg.on_upstream_queue)
    {
        const size_t index = u.index;
        auto publish = cfg.on_upstream_queue;
        spec.on_queue =
            [publish, index](std::shared_ptr<winject::UpstreamQueue> q)
        {
            publish(index, std::move(q));
        };
    }
    return spec;
}

}  // namespace

Version version()
{
    const winject::WinjectVersion v = winject::own_version();
    Version out;
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;
    return out;
}

Node node()
{
    return detail::HandleAccess::node(process_core());
}

Status apply(const Node& node, const Config& cfg)
{
    std::string err;
    if (!cfg.validate(&err))
    {
        LOG_ERR("%s", err.c_str());
        return detail::error_status(winject::k_einval);
    }
    RadioManager radios = node.radios();
    const Result<Radio> radio = radios.create(radio_spec_of(cfg));
    if (!radio.ok())
    {
        LOG_ERR("radio: %s", radio.status.error.c_str());
        return radio.status;
    }
    UpstreamManager upstreams = node.upstreams();
    for (const winject::UpstreamConfig& u : cfg.upstreams)
    {
        const Result<Upstream> up = upstreams.add(upstream_spec_of(cfg, u));
        if (!up.ok())
        {
            LOG_ERR("upstream-%zu: %s", u.index, up.status.error.c_str());
            return up.status;
        }
    }
    const Status added = radios.add(radio.value);
    if (!added.ok())
    {
        LOG_ERR("radio: %s", added.error.c_str());
    }
    return added;
}

int detail::run_node(const std::shared_ptr<NodeCore>& core, const Config& cfg,
                     std::function<void(const Node&)> started,
                     ThreadSetup thread_setup)
{
    std::string err;
    if (!cfg.validate(&err))
    {
        LOG_ERR("%s", err.c_str());
        return 1;
    }
    bool idle = false;
    if (!core->busy.compare_exchange_strong(idle, true))
    {
        LOG_ERR("winject-l3: this node is already running");
        return 1;
    }
    int rc = 0;
    {
        winject::NodeImpl impl(cfg.stats_sec);
        core->callbacks.open();
        impl.start(core.get(), thread_setup);
        const Node node = HandleAccess::node(core);
        if (!core->stop_requested)
        {
            const Status applied = apply(node, cfg);
            if (!applied.ok())
            {
                if (!core->stop_requested)
                {
                    rc = 1;
                }
                core->request_stop();
            }
            else if (started)
            {
                started(node);
            }
        }
        // Every Done and Node::post until the node's thread closes the queue,
        // after it cancelled what was still pending.
        core->callbacks.run();
        impl.join();
    }
    core->stop_requested = false;
    core->busy = false;
    return rc;
}

int run(const Config& cfg, std::function<void(const Node&)> started,
        ThreadSetup thread_setup)
{
    return detail::run_node(process_core(), cfg, std::move(started),
                            std::move(thread_setup));
}

void stop()
{
    process_core()->request_stop();
}

void stop_from_signal()
{
    process_core()->request_stop_from_signal();
}

}  // namespace winject_l3
