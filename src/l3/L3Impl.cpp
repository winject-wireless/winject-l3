#include "L3Impl.h"

#include "L3Core.h"
#include "WinjectBuildVersion.h"
#include "fec/RsBlockErasure.h"
#include "frames/Mpdu.h"
#include "mplane/MplaneCorrelation.h"
#include "mplane/MplaneErrno.h"
#include "mplane/MplaneText.h"
#include "radio/RadioDefs.h"
#include "radio/RadioMplaneParse.h"
#include "radio/RxEvent.h"
#include "radio/WifiQueue.h"
#include "radio/WifiUdp.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"
#include "utils/Version.h"

#include <algorithm>
#include <cinttypes>
#include <errno.h>
#include <future>
#include <optional>
#include <stdexcept>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace winject::l3::detail
{

using namespace winject::core;

namespace api = winject;

namespace
{

constexpr size_t k_upstream_not_found = static_cast<size_t>(-1);

core::FecType core_fec_type(api::FecType type)
{
    return type == api::FecType::rs_block ? core::FecType::RsBlockErasure
                                          : core::FecType::none;
}

api::Fec api_fec(core::FecType type, int k, int n, int timeout_ms)
{
    api::Fec fec;
    fec.type = type == core::FecType::RsBlockErasure ? api::FecType::rs_block
                                                     : api::FecType::none;
    fec.k = k;
    fec.n = n;
    fec.timeout = std::chrono::milliseconds(timeout_ms);
    return fec;
}

api::Fec api_fec_of(const UpstreamLink* endpoint, int timeout_ms)
{
    core::FecType type = core::FecType::none;
    int k = 0;
    int n = 0;
    if (endpoint != nullptr)
    {
        endpoint->get_fec(&type, &k, &n);
    }
    return api_fec(type, k, n, timeout_ms);
}

RadioFcsConfig core_fcs(api::RadioFcs fcs)
{
    switch (fcs)
    {
        case api::RadioFcs::signal:
            return RadioFcsConfig::signal;
        case api::RadioFcs::actual:
            return RadioFcsConfig::actual;
        case api::RadioFcs::auto_detect:
        default:
            return RadioFcsConfig::auto_detect;
    }
}

api::RadioFcs api_fcs(RadioFcsConfig fcs)
{
    switch (fcs)
    {
        case RadioFcsConfig::signal:
            return api::RadioFcs::signal;
        case RadioFcsConfig::actual:
            return api::RadioFcs::actual;
        case RadioFcsConfig::auto_detect:
        default:
            return api::RadioFcs::auto_detect;
    }
}

api::RadioFcs api_fcs_mode(RadioFcsMode mode)
{
    switch (mode)
    {
        case RadioFcsMode::signal:
            return api::RadioFcs::signal;
        case RadioFcsMode::actual:
            return api::RadioFcs::actual;
        default:
            return api::RadioFcs::auto_detect;
    }
}

const char* fcs_config_name(RadioFcsConfig fcs)
{
    switch (fcs)
    {
        case RadioFcsConfig::signal:
            return "SIGNAL";
        case RadioFcsConfig::actual:
            return "ACTUAL";
        case RadioFcsConfig::auto_detect:
        default:
            return "AUTO";
    }
}

bool ipv4_set(const sockaddr_in& addr)
{
    return addr.sin_family == AF_INET && addr.sin_port != 0;
}

bool same_addr(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::string device_text(const RadioDeviceConfig& d)
{
    std::string text =
        "mplane=" + mplane_format_ipv4_port(d.mplane) + " dplane=";
    if (d.dplane_is_queue())
    {
        text += "queue:" + d.dplane_tx + "," + d.dplane_rx + "@" + d.reactor;
    }
    else
    {
        text += mplane_format_ipv4_port(d.dplane);
    }
    text += " fcs=";
    text += fcs_config_name(d.fcs);
    return text;
}

}  // namespace

L3Impl::L3Impl(unsigned stats_sec)
{
    cfg.stats_sec = stats_sec;
}

L3Impl::~L3Impl()
{
    tx_mux.stop();
    radio_upstream_table.clear();
    upstreams.clear();
    if (!host_upstream)
    {
        upstream_reactor->stop();
    }
    if (upstream_reactor_thread.joinable())
    {
        upstream_reactor_thread.join();
    }
    if (l3_thread.joinable())
    {
        reactor->stop();
        l3_thread.join();
    }
    close_radio();
}

bool L3Impl::use_host_reactors(const std::string& io,
                               const std::string& upstream)
{
    if (!io.empty())
    {
        auto r = bfc::default_reactor_registry::get_instance().get(io);
        if (!r)
        {
            LOG_ERR("l3 IO reactor %s: not registered", io.c_str());
            return false;
        }
        reactor = std::move(r);
        host_io = true;
    }
    if (!upstream.empty())
    {
        auto r = bfc::cv_reactor_registry::get_instance().get(upstream);
        if (!r)
        {
            LOG_ERR("l3 upstream reactor %s: not registered", upstream.c_str());
            return false;
        }
        upstream_reactor = std::move(r);
        host_upstream = true;
    }
    return true;
}

bool L3Impl::add_upstream_config(const UpstreamConfig& uc)
{
    if (!radio)
    {
        return false;
    }
    auto tx_queue = tx_mux.make_tx_queue();
    std::shared_ptr<UpstreamLink> up;
    try
    {
        up = construct_upstream(uc, tx_queue);
    }
    catch (const std::exception& e)
    {
        LOG_ERR("upstream construction failed: %s", e.what());
        return false;
    }
    radio_upstream_table.add(up, tx_queue, radio, uc.bus_tx, uc.bus_rx,
                             uc.scheduler_budget);
    upstreams.push_back(up);
    tx_mux.request_tick();
    LOG_INF("upstream-%zu type=%s bus_tx=%s bus_rx=%s", uc.index,
            up->type_name(), uc.bus_tx ? bus_to_string(uc.bus_tx).c_str() : "-",
            uc.bus_rx ? bus_to_string(uc.bus_rx).c_str() : "-");
    return true;
}

const char* L3Impl::setup_radio()
{
    const RadioDeviceConfig& d = cfg.radio_device;
    radio_queue_reactor.reset();
    // A queue d-plane delivers on its CV reactor thread, UDP on the IO thread;
    // RxDemux takes the table lock either way.
    auto on_rx = [this](bfc::shared_sized_buffer mpdu)
    {
        rx_demux.on_mpdu(std::move(mpdu));
    };
    std::string dplane;
    if (d.dplane_is_queue())
    {
        radio_queue_reactor =
            bfc::cv_reactor_registry::get_instance().get(d.reactor);
        if (!radio_queue_reactor)
        {
            LOG_ERR("radio queue reactor %s: not registered",
                    d.reactor.c_str());
            return k_einval;
        }
        auto queue = std::make_shared<WifiQueue>();
        if (!queue->open(*reactor, std::move(on_rx)) ||
            !queue->bind_dplane(
                WifiQueueDplaneBinding{d.dplane_tx, d.dplane_rx, d.reactor}))
        {
            radio_queue_reactor.reset();
            return k_eio;
        }
        radio = std::move(queue);
        dplane = "queue:" + d.dplane_tx + "," + d.dplane_rx + "@" + d.reactor;
    }
    else
    {
        radio = std::make_shared<WifiUdp>();
        if (!radio->open(*reactor, std::move(on_rx)))
        {
            radio.reset();
            return k_eio;
        }
        dplane = mplane_format_ipv4_port(d.dplane);
    }
    apply_radio_dplane();
    apply_configured_fcs();
    LOG_INF("radio dplane=%s fcs=%s", dplane.c_str(), fcs_config_name(d.fcs));
    return nullptr;
}

void L3Impl::close_radio()
{
    // A queue d-plane's host reactor outlives l3 and keeps delivering;
    // close before rx_demux goes, even if a pending TX event holds the radio.
    if (radio)
    {
        radio->close();
    }
}

void L3Impl::apply_radio_dplane()
{
    const RadioDeviceConfig& d = cfg.radio_device;
    if (radio == nullptr)
    {
        return;
    }
    tx_mux.post_send_socket_change(radio, d.dplane);
    radio->post_rx_event(EventCtrlRecvSocketChange{d.dplane});
}

void L3Impl::apply_configured_fcs()
{
    if (radio == nullptr)
    {
        return;
    }
    switch (cfg.radio_device.fcs)
    {
        case RadioFcsConfig::signal:
            radio->set_fcs_mode(RadioFcsMode::signal);
            break;
        case RadioFcsConfig::actual:
            radio->set_fcs_mode(RadioFcsMode::actual);
            break;
        case RadioFcsConfig::auto_detect:
        default:
            radio->set_fcs_mode(RadioFcsMode::unknown);
            break;
    }
}

std::shared_ptr<UpstreamLink> L3Impl::construct_upstream(
    const UpstreamConfig& uc,
    const std::shared_ptr<BoundedLossyQueue>& tx_queue,
    const UpstreamLink::TxSequences* resume)
{
    if (uc.mode != UpstreamMode::queue)
    {
        auto up = std::make_shared<UDPUpstream>(upstream_reactor, reactor, uc,
                                                tx_queue);
        if (resume)
        {
            up->resume_tx_sequences(*resume);
        }
        up->bind_callbacks();
        return up;
    }
    using Registry =
        bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;
    auto cv = bfc::cv_reactor_registry::get_instance().get(uc.reactor);
    auto app_tx_queue = Registry::get_instance().get(uc.endpoint.rx);
    auto app_rx_queue = Registry::get_instance().get(uc.endpoint.tx);
    // No registry names: L3 owns the queues and publishes them through
    // on_queue (add_upstream requires the callback).
    const auto publish = on_queue.find(uc.index);
    if (uc.endpoint.rx.empty() && uc.endpoint.tx.empty() &&
        uc.reactor.empty() && publish != on_queue.end())
    {
        cv = upstream_reactor;
        app_tx_queue = std::make_shared<ReactiveLossyQueue>();
        app_rx_queue = std::make_shared<ReactiveLossyQueue>();
        app_tx_queue->set_capacity(uc.rx_queue_capacity);
        app_rx_queue->set_capacity(uc.rx_queue_capacity);
    }
    auto up = std::make_shared<QueueUpstream>(cv, uc, tx_queue, app_tx_queue,
                                              app_rx_queue);
    if (resume)
    {
        up->resume_tx_sequences(*resume);
    }
    up->bind_callbacks();
    if (publish != on_queue.end() && publish->second)
    {
        // User code on l3's thread: its l3 calls return EDEADLK.
        publish->second(up->app_transport());
    }
    return up;
}

bool L3Impl::replace_upstream(size_t index, const UpstreamConfig& uc,
                              std::string* error)
{
    const auto original = cfg.upstreams[index];
    {
        std::lock_guard<std::mutex> lock(radio_upstream_table.mutex());
        radio_upstream_table.entries()[index] = {};
    }
    // The peer's sequencers see one unbroken stream across the swap.
    std::optional<UpstreamLink::TxSequences> sequences;
    if (upstreams[index])
    {
        upstreams[index]->teardown();
        sequences = upstreams[index]->tx_sequences();
    }
    upstreams[index].reset();
    auto install = [&](const UpstreamConfig& config)
    {
        auto queue = tx_mux.make_tx_queue();
        auto up = construct_upstream(config, queue,
                                     sequences ? &*sequences : nullptr);
        {
            std::lock_guard<std::mutex> lock(radio_upstream_table.mutex());
            radio_upstream_table.entries()[index] =
                RadioUpstreamEntry{up,
                                   up->rx_queue(),
                                   up->rx_queue_eviction_counter(),
                                   queue,
                                   radio,
                                   config.bus_tx,
                                   config.bus_rx,
                                   config.scheduler_budget};
        }
        upstreams[index] = std::move(up);
    };
    try
    {
        install(uc);
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = k_eio;
        }
        LOG_ERR("upstream replacement failed: %s", e.what());
        try
        {
            install(original);
        }
        catch (const std::exception& rollback)
        {
            LOG_ERR("upstream restore failed: %s", rollback.what());
        }
        return false;
    }
    return true;
}

bool L3Impl::set_upstream_scheduler_budget(size_t index, size_t budget,
                                           std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (index >= upstreams.size() || budget == 0)
    {
        return fail("invalid index or budget");
    }
    if (!radio_upstream_table.set_budget(index, budget))
    {
        return fail("failed to set budget");
    }
    if (index < cfg.upstreams.size())
    {
        cfg.upstreams[index].scheduler_budget = budget;
    }
    tx_mux.request_tick();
    LOG_INF("upstream-%zu scheduler_budget=%zu", index, budget);
    return true;
}

size_t L3Impl::upstream_index(uint8_t id) const
{
    for (size_t i = 0; i < cfg.upstreams.size(); ++i)
    {
        if (cfg.upstreams[i].index == id)
        {
            return i;
        }
    }
    return k_upstream_not_found;
}

bool L3Impl::has_upstream(uint8_t id) const
{
    return upstream_index(id) != k_upstream_not_found;
}

std::vector<uint8_t> L3Impl::upstream_ids() const
{
    std::vector<uint8_t> ids;
    ids.reserve(cfg.upstreams.size());
    for (const UpstreamConfig& u : cfg.upstreams)
    {
        ids.push_back(static_cast<uint8_t>(u.index));
    }
    return ids;
}

api::UpstreamSpec L3Impl::upstream_spec_at(size_t index) const
{
    const UpstreamConfig& uc = cfg.upstreams[index];
    const UpstreamLink* endpoint =
        index < upstreams.size() ? upstreams[index].get() : nullptr;
    api::UpstreamSpec spec;
    spec.id = static_cast<uint8_t>(uc.index);
    spec.bus_tx = uc.bus_tx;
    spec.bus_rx = uc.bus_rx;
    spec.transport = uc.mode == UpstreamMode::queue ? api::Transport::queue
                                                    : api::Transport::udp;
    spec.reactor = uc.reactor;
    spec.rx_queue_capacity = uc.rx_queue_capacity;
    if (endpoint != nullptr)
    {
        spec.rx = endpoint->endpoint_rx();
        spec.tx = endpoint->endpoint_tx();
        spec.fec = api_fec_of(endpoint, endpoint->fec_timeout_ms());
    }
    else
    {
        spec.rx = uc.endpoint.rx;
        spec.tx = uc.endpoint.tx;
        spec.fec = api_fec(uc.fec_type, uc.fec_k, uc.fec_n, uc.fec_timeout_ms);
    }
    size_t budget = 0;
    spec.quanta = radio_upstream_table.get_budget(index, &budget)
                      ? budget
                      : uc.scheduler_budget;
    return spec;
}

api::Status L3Impl::add_upstream(const api::UpstreamSpec& spec)
{
    if (!radio_created)
    {
        return error_status(k_enodev);
    }
    if (spec.bus_tx == 0 && spec.bus_rx == 0)
    {
        return error_status(k_einval);
    }
    if (has_upstream(spec.id))
    {
        return error_status(k_eexist);
    }
    if (spec.quanta == 0 || spec.rx_queue_capacity == 0 ||
        spec.fec.timeout.count() < 0)
    {
        return error_status(k_einval);
    }
    UpstreamConfig uc;
    uc.index = spec.id;
    uc.bus_tx = spec.bus_tx;
    uc.bus_rx = spec.bus_rx;
    uc.scheduler_budget = spec.quanta;
    uc.rx_queue_capacity = spec.rx_queue_capacity;
    uc.endpoint = EndpointAddress{spec.rx, spec.tx};
    if (spec.transport == api::Transport::queue)
    {
        uc.mode = UpstreamMode::queue;
        uc.reactor = spec.reactor;
        const bool owned =
            spec.rx.empty() && spec.tx.empty() && spec.reactor.empty();
        if (owned && !spec.on_queue)
        {
            return error_status(k_einval);
        }
    }
    else
    {
        uc.mode = upstream_mode(uc.endpoint);
        if (!spec.reactor.empty())
        {
            return error_status(k_einval);
        }
    }
    uc.fec_type = core_fec_type(spec.fec.type);
    uc.fec_k = spec.fec.k;
    uc.fec_n = spec.fec.n;
    uc.fec_timeout_ms = static_cast<int>(spec.fec.timeout.count());
    if (uc.fec_type == core::FecType::none)
    {
        uc.fec_k = 0;
        uc.fec_n = 0;
    }
    else
    {
        RsBlockErasure codec;
        if (!codec.init(uc.fec_k, uc.fec_n, uc.fec_timeout_ms))
        {
            return error_status(k_einval);
        }
    }
    std::vector<UpstreamConfig> candidate = cfg.upstreams;
    candidate.push_back(uc);
    std::string val_err;
    if (!Config::validate_upstreams(candidate, &val_err))
    {
        return error_status(k_einval);
    }
    if (spec.on_queue)
    {
        on_queue[uc.index] = spec.on_queue;
    }
    cfg.upstreams.push_back(uc);
    if (!add_upstream_config(uc))
    {
        cfg.upstreams.pop_back();
        on_queue.erase(uc.index);
        return error_status(k_eio);
    }
    return {};
}

api::Status L3Impl::remove_upstream(uint8_t id)
{
    const size_t index = upstream_index(id);
    if (index == k_upstream_not_found || !radio_upstream_table.remove(index))
    {
        return error_status(k_enoent);
    }
    upstreams.erase(upstreams.begin() + static_cast<ptrdiff_t>(index));
    tx_mux.request_tick();
    cfg.upstreams.erase(cfg.upstreams.begin() + static_cast<ptrdiff_t>(index));
    on_queue.erase(id);
    metrics_registry.remove_prefix("upstream_" + std::to_string(id) + "_");
    return {};
}

L3Impl::Result<api::UpstreamSpec> L3Impl::upstream_spec(uint8_t id) const
{
    const size_t index = upstream_index(id);
    if (index == k_upstream_not_found)
    {
        return error_result<api::UpstreamSpec>(k_enoent);
    }
    return value_result(upstream_spec_at(index));
}

L3Impl::Result<api::UpstreamSpec> L3Impl::update_upstream(
    uint8_t id, const api::UpstreamPatch& patch)
{
    const size_t index = upstream_index(id);
    if (index == k_upstream_not_found)
    {
        return error_result<api::UpstreamSpec>(k_enoent);
    }
    if (!patch.fec && !patch.quanta)
    {
        return error_result<api::UpstreamSpec>(k_einval);
    }
    const UpstreamConfig& current = cfg.upstreams[index];
    const bool have_fec = patch.fec.has_value();
    const api::Fec fec = have_fec ? *patch.fec : api::Fec{};
    const int timeout_ms = static_cast<int>(fec.timeout.count());
    // A timeout equal to the current one is no change (it may be 0 from a
    // config file, which an update could not set).
    const bool have_timeout = have_fec && timeout_ms != current.fec_timeout_ms;
    UpstreamConfig candidate;
    std::string val_err;
    if (!Config::validate_upstream_update(
            current, core_fec_type(fec.type), fec.k, fec.n, timeout_ms,
            patch.quanta ? *patch.quanta : 0, have_fec, have_fec, have_fec,
            have_timeout, patch.quanta.has_value(), cfg.upstreams, &candidate,
            &val_err))
    {
        return error_result<api::UpstreamSpec>(k_einval);
    }
    std::string err;
    if (have_fec)
    {
        if (!replace_upstream(index, candidate, &err))
        {
            return error_result<api::UpstreamSpec>(err.empty() ? k_eio
                                                               : err.c_str());
        }
    }
    else if (!set_upstream_scheduler_budget(index, candidate.scheduler_budget,
                                            &err))
    {
        return error_result<api::UpstreamSpec>(err);
    }
    cfg.upstreams[index] = candidate;
    return value_result(upstream_spec_at(index));
}

L3Impl::Result<api::UpstreamRxStats> L3Impl::upstream_rx_stats(uint8_t id) const
{
    const size_t index = upstream_index(id);
    if (index == k_upstream_not_found)
    {
        return error_result<api::UpstreamRxStats>(k_enoent);
    }
    const UpstreamLink* endpoint =
        index < upstreams.size() ? upstreams[index].get() : nullptr;
    api::UpstreamRxStats s;
    s.fec = api_fec_of(endpoint, cfg.upstreams[index].fec_timeout_ms);
    if (endpoint != nullptr)
    {
        s.fec.timeout = std::chrono::milliseconds(endpoint->fec_timeout_ms());
        s.bytes = endpoint->app_rx_bytes();
        s.packets = endpoint->app_rx_packets();
        s.oversize = endpoint->app_rx_oversize_pkt();
        s.fec_recovered = endpoint->fec_recovered();
        s.fec_lost = endpoint->fec_decode_fail();
        s.fec_bytes = endpoint->fec_air_rx_bytes();
        s.fec_packets = endpoint->fec_air_rx_packets();
        s.push_drops = endpoint->rx_counters().queue_evictions;
    }
    uint64_t gap = 0;
    if (radio_upstream_table.peek_seq_lost(index, &gap))
    {
        s.air_gap = gap;
    }
    return value_result(s);
}

L3Impl::Result<api::UpstreamTxStats> L3Impl::upstream_tx_stats(uint8_t id) const
{
    const size_t index = upstream_index(id);
    if (index == k_upstream_not_found)
    {
        return error_result<api::UpstreamTxStats>(k_enoent);
    }
    const UpstreamLink* endpoint =
        index < upstreams.size() ? upstreams[index].get() : nullptr;
    api::UpstreamTxStats s;
    s.fec = api_fec_of(endpoint, cfg.upstreams[index].fec_timeout_ms);
    if (endpoint != nullptr)
    {
        s.fec.timeout = std::chrono::milliseconds(endpoint->fec_timeout_ms());
        s.bytes = endpoint->app_tx_bytes();
        s.packets = endpoint->app_tx_packets();
        s.fec_bytes = endpoint->fec_air_tx_bytes();
        s.fec_packets = endpoint->fec_air_tx_packets();
        endpoint->tx_pending_stats(&s.pending_packets, &s.pending_bytes);
    }
    return value_result(s);
}

L3Impl::Result<std::vector<api::Metric>> L3Impl::metrics(
    const std::vector<std::string>& keys)
{
    refresh_host_metrics();
    const std::map<std::string, Metrics> snapshot =
        metrics_registry.getMetrics(keys);
    std::vector<api::Metric> out;
    const auto want_key = [&](const std::string& k) -> bool
    {
        return keys.empty() ||
               std::find(keys.begin(), keys.end(), k) != keys.end();
    };
    if (want_key("radio_version"))
    {
        api::Metric row;
        row.key = "radio_version";
        if (radio_version_known)
        {
            row.value = "v" + std::to_string(radio_version.x) + "." +
                        std::to_string(radio_version.y) + "." +
                        std::to_string(radio_version.z);
        }
        else
        {
            row.value = "-";
        }
        out.push_back(std::move(row));
    }
    for (const auto& entry : snapshot)
    {
        api::Metric row;
        row.key = entry.first;
        row.value = metric_value_to_string(entry.second);
        out.push_back(std::move(row));
    }
    return value_result(std::move(out));
}

void L3Impl::refresh_host_metrics()
{
    metrics_registry.get_metrics<MetricU64>("rx_drop_domain")
        ->store(rx_demux.rx_drop_domain());
    metrics_registry.get_metrics<MetricU64>("rx_drop_bus")
        ->store(rx_demux.rx_drop_bus());
    metrics_registry.get_metrics<MetricU64>("tx_send_fail_mpdu")
        ->store(tx_mux.tx_send_fail_mpdu());
    metrics_registry.get_metrics<MetricU64>("tx_send_fail_byt")
        ->store(tx_mux.tx_send_fail_byt());
    metrics_registry.get_metrics<MetricU64>("tx_pacing_txtime_us")
        ->store(tx_mux.pacing_txtime_full_us());
    metrics_registry.get_metrics<MetricU64>("tx_pacing_gap_us")
        ->store(tx_mux.pacing_gap_us());
    metrics_registry.get_metrics<MetricU64>("tx_pacing_interval_us")
        ->store(tx_mux.pacing_interval_full_us());
    if (radio != nullptr)
    {
        const RadioDplane::counters_s rc = radio->peek_counters();
        metrics_registry.get_metrics<MetricU64>("radio_rx_pkt")
            ->store(rc.rx_pkt);
        metrics_registry.get_metrics<MetricU64>("radio_rx_byt")
            ->store(rc.rx_byte);
        metrics_registry.get_metrics<MetricU64>("radio_tx_pkt")
            ->store(rc.tx_pkt);
        metrics_registry.get_metrics<MetricU64>("radio_tx_byt")
            ->store(rc.tx_byte);
        metrics_registry.get_metrics<MetricU64>("radio_fcs_err_pkt")
            ->store(rc.fcs_error_pkt);
        metrics_registry.get_metrics<MetricU64>("radio_fcs_unknown_pkt")
            ->store(rc.fcs_unknown_pkt);
        metrics_registry.get_metrics<MetricU64>("radio_tx_drops")
            ->store(rc.tx_drops);
        const RadioFcsMode fcs = radio->fcs_mode();
        metrics_registry.get_metrics<MetricU64>("radio_fcs_mode")
            ->store(static_cast<uint64_t>(fcs));
    }
    metrics_registry.get_metrics<MetricU64>("radio_proto_ok")
        ->store(radio_proto_ok ? 1u : 0u);
    for (size_t i = 0; i < upstreams.size(); ++i)
    {
        const UpstreamLink* up = upstreams[i].get();
        if (up == nullptr)
        {
            continue;
        }
        if (i >= cfg.upstreams.size())
        {
            continue;
        }
        struct
        {
            size_t id;
        } view{cfg.upstreams[i].index};
        const std::string gap_key =
            "upstream_" + std::to_string(view.id) + "_air_rx_gap_loss";
        const UpstreamStats air = up->stats();
        metrics_registry.get_metrics<MetricU64>(gap_key)->store(
            air.air_rx_gap_loss);
        metrics_registry
            .get_metrics<MetricU64>("upstream_" + std::to_string(view.id) +
                                    "_air_rx_resyncs")
            ->store(air.air_rx_resyncs);
        const std::string oversize_key =
            "upstream_" + std::to_string(view.id) + "_app_rx_oversize_pkt";
        metrics_registry.get_metrics<MetricU64>(oversize_key)
            ->store(up->app_rx_oversize_pkt());
        metrics_registry
            .get_metrics<MetricU64>("upstream_" + std::to_string(view.id) +
                                    "_tx_queue_drops")
            ->store(up->tx_queue_drops());
        metrics_registry
            .get_metrics<MetricU64>("upstream_" + std::to_string(view.id) +
                                    "_rx_queue_evictions")
            ->store(up->rx_queue_evictions());
        metrics_registry
            .get_metrics<MetricU64>("upstream_" + std::to_string(view.id) +
                                    "_app_tx_queue_evictions")
            ->store(up->tx_counters().app_queue_evictions);
        const auto transport = up->rx_counters();
        const std::string transport_prefix =
            "upstream_" + std::to_string(view.id) + "_";
        metrics_registry
            .get_metrics<MetricU64>(transport_prefix + "app_delivery_attempts")
            ->store(transport.delivery_attempts);
        metrics_registry
            .get_metrics<MetricU64>(transport_prefix + "app_delivery_accepted")
            ->store(transport.accepted_packets);
        metrics_registry
            .get_metrics<MetricU64>(transport_prefix + "app_delivery_errors")
            ->store(transport.send_errors);
        metrics_registry
            .get_metrics<MetricU64>(transport_prefix + "app_delivery_no_peer")
            ->store(transport.no_peer_drops);
        metrics_registry
            .get_metrics<MetricU64>(transport_prefix + "app_delivery_closed")
            ->store(transport.closed_drops);
        metrics_registry
            .get_metrics<MetricU64>(transport_prefix + "app_delivery_evictions")
            ->store(transport.queue_evictions);
        const std::string fec_unexpected_key =
            "upstream_" + std::to_string(view.id) + "_air_rx_fec_unexpected";
        metrics_registry.get_metrics<MetricU64>(fec_unexpected_key)
            ->store(up->fec_air_rx_unexpected());
    }
}

void L3Impl::apply_radio_caps(const RadioCapsReply& caps)
{
    if (radio == nullptr)
    {
        return;
    }
    if (!caps.resolved)
    {
        LOG_WRN("radio_caps_info: %s", caps.error.c_str());
        return;
    }
    const RadioFcsMode mode = caps.fcs;
    if (caps.legacy)
    {
        LOG_WRN("radio has no radio_caps_info; assuming fcs=ACTUAL");
    }
    if (cfg.radio_device.fcs == RadioFcsConfig::signal &&
        mode != RadioFcsMode::signal)
    {
        LOG_WRN("radio_device fcs=SIGNAL but radio reports fcs=%s; using radio",
                radio_fcs_mode_name(mode));
    }
    else if (cfg.radio_device.fcs == RadioFcsConfig::actual &&
             mode != RadioFcsMode::actual)
    {
        LOG_WRN("radio_device fcs=ACTUAL but radio reports fcs=%s; using radio",
                radio_fcs_mode_name(mode));
    }
    const RadioFcsMode prev = radio->fcs_mode();
    if (prev != mode && prev != RadioFcsMode::unknown)
    {
        LOG_INF("radio fcs mode %s -> %s", radio_fcs_mode_name(prev),
                radio_fcs_mode_name(mode));
    }
    radio->set_fcs_mode(mode);
}

const char* L3Impl::radio_unavailable_error() const
{
    if (radio_version_known && !radio_proto_ok)
    {
        return k_eproto;
    }
    return k_enodev;
}

api::RadioInfo L3Impl::radio_info_from(const RadioPhyReply& reply) const
{
    api::RadioInfo info;
    if (reply.phy)
    {
        const RadioPhyView& v = *reply.phy;
        info.phy.channel = static_cast<uint8_t>(v.channel);
        info.phy.modulation = v.modulation;
        info.phy.power_dbm = static_cast<int8_t>(v.tx_power);
        if (v.cca_valid)
        {
            info.phy.cca = v.cca;
        }
        if (v.rssi_valid)
        {
            info.rssi = v.rssi;
        }
    }
    if (radio_version_known)
    {
        api::Version ver;
        ver.x = radio_version.x;
        ver.y = radio_version.y;
        ver.z = radio_version.z;
        info.version = ver;
    }
    info.text = reply.result.text();
    return info;
}

api::Status L3Impl::create_radio(const api::RadioSpec& spec)
{
    if (radio_created)
    {
        return error_status(k_eexist);
    }
    const api::RadioDevice& d = spec.device;
    const bool queue =
        !d.dplane_tx.empty() || !d.dplane_rx.empty() || !d.reactor.empty();
    const std::string modulation =
        Config::canonical_modulation(spec.phy.modulation);
    if (spec.domain == 0 || !ipv4_set(d.mplane) || modulation.empty() ||
        spec.phy.channel < 1 || spec.phy.channel > 14 ||
        !Config::modulation_ok_for_channel(modulation, spec.phy.channel) ||
        spec.phy.power_dbm < Config::k_power_min_dbm ||
        spec.phy.power_dbm > Config::k_power_max_dbm)
    {
        return error_status(k_einval);
    }
    if (queue ? (d.dplane_tx.empty() || d.dplane_rx.empty() ||
                 d.reactor.empty() || d.dplane_tx == d.dplane_rx)
              : !ipv4_set(d.dplane))
    {
        return error_status(k_einval);
    }
    RadioConfig& rc = cfg;
    rc = RadioConfig{};
    rc.radio_device.mplane = d.mplane;
    rc.radio_device.dplane = d.dplane;
    rc.radio_device.dplane_tx = d.dplane_tx;
    rc.radio_device.dplane_rx = d.dplane_rx;
    rc.radio_device.reactor = d.reactor;
    rc.radio_device.fcs = core_fcs(d.fcs);
    rc.channel = spec.phy.channel;
    rc.modulation = modulation;
    rc.power_dbm = spec.phy.power_dbm;
    rc.cca_explicit = spec.phy.cca.has_value();
    rc.cca = spec.phy.cca.value_or(false);
    rc.domain = spec.domain;
    rc.config_slot = spec.config_slot;
    if (const char* err = setup_radio())
    {
        return error_status(err);
    }
    radio_supervisor.bind(&cfg, &tx_mux, radio.get(), reactor.get());
    radio_supervisor.configure_tx_mux();
    rx_demux.set_domain(cfg.domain);
    radio_created = true;
    return {};
}

api::Status L3Impl::add_radio()
{
    if (!radio_created)
    {
        return error_status(k_enoent);
    }
    if (radio_added)
    {
        return error_status(k_ealready);
    }
    radio_added = true;
    begin_console();
    if (!console_ok)
    {
        LOG_WRN("waiting for radio m-plane %s",
                mplane_format_ipv4_port(cfg.radio_device.mplane).c_str());
    }
    LOG_INF("manager running radio_device id=0 %s (%s)",
            device_text(cfg.radio_device).c_str(), cfg.modulation.c_str());
    if (radio_queue_reactor)
    {
        // The host already runs this reactor for radio/upstream queues.
        // Its native blocking wait avoids poll()'s sub-ms spin tail and
        // keeps TX off the independently scheduled control-plane thread.
        tx_mux.start(*radio_queue_reactor);
        LOG_INF("TX scheduler: queue reactor %s, timer step %llu us",
                cfg.radio_device.reactor.c_str(),
                static_cast<unsigned long long>(
                    radio_queue_reactor->minimum_timer_step()));
    }
    else
    {
        tx_mux.start(*reactor);
    }
    return {};
}

api::Status L3Impl::remove_radio()
{
    if (!radio_created)
    {
        return error_status(k_enoent);
    }
    if (!cfg.upstreams.empty())
    {
        return error_status(k_ebusy);
    }
    if (radio_added)
    {
        tx_mux.stop();
        radio_added = false;
        close_console();
    }
    reconnect_timer.cancel();
    close_radio();
    radio.reset();
    radio_queue_reactor.reset();
    radio_created = false;
    radio_version_known = false;
    radio_proto_ok = false;
    proto_mismatch_logged = false;
    return {};
}

L3Impl::Result<api::RadioState> L3Impl::radio_state() const
{
    if (!radio_created)
    {
        return error_result<api::RadioState>(k_enoent);
    }
    if (!radio_added)
    {
        return value_result(api::RadioState::stopped);
    }
    if (radio_version_known && !radio_proto_ok)
    {
        return value_result(api::RadioState::incompatible);
    }
    // up: the m-plane answers and the radio has been programmed.
    return value_result(console_ok && radio_programmed
                            ? api::RadioState::up
                            : api::RadioState::connecting);
}

L3Impl::Result<api::RadioDevice> L3Impl::radio_device() const
{
    if (!radio_created)
    {
        return error_result<api::RadioDevice>(k_enoent);
    }
    const RadioDeviceConfig& d = cfg.radio_device;
    api::RadioDevice out;
    out.mplane = d.mplane;
    out.dplane = d.dplane;
    out.dplane_tx = d.dplane_tx;
    out.dplane_rx = d.dplane_rx;
    out.reactor = d.reactor;
    out.fcs = api_fcs(d.fcs);
    return value_result(out);
}

L3Impl::Result<api::RadioSpec> L3Impl::radio_spec() const
{
    const auto device = radio_device();
    if (!device.ok())
    {
        return error_result<api::RadioSpec>(device.status.error);
    }
    api::RadioSpec spec;
    spec.device = device.value;
    spec.phy.channel = cfg.channel;
    spec.phy.modulation = cfg.modulation;
    spec.phy.power_dbm = cfg.power_dbm;
    if (cfg.cca_explicit)
    {
        spec.phy.cca = cfg.cca;
    }
    spec.config_slot = cfg.config_slot;
    spec.domain = cfg.domain;
    return value_result(spec);
}

api::Status L3Impl::set_radio_device(const api::RadioDevice& d)
{
    if (!radio_created)
    {
        return error_status(k_enoent);
    }
    const RadioDeviceConfig& cur = cfg.radio_device;
    if (cur.dplane_is_queue())
    {
        // A queue d-plane is fixed at create.
        if (d.dplane_tx != cur.dplane_tx || d.dplane_rx != cur.dplane_rx ||
            d.reactor != cur.reactor || !same_addr(d.dplane, cur.dplane))
        {
            return error_status(k_einval);
        }
    }
    else if (!d.dplane_tx.empty() || !d.dplane_rx.empty() ||
             !d.reactor.empty() || !ipv4_set(d.dplane))
    {
        return error_status(k_einval);
    }
    if (!ipv4_set(d.mplane))
    {
        return error_status(k_einval);
    }
    const RadioFcsConfig fcs = core_fcs(d.fcs);
    const bool mplane_changed = !same_addr(cur.mplane, d.mplane);
    const bool dplane_changed =
        !cur.dplane_is_queue() && !same_addr(cur.dplane, d.dplane);
    const bool fcs_changed = cur.fcs != fcs;
    cfg.radio_device.mplane = d.mplane;
    cfg.radio_device.dplane = d.dplane;
    cfg.radio_device.fcs = fcs;
    if (dplane_changed)
    {
        apply_radio_dplane();
    }
    if (mplane_changed)
    {
        radio_version_known = false;
        radio_proto_ok = false;
        if (radio_added)
        {
            drop_console();
            apply_configured_fcs();
            schedule_reconnect(std::chrono::milliseconds(0));
        }
        else
        {
            apply_configured_fcs();
        }
    }
    else if (fcs_changed)
    {
        if (console_ok && cfg.radio_device.fcs == RadioFcsConfig::auto_detect)
        {
            console.query_radio_caps(
                [this](RadioCapsReply r)
                {
                    apply_radio_caps(r);
                });
        }
        else if (!console_ok)
        {
            apply_configured_fcs();
        }
    }
    return {};
}

void L3Impl::set_radio_phy(const api::RadioPhyPatch& patch,
                           Reply<api::RadioInfo> reply)
{
    if (!radio_created)
    {
        reply(error_result<api::RadioInfo>(k_enoent));
        return;
    }
    if (!patch.channel && !patch.modulation && !patch.power_dbm && !patch.cca)
    {
        reply(error_result<api::RadioInfo>(k_einval));
        return;
    }
    std::string canonical;
    if (patch.modulation)
    {
        canonical = Config::canonical_modulation(*patch.modulation);
        if (canonical.empty() ||
            !Config::modulation_ok_for_channel(
                canonical, patch.channel ? *patch.channel : cfg.channel))
        {
            reply(error_result<api::RadioInfo>(k_einval));
            return;
        }
    }
    if (!radio_added)
    {
        // Stored only: the radio checks a live change itself, this one is
        // checked here before it reaches the air on add.
        const uint8_t channel = patch.channel.value_or(cfg.channel);
        const std::string modulation =
            patch.modulation ? canonical : cfg.modulation;
        const int8_t power = patch.power_dbm.value_or(cfg.power_dbm);
        if (channel < 1 || channel > 14 || power < Config::k_power_min_dbm ||
            power > Config::k_power_max_dbm ||
            !Config::modulation_ok_for_channel(modulation, channel))
        {
            reply(error_result<api::RadioInfo>(k_einval));
            return;
        }
        cfg.channel = channel;
        cfg.modulation = modulation;
        cfg.power_dbm = power;
        if (patch.cca)
        {
            cfg.cca = *patch.cca;
            cfg.cca_explicit = true;
        }
        if (patch.modulation)
        {
            radio_supervisor.sync_pacing_for_modulation(cfg.modulation);
        }
        api::RadioInfo info;
        info.phy.channel = cfg.channel;
        info.phy.modulation = cfg.modulation;
        info.phy.power_dbm = cfg.power_dbm;
        if (cfg.cca_explicit)
        {
            info.phy.cca = cfg.cca;
        }
        reply(value_result(info));
        return;
    }
    // Connecting includes "m-plane up, not programmed yet": the startup
    // programming copied cfg and would send the old PHY after this one.
    if (!console_ok || !radio_programmed)
    {
        reply(error_result<api::RadioInfo>(radio_unavailable_error()));
        return;
    }
    std::string kv;
    auto add = [&kv](const std::string& field)
    {
        if (!kv.empty())
        {
            kv += ' ';
        }
        kv += field;
    };
    if (patch.channel)
    {
        add("channel=" + std::to_string(*patch.channel));
    }
    if (patch.power_dbm)
    {
        add("tx_power=" + std::to_string(*patch.power_dbm));
    }
    if (patch.modulation)
    {
        add("modulation=" + canonical);
    }
    if (patch.cca)
    {
        add(*patch.cca ? "cca=1" : "cca=0");
    }
    console.send_radio_tx(
        kv,
        [this, patch, canonical, reply](RadioPhyReply r1)
        {
            if (!r1.result.ok)
            {
                reply(error_result<api::RadioInfo>(
                    mplane_err_string(r1.result.error)));
                return;
            }
            if (patch.channel)
            {
                cfg.channel = *patch.channel;
            }
            if (patch.power_dbm)
            {
                cfg.power_dbm = *patch.power_dbm;
            }
            if (patch.modulation)
            {
                cfg.modulation = canonical;
                radio_supervisor.sync_pacing_for_modulation(cfg.modulation);
            }
            if (patch.cca)
            {
                cfg.cca = *patch.cca;
                cfg.cca_explicit = true;
            }
            if (r1.phy)
            {
                radio_supervisor.set_actual_phy(*r1.phy);
            }
            const api::RadioInfo info = radio_info_from(r1);
            console.send_save_slot(cfg.config_slot,
                                   [info, reply](MplaneResult r2)
                                   {
                                       if (!r2.ok)
                                       {
                                           reply(error_result<api::RadioInfo>(
                                               mplane_err_string(r2.error)));
                                           return;
                                       }
                                       reply(value_result(info));
                                   });
        });
}

void L3Impl::radio_info(Reply<api::RadioInfo> reply)
{
    if (!radio_created)
    {
        reply(error_result<api::RadioInfo>(k_enoent));
        return;
    }
    if (!console_ok)
    {
        reply(error_result<api::RadioInfo>(radio_unavailable_error()));
        return;
    }
    console.query_radio_info(
        [this, reply](RadioPhyReply r)
        {
            if (!r.result.ok)
            {
                reply(error_result<api::RadioInfo>(
                    mplane_err_string(r.result.error)));
                return;
            }
            if (r.phy)
            {
                radio_supervisor.set_actual_phy(*r.phy);
            }
            reply(value_result(radio_info_from(r)));
        });
}

void L3Impl::radio_caps(Reply<api::RadioCaps> reply)
{
    if (!radio_created || radio == nullptr)
    {
        reply(error_result<api::RadioCaps>(k_enoent));
        return;
    }
    auto answer = [this, reply]()
    {
        if (radio == nullptr)
        {
            reply(error_result<api::RadioCaps>(k_enodev));
            return;
        }
        const RadioFcsMode mode = radio->fcs_mode();
        if (mode == RadioFcsMode::unknown)
        {
            reply(error_result<api::RadioCaps>(k_enodata));
            return;
        }
        api::RadioCaps caps;
        caps.fcs = api_fcs_mode(mode);
        caps.text = format_radio_caps_ok_line(mode);
        reply(value_result(caps));
    };
    if (!console_ok)
    {
        answer();
        return;
    }
    console.query_radio_caps(
        [this, reply, answer](RadioCapsReply r)
        {
            if (!r.result.ok && !r.result.is_enosys())
            {
                reply(error_result<api::RadioCaps>(
                    mplane_err_string(r.result.error)));
                return;
            }
            apply_radio_caps(r);
            answer();
        });
}

void L3Impl::radio_stats(Reply<api::RadioStats> reply)
{
    if (!radio_created)
    {
        reply(error_result<api::RadioStats>(k_enoent));
        return;
    }
    if (!console_ok)
    {
        reply(error_result<api::RadioStats>(radio_unavailable_error()));
        return;
    }
    console.query_radio_counters(
        [reply](RadioCountersReply r)
        {
            if (!r.result.ok)
            {
                reply(error_result<api::RadioStats>(
                    mplane_err_string(r.result.error)));
                return;
            }
            if (!r.counters)
            {
                reply(error_result<api::RadioStats>(k_eproto));
                return;
            }
            const RadioCounters& c = *r.counters;
            api::RadioStats s;
            s.tx.queue_size = c.tx.queue_size;
            s.tx.in_flight = c.tx.in_flight;
            s.tx.dropped_invalid_frame = c.tx.dropped_invalid_frame;
            s.tx.dropped_queue = c.tx.dropped_queue;
            s.tx.dropped_wifi = c.tx.dropped_wifi;
            s.tx.ether_packets = c.tx.ether_packets;
            s.tx.air_packets = c.tx.air_packets;
            s.tx.timestamp_us = c.tx.timestamp_us;
            s.rx.queue_size = c.rx.queue_size;
            s.rx.dropped_filter_mismatched = c.rx.dropped_filter_mismatched;
            s.rx.dropped_queue = c.rx.dropped_queue;
            s.rx.dropped_no_peer = c.rx.dropped_no_peer;
            s.rx.dropped_send_failed = c.rx.dropped_send_failed;
            s.rx.ether_packets = c.rx.ether_packets;
            s.rx.air_packets = c.rx.air_packets;
            s.rx.timestamp_us = c.rx.timestamp_us;
            reply(value_result(s));
        });
}

void L3Impl::radio_reset(Reply<Empty> reply)
{
    if (!radio_created)
    {
        reply(error_result<Empty>(k_enoent));
        return;
    }
    if (!console_ok)
    {
        reply(error_result<Empty>(radio_unavailable_error()));
        return;
    }
    console.send_radio_reset(
        [reply](MplaneResult r)
        {
            if (!r.ok)
            {
                reply(error_result<Empty>(mplane_err_string(r.error)));
                return;
            }
            reply(value_result(Empty{}));
        });
}

// Transparent radio m-plane: a radio-specific command line, forwarded
// unchanged. l3 does not track what it changes.
void L3Impl::radio_mplane(const std::string& line, Reply<std::string> reply)
{
    if (line.find_first_not_of(" \t") == std::string::npos ||
        line.find_first_of("\r\n") != std::string::npos)
    {
        reply(error_result<std::string>(k_einval));
        return;
    }
    if (!radio_created)
    {
        reply(error_result<std::string>(k_enoent));
        return;
    }
    if (!console_ok)
    {
        reply(error_result<std::string>(radio_unavailable_error()));
        return;
    }
    console.request(line,
                    [reply](MplaneResult r)
                    {
                        if (!r.ok)
                        {
                            reply(error_result<std::string>(
                                mplane_err_string(r.error)));
                            return;
                        }
                        reply(value_result(r.text()));
                    });
}

void L3Impl::set_config_slot(uint8_t slot, Reply<Empty> reply)
{
    if (!radio_created)
    {
        reply(error_result<Empty>(k_enoent));
        return;
    }
    if (!console_ok)
    {
        reply(error_result<Empty>(radio_unavailable_error()));
        return;
    }
    console.send_save_slot(
        slot,
        [this, slot, reply](MplaneResult r1)
        {
            if (!r1.ok)
            {
                reply(error_result<Empty>(mplane_err_string(r1.error)));
                return;
            }
            console.send_load_slot(
                slot,
                [this, slot, reply](MplaneResult r2)
                {
                    if (!r2.ok)
                    {
                        reply(error_result<Empty>(mplane_err_string(r2.error)));
                        return;
                    }
                    cfg.config_slot = slot;
                    reply(value_result(Empty{}));
                });
        });
}

void L3Impl::on_radio_version(bool known, const WinjectVersion& v)
{
    radio_version_known = known;
    radio_version = v;
    radio_proto_ok = known && protocol_compatible(v, own_version());
    if (known && !radio_proto_ok)
    {
        log_proto_mismatch_once(v);
    }
    if (known && radio_proto_ok)
    {
        proto_mismatch_logged = false;
    }
}

void L3Impl::log_proto_mismatch_once(const WinjectVersion& radio_ver)
{
    if (proto_mismatch_logged)
    {
        return;
    }
    proto_mismatch_logged = true;
    const WinjectVersion mgr = own_version();
    LOG_ERR(
        "radio is v%u.%u.%u, manager is v%u.%u.%u: protocol %u.%u != %u.%u, "
        "not using its m-plane",
        static_cast<unsigned>(radio_ver.x), static_cast<unsigned>(radio_ver.y),
        static_cast<unsigned>(radio_ver.z), static_cast<unsigned>(mgr.x),
        static_cast<unsigned>(mgr.y), static_cast<unsigned>(mgr.z),
        static_cast<unsigned>(radio_ver.x), static_cast<unsigned>(radio_ver.y),
        static_cast<unsigned>(mgr.x), static_cast<unsigned>(mgr.y));
}

void L3Impl::reapply_radio_console(std::function<void(bool ok)> done)
{
    if (!console_ok || !radio)
    {
        if (done)
        {
            done(false);
        }
        return;
    }
    console.apply_radio(
        cfg, cfg.config_slot,
        [this, done = std::move(done)](MplaneResult r)
        {
            if (!r.ok)
            {
                if (r.is_eproto())
                {
                    radio_proto_ok = false;
                }
                if (done)
                {
                    done(false);
                }
                return;
            }
            radio_supervisor.on_phy_programmed();
            if (done)
            {
                done(true);
            }
        },
        [this](const RadioCapsReply& caps)
        {
            apply_radio_caps(caps);
        },
        [this](bool known, const WinjectVersion& v)
        {
            on_radio_version(known, v);
        });
}

bool L3Impl::apply_console()
{
    if (!hold_console())
    {
        return false;
    }
    reapply_radio_console(
        [this](bool ok)
        {
            if (!ok)
            {
                LOG_ERR("radio reapply failed");
                drop_console();
                return;
            }
            radio_programmed = true;
            radio_supervisor.on_phy_programmed();
        });
    return true;
}

bool L3Impl::hold_console()
{
    console.clear_pending();
    console_ok = true;
    if (!reactor->add_read_rdy(console.fd(),
                               [this]()
                               {
                                   on_console();
                               }))
    {
        LOG_ERR("console epoll add failed");
        return false;
    }
    reconnect_timer.cancel();
    radio_supervisor.on_console_up();
    return true;
}

void L3Impl::close_console()
{
    const int fd = console.fd();
    if (fd >= 0)
    {
        reactor->rem_read_rdy(fd);
        reactor->rem_write_rdy(fd);
    }
    radio_supervisor.on_console_down();
    console.cancel_pending();
    console.close();
    console_deadline_timer.cancel();
    console_ok = false;
    radio_programmed = false;
}

void L3Impl::drop_console()
{
    close_console();
    if (radio_added && !reconnect_timer.armed())
    {
        schedule_reconnect(k_reconnect_interval);
    }
}

void L3Impl::schedule_reconnect(std::chrono::milliseconds after)
{
    reconnect_timer.arm(after,
                        [this]()
                        {
                            if (console_ok || !radio_added)
                            {
                                return;
                            }
                            LOG_INF("retrying console");
                            begin_console();
                        });
}

void L3Impl::on_console_request_deadline(
    std::chrono::steady_clock::time_point deadline)
{
    if (console_deadline_timer.armed() && console_deadline_at <= deadline)
    {
        return;
    }
    console_deadline_at = deadline;
    const auto after = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now());
    console_deadline_timer.arm(
        after,
        [this]()
        {
            console.poll_deadlines(std::chrono::steady_clock::now());
            arm_console_deadline();
        });
}

// After deadlines were polled: arm for the earliest one still pending. A
// timeout callback may already have armed for a later request; this moves
// the timer earlier if an older request is still due first.
void L3Impl::arm_console_deadline()
{
    if (const auto next = console.next_deadline())
    {
        on_console_request_deadline(*next);
    }
}

void L3Impl::begin_console()
{
    if (console_ok)
    {
        return;
    }
    std::string err;
    if (!console.start_connect(cfg.radio_device.mplane, &err) ||
        !console.finish_connect(&err))
    {
        LOG_ERR("console: %s", err.c_str());
        drop_console();
        return;
    }
    LOG_INF("console ready %s",
            mplane_format_ipv4_port(cfg.radio_device.mplane).c_str());
    if (!apply_console())
    {
        drop_console();
        return;
    }
    LOG_INF("console upstreams reapplied");
}

void L3Impl::on_console()
{
    if (!console_ok)
    {
        return;
    }
    char buf[2048];
    const ssize_t n = recv(console.fd(), buf, sizeof(buf), 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        return;
    }
    if (n < 0)
    {
        LOG_WRN("console recv: %s", strerror(errno));
        drop_console();
        return;
    }
    if (n > 0)
    {
        console.append_recv(buf, static_cast<size_t>(n));
    }

    console.poll_deadlines(std::chrono::steady_clock::now());
    std::vector<std::string> lines;
    std::string line;
    while (console.pop_line(&line))
    {
        lines.push_back(std::move(line));
    }
    console.on_lines(lines);
}

void L3Impl::arm_stats()
{
    if (cfg.stats_sec == 0)
    {
        return;
    }
    stats_timer.arm(
        std::chrono::seconds(cfg.stats_sec),
        [this]()
        {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_stats)
                    .count();
            last_stats = now;
            tx_mux.log_stats(elapsed_ms / 1000.0);
            arm_stats();
        });
}

void L3Impl::stop()
{
    stop_requested.store(true);
    if (!host_io)
    {
        reactor->stop();
        return;
    }
    // Any thread; core detaches on the reactor thread before l3 goes, so
    // this wake-up runs while l3 still exists (join waits for it).
    reactor->wake_up(
        [this]()
        {
            finish_on_host();
        });
}

void L3Impl::start(winject::l3::detail::L3Core* l3_core,
                   const L3::ThreadSetup& thread_setup)
{
    core = l3_core;
    LOG_INF("winject-manager %s", WINJECT_VERSION_STRING);
    auto started = std::make_shared<std::promise<void>>();
    auto ready = started->get_future();
    upstream_reactor->wake_up(
        [started]
        {
            started->set_value();
        });
    if (!host_upstream)
    {
        if (thread_setup)
        {
            thread_setup(L3::Thread::upstream);
        }
        upstream_reactor_thread = std::thread(
            [cv = upstream_reactor]
            {
                cv->run();
            });
    }
    // Also waits until a host's upstream reactor runs.
    ready.wait();
    if (host_io)
    {
        // Setup and attach on the reactor thread, so a stop() (a wake-up
        // once attached) runs after setup. A stop() before attach only set
        // stop_requested: finish here. Returns attached, like a thread of
        // l3's own.
        auto attached = std::make_shared<std::promise<void>>();
        auto is_attached = attached->get_future();
        reactor->wake_up(
            [this, attached]()
            {
                setup();
                core->attach(reactor.get(), this);
                attached->set_value();
                if (stop_requested.load() || core->stop_requested.load())
                {
                    finish_on_host();
                }
            });
        is_attached.wait();
        return;
    }
    core->attach(reactor.get(), this);
    if (thread_setup)
    {
        thread_setup(L3::Thread::l3);
    }
    l3_thread = std::thread(
        [this]
        {
            loop();
        });
}

namespace
{
// Returns once r has run every wake-up queued before this one.
template <typename Reactor>
void drain(Reactor& r)
{
    auto ran = std::make_shared<std::promise<void>>();
    auto done = ran->get_future();
    r.wake_up(
        [ran]()
        {
            ran->set_value();
        });
    done.wait();
}
}  // namespace

void L3Impl::join()
{
    if (l3_thread.joinable())
    {
        l3_thread.join();
    }
    if (host_io)
    {
        host_done.get_future().wait();
        // Wake-ups l3 queued before its teardown (radio events, stop) hold
        // pointers into l3: run them before it goes.
        drain(*reactor);
    }
    if (host_upstream)
    {
        drain(*upstream_reactor);
    }
}

void L3Impl::loop()
{
    setup();
    reactor->run();
    finish();
}

void L3Impl::finish()
{
    // Calls posted from here on get ESRCH; the ones still pending get
    // ECANCELED, also those whose wake-up the stopped loop will never run.
    core->detach_and_cancel();
    begin_shutdown();
    if (!host_upstream)
    {
        upstream_reactor->stop();
        upstream_reactor_thread.join();
    }
    core->callbacks.close();
}

void L3Impl::finish_on_host()
{
    if (host_finished)
    {
        return;
    }
    host_finished = true;
    core->detach_and_cancel();
    begin_shutdown();
    // The host reactor keeps running: take l3's fds and timers off it.
    close_console();
    reconnect_timer.cancel();
    stats_timer.cancel();
    if (registered_stop_fd >= 0)
    {
        reactor->rem_read_rdy(registered_stop_fd);
        registered_stop_fd = -1;
    }
    if (!host_upstream)
    {
        upstream_reactor->stop();
        upstream_reactor_thread.join();
    }
    core->callbacks.close();
    host_done.set_value();
}

void L3Impl::setup()
{
    reconnect_timer.bind(reactor.get());
    console_deadline_timer.bind(reactor.get());
    stats_timer.bind(reactor.get());
    console.set_on_deadline(
        [this](std::chrono::steady_clock::time_point deadline)
        {
            on_console_request_deadline(deadline);
        });
    radio_supervisor.set_console_ops(
        [this](std::function<void(bool, const RadioPhyView&)> done)
        {
            if (!console_ok)
            {
                done(false, RadioPhyView{});
                return;
            }
            console.query_radio_info(
                [this, done](RadioPhyReply r)
                {
                    if (!r.phy)
                    {
                        done(false, RadioPhyView{});
                        return;
                    }
                    radio_supervisor.set_actual_phy(*r.phy);
                    done(true, *r.phy);
                });
        },
        [this](std::function<void(bool ok)> done)
        {
            reapply_radio_console(std::move(done));
        },
        [this](std::function<void(bool ok)> done)
        {
            console.send_ping(
                [done = std::move(done)](MplaneResult r)
                {
                    if (done)
                    {
                        done(r.ok);
                    }
                });
        },
        [this]()
        {
            drop_console();
        });
    radio_supervisor.set_reconcile_filter(
        [this](std::function<void(bool ok)> done)
        {
            if (!console_ok)
            {
                done(false);
                return;
            }
            console.send_rx_filter(
                cfg.domain,
                [this, done = std::move(done)](MplaneResult r)
                {
                    if (!r.ok)
                    {
                        done(false);
                        return;
                    }
                    console.send_save_slot(
                        cfg.config_slot,
                        [done = std::move(done)](MplaneResult r2)
                        {
                            done(r2.ok);
                        });
                });
        });
    const int stop_fd = core->stop_fd();
    if (stop_fd >= 0)
    {
        char buf[16];
        while (::read(stop_fd, buf, sizeof(buf)) > 0)
        {
        }
        registered_stop_fd = stop_fd;
        reactor->add_read_rdy(
            stop_fd,
            [this, stop_fd]()
            {
                char drain[16];
                while (::read(stop_fd, drain, sizeof(drain)) > 0)
                {
                }
                stop();
            });
    }
    last_stats = std::chrono::steady_clock::now();
    if (cfg.stats_sec > 0)
    {
        LOG_INF(
            "stats logging every %u s (WINJECT_STATS_SEC or winject.stats_sec)",
            cfg.stats_sec);
    }
    arm_stats();
    // stop() before reactor->run() set it running would be lost; the first
    // loop pass runs this and stops then.
    // (A host reactor already runs: start() checks after setup instead.)
    if (!host_io)
    {
        reactor->get_timer().wait_us(
            0,
            [this]()
            {
                if (stop_requested.load() || core->stop_requested.load())
                {
                    reactor->stop();
                }
            });
    }
}

void L3Impl::begin_shutdown()
{
    tx_mux.stop();
    radio_upstream_table.clear();
    upstreams.clear();
    close_radio();
}

}  // namespace winject::l3::detail
