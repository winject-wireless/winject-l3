#include "L3Core.h"
#include "L3Impl.h"
#include "WinjectL3.h"
#include "mplane/MplaneErrno.h"
#include "utils/Log.h"
#include "utils/Version.h"

#include <memory>
#include <string>

namespace winject
{

namespace
{

const std::shared_ptr<l3::detail::L3Core>& process_core()
{
    static const std::shared_ptr<l3::detail::L3Core> core =
        l3::detail::L3Core::create();
    return core;
}

RadioFcs api_fcs(winject::core::RadioFcsConfig fcs)
{
    switch (fcs)
    {
        case winject::core::RadioFcsConfig::signal:
            return RadioFcs::signal;
        case winject::core::RadioFcsConfig::actual:
            return RadioFcs::actual;
        case winject::core::RadioFcsConfig::auto_detect:
        default:
            return RadioFcs::auto_detect;
    }
}

RadioSpec radio_spec_of(const Config& cfg)
{
    RadioSpec spec;
    const winject::core::RadioDeviceConfig& d = cfg.radio_device;
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
                              const winject::core::UpstreamConfig& u)
{
    UpstreamSpec spec;
    spec.id = static_cast<uint8_t>(u.index);
    spec.bus_tx = u.bus_tx;
    spec.bus_rx = u.bus_rx;
    spec.transport = u.mode == winject::core::UpstreamMode::queue
                         ? Transport::queue
                         : Transport::udp;
    spec.rx = u.endpoint.rx;
    spec.tx = u.endpoint.tx;
    spec.reactor = u.reactor;
    spec.fec.type = u.fec_type == winject::core::FecType::RsBlockErasure
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
            [publish, index](std::shared_ptr<winject::core::UpstreamQueue> q)
        {
            publish(index, std::move(q));
        };
    }
    return spec;
}

}  // namespace

Version L3::version()
{
    const winject::core::WinjectVersion v = winject::core::own_version();
    Version out;
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;
    return out;
}

L3 L3::instance()
{
    return l3::detail::HandleAccess::l3(process_core());
}

Status L3::apply(const Config& cfg) const
{
    std::string err;
    if (!cfg.validate(&err))
    {
        LOG_ERR("%s", err.c_str());
        return l3::detail::error_status(winject::core::k_einval);
    }
    RadioManager radios = this->radios();
    const Result<Radio> radio = radios.create(radio_spec_of(cfg));
    if (!radio.ok())
    {
        LOG_ERR("radio: %s", radio.status.error.c_str());
        return radio.status;
    }
    UpstreamManager upstreams = this->upstreams();
    for (const winject::core::UpstreamConfig& u : cfg.upstreams)
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

int l3::detail::run_l3(const std::shared_ptr<L3Core>& core, const Config& cfg,
                       std::function<void(const L3&)> started,
                       L3::ThreadSetup thread_setup)
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
        LOG_ERR("winject-l3: l3 is already running");
        return 1;
    }
    int rc = 0;
    {
        // On the heap: run's caller is a host thread (16 KB on the P4) that
        // also runs apply, every Done and the console commands.
        auto impl =
            std::make_unique<winject::l3::detail::L3Impl>(cfg.stats_sec);
        core->callbacks.open();
        impl->start(core.get(), thread_setup);
        const L3 l3 = HandleAccess::l3(core);
        if (!core->stop_requested)
        {
            const Status applied = l3.apply(cfg);
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
                started(l3);
            }
        }
        // Every Done and L3::post until l3's thread closes the queue,
        // after it cancelled what was still pending.
        core->callbacks.run();
        impl->join();
    }
    core->stop_requested = false;
    core->busy = false;
    return rc;
}

int L3::run(const Config& cfg, std::function<void(const L3&)> started,
            ThreadSetup thread_setup)
{
    return l3::detail::run_l3(process_core(), cfg, std::move(started),
                              std::move(thread_setup));
}

void L3::stop()
{
    process_core()->request_stop();
}

void L3::stop_from_signal()
{
    process_core()->request_stop_from_signal();
}

}  // namespace winject
