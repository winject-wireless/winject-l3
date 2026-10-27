#include "App.h"

#include "WinjectBuildVersion.h"
#include "console/ConsoleParse.h"
#include "console/MplaneCorrelation.h"
#include "console/MplaneErrno.h"
#include "frames/Mpdu.h"
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

namespace winject
{

App::~App()
{
    tx_mux.stop();
    radio_upstream_table.clear();
    upstreams.clear();
    upstream_reactor->stop();
    if (upstream_reactor_thread.joinable())
    {
        upstream_reactor_thread.join();
    }
    close_radio();
}

void App::close_radio()
{
    // A queue d-plane's host reactor outlives the App and keeps delivering;
    // close before rx_demux goes, even if a pending TX event holds the radio.
    if (radio)
    {
        radio->close();
    }
}

bool App::load(Config config)
{
    std::string err;
    if (!config.validate(&err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    cfg = std::move(config);
    return true;
}

bool App::add_upstream(const UpstreamConfig& uc)
{
    if (!radio)
    {
        return false;
    }
    auto tx_queue = tx_mux.make_tx_queue();
    std::shared_ptr<Upstream> up;
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

bool App::setup_radio()
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
            LOG_ERR("radio queue reactor %s: not registered", d.reactor.c_str());
            return false;
        }
        auto queue = std::make_shared<WifiQueue>();
        if (!queue->open(*reactor, std::move(on_rx)) ||
            !queue->bind_dplane(
                WifiQueueDplaneBinding{d.dplane_tx, d.dplane_rx, d.reactor}))
        {
            return false;
        }
        radio = std::move(queue);
        dplane = "queue:" + d.dplane_tx + "," + d.dplane_rx + "@" + d.reactor;
    }
    else
    {
        radio = std::make_shared<WifiUdp>();
        if (!radio->open(*reactor, std::move(on_rx)))
        {
            return false;
        }
        dplane = console_format_ipv4_port(d.dplane);
    }
    apply_radio_dplane();
    apply_configured_fcs();
    LOG_INF("radio dplane=%s fcs=%s", dplane.c_str(),
            console_format_radio_fcs(d.fcs));
    return true;
}

void App::apply_radio_dplane()
{
    const RadioDeviceConfig& d = cfg.radio_device;
    if (radio == nullptr)
    {
        return;
    }
    tx_mux.post_send_socket_change(radio, d.dplane);
    radio->post_rx_event(EventCtrlRecvSocketChange{d.dplane});
}

void App::apply_configured_fcs()
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

bool App::setup_upstreams()
{
    if (!setup_radio())
    {
        return false;
    }
    radio_manager.bind(&cfg, &tx_mux, radio.get(), reactor.get());
    radio_manager.configure_tx_mux();
    rx_demux.set_domain(cfg.domain);
    for (const auto& uc : cfg.upstreams)
    {
        if (!add_upstream(uc))
        {
            return false;
        }
    }
    return true;
}

std::shared_ptr<Upstream> App::construct_upstream(
    const UpstreamConfig& uc, const std::shared_ptr<BoundedLossyQueue>& tx_queue,
    const Upstream::TxSequences* resume)
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
    // on_upstream_queue (Config::validate requires the callback).
    if (uc.endpoint.rx.empty() && uc.endpoint.tx.empty() &&
        uc.reactor.empty() && cfg.on_upstream_queue)
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
    if (cfg.on_upstream_queue)
    {
        cfg.on_upstream_queue(uc.index, up->app_transport());
    }
    return up;
}

bool App::replace_upstream(size_t index, const UpstreamConfig& uc,
                           std::string* error)
{
    const auto original = cfg.upstreams[index];
    {
        std::lock_guard<std::mutex> lock(radio_upstream_table.mutex());
        radio_upstream_table.entries()[index] = {};
    }
    // The peer's sequencers see one unbroken stream across the swap.
    std::optional<Upstream::TxSequences> sequences;
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

bool App::set_upstream_scheduler_budget(size_t index, size_t budget,
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

bool App::fill_upstream_view(size_t index, ManagerUpstreamView* out) const
{
    if (out == nullptr || index >= upstreams.size() ||
        index >= cfg.upstreams.size())
    {
        return false;
    }
    const UpstreamConfig& uc = cfg.upstreams[index];
    const auto* endpoint = upstreams[index].get();
    out->id = static_cast<uint8_t>(uc.index);
    out->bus_tx = uc.bus_tx;
    out->bus_rx = uc.bus_rx;
    out->type = uc.mode == UpstreamMode::queue ? "QUEUE" : "UDP";
    if (endpoint != nullptr)
    {
        out->rx = endpoint->endpoint_rx();
        out->tx = endpoint->endpoint_tx();
        out->fec_timeout_ms = endpoint->fec_timeout_ms();
        endpoint->get_fec(&out->fec, &out->fec_k, &out->fec_n);
    }
    else
    {
        out->rx = uc.endpoint.rx;
        out->tx = uc.endpoint.tx;
        out->fec = uc.fec_type;
        out->fec_k = uc.fec_k;
        out->fec_n = uc.fec_n;
        out->fec_timeout_ms = uc.fec_timeout_ms;
    }
    size_t budget = 0;
    if (radio_upstream_table.get_budget(index, &budget))
    {
        out->quanta = budget;
    }
    else
    {
        out->quanta = uc.scheduler_budget;
    }
    return true;
}

namespace
{

constexpr size_t k_upstream_not_found = static_cast<size_t>(-1);

size_t find_upstream_vec_index(const Config& cfg, uint8_t id)
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

}  // namespace

bool App::console_add_upstream(const ManagerUpstreamView& spec,
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
    if (spec.bus_tx == 0 && spec.bus_rx == 0)
    {
        return fail(k_einval);
    }
    for (const auto& prev : cfg.upstreams)
    {
        if (prev.index == spec.id)
        {
            return fail(k_eexist);
        }
    }
    UpstreamConfig uc;
    uc.index = spec.id;
    uc.bus_tx = spec.bus_tx;
    uc.bus_rx = spec.bus_rx;
    uc.scheduler_budget = spec.quanta;
    uc.endpoint = EndpointAddress{spec.rx, spec.tx};
    uc.mode = upstream_mode(uc.endpoint);
    uc.fec_type = spec.fec;
    uc.fec_k = spec.fec_k;
    uc.fec_n = spec.fec_n;
    uc.fec_timeout_ms = spec.fec_timeout_ms;
    if (uc.fec_type == FecType::none)
    {
        uc.fec_k = 0;
        uc.fec_n = 0;
    }
    std::vector<UpstreamConfig> candidate = cfg.upstreams;
    candidate.push_back(uc);
    std::string val_err;
    if (!Config::validate_upstreams(candidate, &val_err))
    {
        return fail(k_einval);
    }
    cfg.upstreams.push_back(uc);
    if (!add_upstream(uc))
    {
        cfg.upstreams.pop_back();
        return fail(k_eio);
    }
    return true;
}

bool App::console_remove_upstream(uint8_t id, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    const size_t index = find_upstream_vec_index(cfg, id);
    if (index == k_upstream_not_found)
    {
        return fail(k_enoent);
    }
    if (!radio_upstream_table.remove(index))
    {
        return fail(k_enoent);
    }
    upstreams.erase(upstreams.begin() + static_cast<ptrdiff_t>(index));
    tx_mux.request_tick();
    cfg.upstreams.erase(cfg.upstreams.begin() + static_cast<ptrdiff_t>(index));
    metrics_registry.remove_prefix("upstream_" + std::to_string(id) + "_");
    return true;
}

bool App::console_list_upstream(const std::vector<uint8_t>& ids,
                                std::vector<ManagerUpstreamView>* out,
                                std::string* error) const
{
    if (out == nullptr)
    {
        if (error != nullptr)
        {
            *error = k_einval;
        }
        return false;
    }
    out->clear();
    if (ids.empty())
    {
        for (size_t i = 0; i < upstreams.size(); i++)
        {
            ManagerUpstreamView row;
            if (!fill_upstream_view(i, &row))
            {
                continue;
            }
            out->push_back(row);
        }
        return true;
    }
    for (uint8_t id : ids)
    {
        const size_t index = find_upstream_vec_index(cfg, id);
        if (index == k_upstream_not_found)
        {
            if (error != nullptr)
            {
                *error = k_enoent;
            }
            return false;
        }
        ManagerUpstreamView row;
        if (!fill_upstream_view(index, &row))
        {
            if (error != nullptr)
            {
                *error = k_enoent;
            }
            return false;
        }
        out->push_back(row);
    }
    return true;
}

bool App::console_update_upstream(const ManagerUpstreamUpdate& patch,
                                  ManagerUpstreamView* out, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    const size_t index = find_upstream_vec_index(cfg, patch.id);
    if (index == k_upstream_not_found)
    {
        return fail(k_enoent);
    }
    if (!patch.have_fec && !patch.have_k && !patch.have_n &&
        !patch.have_fec_timeout && !patch.have_quanta)
    {
        return fail(k_einval);
    }
    UpstreamConfig candidate;
    std::string val_err;
    if (!Config::validate_upstream_update(
            cfg.upstreams[index], patch.have_fec ? patch.fec : FecType::none,
            patch.fec_k, patch.fec_n, patch.fec_timeout_ms, patch.quanta,
            patch.have_fec, patch.have_k, patch.have_n, patch.have_fec_timeout,
            patch.have_quanta, cfg.upstreams, &candidate, &val_err))
    {
        return fail(k_einval);
    }
    if (patch.have_fec || patch.have_k || patch.have_n ||
        patch.have_fec_timeout)
    {
        if (!replace_upstream(index, candidate, error))
        {
            return false;
        }
    }
    else if (patch.have_quanta && !set_upstream_scheduler_budget(
                                      index, candidate.scheduler_budget, error))
    {
        return false;
    }
    cfg.upstreams[index] = candidate;
    if (out == nullptr || !fill_upstream_view(index, out))
    {
        return fail(k_enoent);
    }
    return true;
}

namespace
{

std::vector<uint8_t> console_upstream_ids(const std::vector<uint8_t>& ids,
                                          const Config& cfg)
{
    if (!ids.empty())
    {
        return ids;
    }
    std::vector<uint8_t> want;
    for (const UpstreamConfig& u : cfg.upstreams)
    {
        want.push_back(static_cast<uint8_t>(u.index));
    }
    return want;
}

void fill_upstream_fec_fields(const Upstream* endpoint, FecType* fec, int* k,
                              int* n)
{
    if (fec == nullptr || k == nullptr || n == nullptr)
    {
        return;
    }
    if (endpoint != nullptr)
    {
        endpoint->get_fec(fec, k, n);
        return;
    }
    *fec = FecType::none;
    *k = 0;
    *n = 0;
}

}  // namespace

bool App::console_list_upstream_rx_stat(
    const std::vector<uint8_t>& ids,
    std::vector<ManagerUpstreamRxStatView>* out, std::string* error) const
{
    if (out == nullptr)
    {
        if (error != nullptr)
        {
            *error = k_einval;
        }
        return false;
    }
    out->clear();
    const std::vector<uint8_t> want = console_upstream_ids(ids, cfg);
    for (uint8_t id : want)
    {
        const size_t index = find_upstream_vec_index(cfg, id);
        if (index == k_upstream_not_found)
        {
            if (error != nullptr)
            {
                *error = k_enoent;
            }
            return false;
        }
        const auto* endpoint = upstreams[index].get();
        ManagerUpstreamRxStatView row;
        row.id = id;
        fill_upstream_fec_fields(endpoint, &row.fec, &row.fec_k, &row.fec_n);
        if (endpoint != nullptr)
        {
            row.rxbyt = endpoint->app_rx_bytes();
            row.rxpkt = endpoint->app_rx_packets();
            row.rx_oversize = endpoint->app_rx_oversize_pkt();
            row.fec_rec = endpoint->fec_recovered();
            row.fec_lost = endpoint->fec_decode_fail();
            row.fec_rxbyt = endpoint->fec_air_rx_bytes();
            row.fec_rxpkt = endpoint->fec_air_rx_packets();
        }
        uint64_t gap = 0;
        if (radio_upstream_table.peek_seq_lost(index, &gap))
        {
            row.rxgap = gap;
        }
        out->push_back(row);
    }
    return true;
}

bool App::console_list_upstream_tx_stat(
    const std::vector<uint8_t>& ids,
    std::vector<ManagerUpstreamTxStatView>* out, std::string* error) const
{
    if (out == nullptr)
    {
        if (error != nullptr)
        {
            *error = k_einval;
        }
        return false;
    }
    out->clear();
    const std::vector<uint8_t> want = console_upstream_ids(ids, cfg);
    for (uint8_t id : want)
    {
        const size_t index = find_upstream_vec_index(cfg, id);
        if (index == k_upstream_not_found)
        {
            if (error != nullptr)
            {
                *error = k_enoent;
            }
            return false;
        }
        const auto* endpoint = upstreams[index].get();
        ManagerUpstreamTxStatView row;
        row.id = id;
        fill_upstream_fec_fields(endpoint, &row.fec, &row.fec_k, &row.fec_n);
        if (endpoint != nullptr)
        {
            row.txbyt = endpoint->app_tx_bytes();
            row.txpkt = endpoint->app_tx_packets();
            row.fec_txbyt = endpoint->fec_air_tx_bytes();
            row.fec_txpkt = endpoint->fec_air_tx_packets();
            endpoint->tx_pending_stats(&row.tx_pending_pkt,
                                       &row.tx_pending_byt);
        }
        out->push_back(row);
    }
    return true;
}

void App::refresh_host_metrics()
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
        const Upstream* up = upstreams[i].get();
        if (up == nullptr)
        {
            continue;
        }
        ManagerUpstreamView view;
        if (!fill_upstream_view(i, &view))
        {
            continue;
        }
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

bool App::console_get_metrics(const std::vector<std::string>& keys,
                              std::vector<ManagerMetricView>* out,
                              std::string* error)
{
    if (out == nullptr)
    {
        if (error != nullptr)
        {
            *error = k_einval;
        }
        return false;
    }
    refresh_host_metrics();
    const std::map<std::string, Metrics> snapshot =
        metrics_registry.getMetrics(keys);
    out->clear();
    const auto want_key = [&](const std::string& k) -> bool
    {
        return keys.empty() ||
               std::find(keys.begin(), keys.end(), k) != keys.end();
    };
    if (want_key("radio_version"))
    {
        ManagerMetricView row;
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
        out->push_back(std::move(row));
    }
    for (const auto& entry : snapshot)
    {
        ManagerMetricView row;
        row.key = entry.first;
        row.value = metric_value_to_string(entry.second);
        out->push_back(std::move(row));
    }
    return true;
}

namespace
{

std::string mplane_join_body(const MplaneResult& r)
{
    std::string body;
    for (const auto& line : r.body_lines)
    {
        if (!body.empty())
        {
            body += '\n';
        }
        body += line;
    }
    if (body.empty() && !r.payload.empty())
    {
        body = r.payload;
    }
    return body;
}

}  // namespace

void App::apply_radio_caps_mplane(const MplaneResult& r)
{
    if (radio == nullptr)
    {
        return;
    }
    RadioFcsMode mode = RadioFcsMode::unknown;
    bool legacy = false;
    std::string err;
    if (!resolve_radio_caps_mplane(r, &mode, &legacy, &err))
    {
        LOG_WRN("radio_caps_info: %s", err.c_str());
        return;
    }
    if (legacy)
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

void App::send_radio_unavailable_nok(ManagerConsoleReply reply) const
{
    if (radio_version_known && !radio_proto_ok)
    {
        reply.send_nok(k_eproto);
        return;
    }
    reply.send_nok(k_enodev);
}

void App::on_radio_version(bool known, const WinjectVersion& v)
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

void App::log_proto_mismatch_once(const WinjectVersion& radio_ver)
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

bool App::console_radio_device(const ManagerRadioDeviceUpdate& patch,
                               ManagerRadioDeviceView* out, std::string* error)
{
    if (patch.id != 0)
    {
        if (error != nullptr)
        {
            *error = k_einval;
        }
        return false;
    }
    if (patch.have_dplane && cfg.radio_device.dplane_is_queue())
    {
        // A queue d-plane is fixed at startup.
        if (error != nullptr)
        {
            *error = k_einval;
        }
        return false;
    }
    RadioDeviceConfig next = cfg.radio_device;
    if (patch.have_mplane)
    {
        next.mplane = patch.mplane;
    }
    if (patch.have_dplane)
    {
        next.dplane = patch.dplane;
    }
    if (patch.have_fcs)
    {
        next.fcs = patch.fcs;
    }
    const bool mplane_changed =
        patch.have_mplane &&
        (cfg.radio_device.mplane.sin_addr.s_addr !=
             patch.mplane.sin_addr.s_addr ||
         cfg.radio_device.mplane.sin_port != patch.mplane.sin_port);
    const bool dplane_changed =
        patch.have_dplane &&
        (cfg.radio_device.dplane.sin_addr.s_addr !=
             patch.dplane.sin_addr.s_addr ||
         cfg.radio_device.dplane.sin_port != patch.dplane.sin_port);
    const bool fcs_changed =
        patch.have_fcs && cfg.radio_device.fcs != patch.fcs;
    cfg.radio_device = next;
    if (dplane_changed)
    {
        apply_radio_dplane();
    }
    if (mplane_changed)
    {
        drop_console();
        radio_version_known = false;
        radio_proto_ok = false;
        apply_configured_fcs();
        schedule_reconnect(std::chrono::milliseconds(0));
    }
    else if (fcs_changed)
    {
        if (console_ok && cfg.radio_device.fcs == RadioFcsConfig::auto_detect)
        {
            console.query_radio_caps(
                [this](MplaneResult r)
                {
                    apply_radio_caps_mplane(r);
                });
        }
        else if (!console_ok)
        {
            apply_configured_fcs();
        }
    }
    if (out != nullptr)
    {
        out->id = 0;
        out->device = cfg.radio_device;
    }
    return true;
}

void App::console_radio_info(ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        send_radio_unavailable_nok(reply);
        return;
    }
    console.query_radio_info(
        [this, reply](MplaneResult r)
        {
            if (!r.ok)
            {
                reply.send_nok(mplane_err_string(r.error).c_str());
                return;
            }
            const std::string body = mplane_join_body(r);
            ManagerRadioView view;
            if (parse_radio_info_body(body, &view))
            {
                radio_manager.set_actual_phy(view);
            }
            reply.send_text(body);
        });
}

void App::console_radio_caps_info(ManagerConsoleReply reply)
{
    if (console_ok)
    {
        console.query_radio_caps(
            [this, reply](MplaneResult r)
            {
                if (!r.ok)
                {
                    const std::string& e = r.error;
                    if (e != "ENOSYS" && e.find("ENOSYS") == std::string::npos)
                    {
                        reply.send_nok(mplane_err_string(r.error).c_str());
                        return;
                    }
                }
                apply_radio_caps_mplane(r);
                if (radio == nullptr ||
                    radio->fcs_mode() == RadioFcsMode::unknown)
                {
                    reply.send_nok(k_enodata);
                    return;
                }
                reply.send_text(format_radio_caps_ok_line(radio->fcs_mode()));
            });
        return;
    }
    if (radio == nullptr)
    {
        reply.send_nok(k_enodev);
        return;
    }
    if (radio->fcs_mode() == RadioFcsMode::unknown)
    {
        reply.send_nok(k_enodata);
        return;
    }
    reply.send_text(format_radio_caps_ok_line(radio->fcs_mode()));
}

void App::console_radio_stats(ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        send_radio_unavailable_nok(reply);
        return;
    }
    console.query_radio_counters(
        [reply](MplaneResult r)
        {
            if (!r.ok)
            {
                reply.send_nok(mplane_err_string(r.error).c_str());
                return;
            }
            reply.send_text(mplane_join_body(r));
        });
}

void App::console_radio_tx(const ManagerRadioUpdate& patch,
                           ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        send_radio_unavailable_nok(reply);
        return;
    }
    std::string kv;
    if (patch.have_channel)
    {
        kv += "channel=" + std::to_string(patch.channel);
    }
    if (patch.have_tx_power)
    {
        if (!kv.empty())
        {
            kv += ' ';
        }
        kv += "tx_power=" + std::to_string(patch.tx_power);
    }
    if (patch.have_modulation)
    {
        const std::string canonical =
            Config::canonical_modulation(patch.modulation);
        if (canonical.empty() ||
            !Config::modulation_ok_for_channel(
                canonical, patch.have_channel ? patch.channel : cfg.channel))
        {
            reply.send_nok(k_einval);
            return;
        }
        if (!kv.empty())
        {
            kv += ' ';
        }
        kv += "modulation=" + canonical;
    }
    if (patch.have_cca)
    {
        if (!kv.empty())
        {
            kv += ' ';
        }
        kv += patch.cca ? "cca=1" : "cca=0";
    }
    console.send_radio_tx(
        kv,
        [this, patch, reply](MplaneResult r1)
        {
            if (!r1.ok)
            {
                reply.send_nok(mplane_err_string(r1.error).c_str());
                return;
            }
            if (patch.have_channel)
            {
                cfg.channel = static_cast<uint8_t>(patch.channel);
            }
            if (patch.have_tx_power)
            {
                cfg.power_dbm = static_cast<int8_t>(patch.tx_power);
            }
            if (patch.have_modulation)
            {
                cfg.modulation = Config::canonical_modulation(patch.modulation);
                radio_manager.sync_pacing_for_modulation(cfg.modulation);
            }
            if (patch.have_cca)
            {
                cfg.cca = patch.cca;
                cfg.cca_explicit = true;
            }
            ManagerRadioView view;
            if (parse_radio_info_body(mplane_join_body(r1), &view))
            {
                radio_manager.set_actual_phy(view);
            }
            console.send_save_slot(
                cfg.config_slot,
                [r1, reply](MplaneResult r2)
                {
                    if (!r2.ok)
                    {
                        reply.send_nok(mplane_err_string(r2.error).c_str());
                        return;
                    }
                    reply.send_text(mplane_join_body(r1));
                });
        });
}

void App::console_radio_reset(ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        send_radio_unavailable_nok(reply);
        return;
    }
    console.send_radio_reset(
        [reply](MplaneResult r)
        {
            if (!r.ok)
            {
                reply.send_nok(mplane_err_string(r.error).c_str());
                return;
            }
            reply.send_text("");
        });
}

void App::console_config_slot(uint8_t slot, ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        send_radio_unavailable_nok(reply);
        return;
    }
    console.send_save_slot(
        slot,
        [this, slot, reply](MplaneResult r1)
        {
            if (!r1.ok)
            {
                reply.send_nok(mplane_err_string(r1.error).c_str());
                return;
            }
            console.send_load_slot(
                slot,
                [this, slot, reply](MplaneResult r2)
                {
                    if (!r2.ok)
                    {
                        reply.send_nok(mplane_err_string(r2.error).c_str());
                        return;
                    }
                    cfg.config_slot = slot;
                    reply.send_text("");
                });
        });
}

bool App::start_manager_console()
{
    if (cfg.console_in.empty())
    {
        return true;
    }
    sockaddr_in in_addr = {};
    if (!parse_host_port(cfg.console_in, &in_addr))
    {
        LOG_ERR("manager console: invalid winject.console_in");
        return false;
    }
    ManagerConsoleHandlers handlers;
    handlers.add_upstream =
        [this](const ManagerUpstreamView& spec, std::string* error)
    {
        return console_add_upstream(spec, error);
    };
    handlers.remove_upstream = [this](uint8_t id, std::string* error)
    {
        return console_remove_upstream(id, error);
    };
    handlers.list_upstream = [this](const std::vector<uint8_t>& ids,
                                    std::vector<ManagerUpstreamView>* out,
                                    std::string* error)
    {
        return console_list_upstream(ids, out, error);
    };
    handlers.update_upstream = [this](const ManagerUpstreamUpdate& patch,
                                      ManagerUpstreamView* out,
                                      std::string* error)
    {
        return console_update_upstream(patch, out, error);
    };
    handlers.list_upstream_rx_stat =
        [this](const std::vector<uint8_t>& ids,
               std::vector<ManagerUpstreamRxStatView>* out, std::string* error)
    {
        return console_list_upstream_rx_stat(ids, out, error);
    };
    handlers.list_upstream_tx_stat =
        [this](const std::vector<uint8_t>& ids,
               std::vector<ManagerUpstreamTxStatView>* out, std::string* error)
    {
        return console_list_upstream_tx_stat(ids, out, error);
    };
    handlers.get_metrics = [this](const std::vector<std::string>& keys,
                                  std::vector<ManagerMetricView>* out,
                                  std::string* error)
    {
        return console_get_metrics(keys, out, error);
    };
    handlers.radio_info = [this](ManagerConsoleReply reply)
    {
        console_radio_info(reply);
    };
    handlers.radio_caps_info = [this](ManagerConsoleReply reply)
    {
        console_radio_caps_info(reply);
    };
    handlers.radio_stats = [this](ManagerConsoleReply reply)
    {
        console_radio_stats(reply);
    };
    handlers.radio_reset = [this](ManagerConsoleReply reply)
    {
        console_radio_reset(reply);
    };
    handlers.radio_tx =
        [this](const ManagerRadioUpdate& patch, ManagerConsoleReply reply)
    {
        console_radio_tx(patch, reply);
    };
    handlers.config_slot = [this](uint8_t slot, ManagerConsoleReply reply)
    {
        console_config_slot(slot, reply);
    };
    handlers.radio_device = [this](const ManagerRadioDeviceUpdate& patch,
                                   ManagerRadioDeviceView* out,
                                   std::string* error)
    {
        return console_radio_device(patch, out, error);
    };
    std::string err;
    if (!mgr_console.start(*reactor, in_addr, std::move(handlers), &err))
    {
        LOG_ERR("manager console: %s", err.c_str());
        return false;
    }
    return true;
}

void App::reapply_radio_console(std::function<void(bool ok)> done)
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
                if (r.error == "EPROTO")
                {
                    radio_proto_ok = false;
                }
                if (done)
                {
                    done(false);
                }
                return;
            }
            radio_manager.on_phy_programmed();
            if (done)
            {
                done(true);
            }
        },
        [this](const MplaneResult& caps)
        {
            apply_radio_caps_mplane(caps);
        },
        [this](bool known, const WinjectVersion& v)
        {
            on_radio_version(known, v);
        });
}

bool App::apply_console()
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
            radio_manager.on_phy_programmed();
        });
    return true;
}

bool App::hold_console()
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
    radio_manager.on_console_up();
    return true;
}

void App::drop_console()
{
    const int fd = console.fd();
    if (fd >= 0)
    {
        reactor->rem_read_rdy(fd);
        reactor->rem_write_rdy(fd);
    }
    radio_manager.on_console_down();
    console.cancel_pending();
    console.close();
    console_deadline_timer.cancel();
    console_ok = false;
    if (!reconnect_timer.armed())
    {
        schedule_reconnect(k_reconnect_interval);
    }
}

void App::schedule_reconnect(std::chrono::milliseconds after)
{
    reconnect_timer.arm(after,
                         [this]()
                         {
                             if (console_ok)
                             {
                                 return;
                             }
                             LOG_INF("retrying console");
                             begin_console();
                         });
}

void App::on_console_request_deadline(
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
void App::arm_console_deadline()
{
    if (const auto next = console.next_deadline())
    {
        on_console_request_deadline(*next);
    }
}

void App::begin_console()
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
            console_format_ipv4_port(cfg.radio_device.mplane).c_str());
    if (!apply_console())
    {
        drop_console();
        return;
    }
    LOG_INF("console upstreams reapplied");
}

void App::on_console()
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

void App::arm_stats()
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

void App::stop()
{
    stop_requested.store(true);
    reactor->stop();
}

void App::begin_shutdown()
{
    mgr_console.stop();
    tx_mux.stop();
    radio_upstream_table.clear();
    upstreams.clear();
    close_radio();
}

int App::run()
{
    LOG_INF("winject-manager %s", WINJECT_VERSION_STRING);
    auto started = std::make_shared<std::promise<void>>();
    auto ready = started->get_future();
    upstream_reactor->wake_up(
        [started]
        {
            started->set_value();
        });
    upstream_reactor_thread = std::thread(
        [cv = upstream_reactor]
        {
            cv->run();
        });
    ready.wait();
    if (!setup_upstreams())
    {
        return 1;
    }
    reconnect_timer.bind(reactor.get());
    console_deadline_timer.bind(reactor.get());
    stats_timer.bind(reactor.get());
    console.set_on_deadline(
        [this](std::chrono::steady_clock::time_point deadline)
        {
            on_console_request_deadline(deadline);
        });
    radio_manager.set_console_ops(
        [this](std::function<void(bool, const ManagerRadioView&)> done)
        {
            if (!console_ok)
            {
                done(false, ManagerRadioView{});
                return;
            }
            console.query_radio_info(
                [this, done](MplaneResult r)
                {
                    ManagerRadioView view;
                    if (!r.ok)
                    {
                        done(false, view);
                        return;
                    }
                    const std::string body = mplane_join_body(r);
                    if (!parse_radio_info_body(body, &view))
                    {
                        done(false, view);
                        return;
                    }
                    radio_manager.set_actual_phy(view);
                    done(true, view);
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
    radio_manager.set_reconcile_filter(
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
    if (!start_manager_console())
    {
        return 1;
    }
    begin_console();
    if (!console_ok)
    {
        LOG_WRN("waiting for radio m-plane %s",
                console_format_ipv4_port(cfg.radio_device.mplane).c_str());
    }
    ManagerRadioDeviceView rd_view;
    rd_view.id = 0;
    rd_view.device = cfg.radio_device;
    LOG_INF("manager running %s (%s)",
            console_format_radio_device(rd_view).c_str(),
            cfg.modulation.c_str());
    last_stats = std::chrono::steady_clock::now();
    if (cfg.stats_sec > 0)
    {
        LOG_INF(
            "stats logging every %u s (WINJECT_STATS_SEC or winject.stats_sec)",
            cfg.stats_sec);
    }
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
    arm_stats();
    // stop() before reactor->run() set it running would be lost; the first
    // loop pass runs this and stops then.
    reactor->get_timer().wait_us(0,
                                 [this]()
                                 {
                                     if (stop_requested.load())
                                     {
                                         reactor->stop();
                                     }
                                 });
    reactor->run();
    begin_shutdown();
    upstream_reactor->stop();
    upstream_reactor_thread.join();
    return 0;
}

}  // namespace winject
