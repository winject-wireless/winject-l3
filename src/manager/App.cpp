#include "App.h"

#include "console/ConsoleParse.h"
#include "endpoint/UdpEndpoint.h"
#include "frames/Mpdu.h"
#include "radio/RadioDefs.h"
#include "radio/RadioMplaneParse.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <thread>
#include <time.h>

namespace winject
{

namespace
{

bool mplane_nok_token(const std::string& err, std::string* token)
{
    const auto nok = err.find(" -> nok ");
    const auto nok_upper = err.find(" -> NOK ");
    const size_t pos =
        nok != std::string::npos
            ? nok
            : (nok_upper != std::string::npos ? nok_upper : std::string::npos);
    if (pos == std::string::npos)
    {
        return false;
    }
    *token = err.substr(pos + 8);
    return true;
}

}  // namespace

bool App::load(const std::string& path)
{
    std::string err;
    if (!cfg.load(path, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    if (!parse_host(cfg.device, &device_ip))
    {
        LOG_ERR("cannot resolve winject.device %s", cfg.device.c_str());
        return false;
    }
    if (!cfg.local_ip.empty() && !parse_host(cfg.local_ip, &local_ip))
    {
        LOG_ERR("invalid winject.local_ip");
        return false;
    }
    return true;
}

bool App::add_upstream(const UpstreamConfig& uc)
{
    if (!radio)
    {
        return false;
    }
    auto up = std::make_shared<UdpEndpoint>();
    if (!up->open(reactor, uc,
                  [this]()
                  {
                      tx_mux_.request_tick();
                  }))
    {
        return false;
    }
    radio_upstream_table_.add(up, radio, uc.bus_tx, uc.bus_rx,
                              uc.scheduler_budget);
    upstreams.push_back(up);
    LOG_INF("upstream-%zu bus_tx=%s bus_rx=%s", uc.index,
            uc.bus_tx ? bus_to_string(uc.bus_tx).c_str() : "-",
            uc.bus_rx ? bus_to_string(uc.bus_rx).c_str() : "-");
    return true;
}

bool App::setup_radio()
{
    radio = std::make_shared<WifiUdp>();
    sockaddr_in inject = {};
    inject.sin_family = AF_INET;
    inject.sin_addr = device_ip;
    inject.sin_port = htons(cfg.inject_port);
    if (!radio->open(
            reactor, inject, cfg.forward_port,
            [this](bfcext::shared_sized_buffer mpdu)
            {
                rx_demux_.on_mpdu(std::move(mpdu));
            },
            [this]()
            {
                tx_mux_.request_tick();
            }))
    {
        return false;
    }
    if (cfg.radio_fcs == RadioFcsConfig::signal)
    {
        radio->set_fcs_mode(RadioFcsMode::signal);
    }
    else if (cfg.radio_fcs == RadioFcsConfig::actual)
    {
        radio->set_fcs_mode(RadioFcsMode::actual);
    }
    LOG_INF("radio inject=%u forward=%u fcs=%s", radio->inject_port(),
            radio->forward_port(), radio_fcs_mode_name(radio->fcs_mode()));
    return true;
}

bool App::setup_upstreams()
{
    if (!setup_radio())
    {
        return false;
    }
    radio_manager_.bind(&cfg, &tx_mux_, radio.get());
    radio_manager_.configure_tx_mux();
    rx_demux_.set_domain(cfg.domain);
    for (const auto& uc : cfg.upstreams)
    {
        if (!add_upstream(uc))
        {
            return false;
        }
    }
    return true;
}

bool App::set_upstream_fec(size_t index, FecType type, int k, int n,
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
    if (index >= upstreams.size() || !upstreams[index])
    {
        return fail("invalid upstream index");
    }
    auto* udp = dynamic_cast<UdpEndpoint*>(upstreams[index].get());
    if (udp == nullptr)
    {
        return fail("fec only valid for UDP upstreams");
    }
    if (!udp->set_fec(type, k, n, error))
    {
        return false;
    }
    if (index < cfg.upstreams.size())
    {
        cfg.upstreams[index].fec_type = type;
        cfg.upstreams[index].fec_k = type == FecType::none ? 0 : k;
        cfg.upstreams[index].fec_n = type == FecType::none ? 0 : n;
    }
    return true;
}

bool App::get_upstream_fec(size_t index, FecType* type, int* k, int* n,
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
    if (index >= upstreams.size() || !upstreams[index])
    {
        return fail("invalid upstream index");
    }
    auto* udp = dynamic_cast<UdpEndpoint*>(upstreams[index].get());
    if (udp == nullptr)
    {
        return fail("fec only valid for UDP upstreams");
    }
    udp->get_fec(type, k, n);
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
    if (!radio_upstream_table_.set_budget(index, budget))
    {
        return fail("failed to set budget");
    }
    if (index < cfg.upstreams.size())
    {
        cfg.upstreams[index].scheduler_budget = budget;
    }
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
    const auto* udp = dynamic_cast<const UdpEndpoint*>(upstreams[index].get());
    out->id = static_cast<uint8_t>(uc.index);
    out->bus_tx = uc.bus_tx;
    out->bus_rx = uc.bus_rx;
    out->type = "UDP";
    if (udp != nullptr)
    {
        out->rx = udp->peer_endpoints().rx;
        out->tx = udp->peer_endpoints().tx;
        out->fec_timeout_ms = udp->fec_timeout_ms();
        udp->get_fec(&out->fec, &out->fec_k, &out->fec_n);
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
    if (radio_upstream_table_.get_budget(index, &budget))
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
        return fail("INVALID_ARGUMENT");
    }
    for (const auto& prev : cfg.upstreams)
    {
        if (prev.index == spec.id)
        {
            return fail("INVALID_ARGUMENT");
        }
    }
    UpstreamConfig uc;
    uc.index = spec.id;
    uc.bus_tx = spec.bus_tx;
    uc.bus_rx = spec.bus_rx;
    uc.scheduler_budget = spec.quanta;
    uc.endpoint = UdpPeerEndpoint{spec.rx, spec.tx};
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
        return fail("INVALID_ARGUMENT");
    }
    cfg.upstreams.push_back(uc);
    if (!add_upstream(uc))
    {
        cfg.upstreams.pop_back();
        return fail("INVALID_ARGUMENT");
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
        return fail("NOT_FOUND");
    }
    if (!radio_upstream_table_.remove(index))
    {
        return fail("NOT_FOUND");
    }
    if (upstreams[index])
    {
        auto* udp = dynamic_cast<UdpEndpoint*>(upstreams[index].get());
        if (udp != nullptr)
        {
            udp->close();
        }
    }
    upstreams.erase(upstreams.begin() + static_cast<ptrdiff_t>(index));
    cfg.upstreams.erase(cfg.upstreams.begin() + static_cast<ptrdiff_t>(index));
    metrics_registry_.remove_prefix("upstream_" + std::to_string(id) + "_");
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
            *error = "INVALID_ARGUMENT";
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
                *error = "NOT_FOUND";
            }
            return false;
        }
        ManagerUpstreamView row;
        if (!fill_upstream_view(index, &row))
        {
            if (error != nullptr)
            {
                *error = "NOT_FOUND";
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
        return fail("NOT_FOUND");
    }
    if (!patch.have_fec && !patch.have_k && !patch.have_n &&
        !patch.have_fec_timeout && !patch.have_quanta)
    {
        return fail("INVALID_ARGUMENT");
    }
    UpstreamConfig candidate;
    std::string val_err;
    if (!Config::validate_upstream_update(
            cfg.upstreams[index], patch.have_fec ? patch.fec : FecType::none,
            patch.fec_k, patch.fec_n, patch.fec_timeout_ms, patch.quanta,
            patch.have_fec, patch.have_k, patch.have_n, patch.have_fec_timeout,
            patch.have_quanta, cfg.upstreams, &candidate, &val_err))
    {
        return fail("INVALID_ARGUMENT");
    }
    if (patch.have_quanta && !set_upstream_scheduler_budget(
                                 index, candidate.scheduler_budget, error))
    {
        return fail(error != nullptr && !error->empty() ? error->c_str()
                                                        : "INVALID_ARGUMENT");
    }
    auto* udp = dynamic_cast<UdpEndpoint*>(upstreams[index].get());
    if (patch.have_fec_timeout)
    {
        if (udp == nullptr ||
            !udp->set_fec_timeout_ms(candidate.fec_timeout_ms, error))
        {
            return fail(error != nullptr && !error->empty()
                            ? error->c_str()
                            : "INVALID_ARGUMENT");
        }
    }
    if (patch.have_fec || patch.have_k || patch.have_n)
    {
        if (!set_upstream_fec(index, candidate.fec_type, candidate.fec_k,
                              candidate.fec_n, error))
        {
            return fail(error != nullptr && !error->empty()
                            ? error->c_str()
                            : "INVALID_ARGUMENT");
        }
    }
    cfg.upstreams[index] = candidate;
    if (out == nullptr || !fill_upstream_view(index, out))
    {
        return fail("NOT_FOUND");
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

void fill_upstream_fec_fields(const UdpEndpoint* udp, FecType* fec, int* k,
                              int* n)
{
    if (fec == nullptr || k == nullptr || n == nullptr)
    {
        return;
    }
    if (udp != nullptr)
    {
        udp->get_fec(fec, k, n);
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
            *error = "INVALID_ARGUMENT";
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
                *error = "NOT_FOUND";
            }
            return false;
        }
        const auto* udp =
            dynamic_cast<const UdpEndpoint*>(upstreams[index].get());
        ManagerUpstreamRxStatView row;
        row.id = id;
        fill_upstream_fec_fields(udp, &row.fec, &row.fec_k, &row.fec_n);
        if (udp != nullptr)
        {
            row.rxbyt = udp->app_rx_bytes();
            row.rxpkt = udp->app_rx_packets();
            row.rx_oversize = udp->app_rx_oversize_pkt();
            row.fec_rec = udp->fec_recovered();
            row.fec_lost = udp->fec_decode_fail();
            row.fec_rxbyt = udp->fec_air_rx_bytes();
            row.fec_rxpkt = udp->fec_air_rx_packets();
        }
        uint64_t gap = 0;
        if (radio_upstream_table_.peek_seq_lost(index, &gap))
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
            *error = "INVALID_ARGUMENT";
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
                *error = "NOT_FOUND";
            }
            return false;
        }
        const auto* udp =
            dynamic_cast<const UdpEndpoint*>(upstreams[index].get());
        ManagerUpstreamTxStatView row;
        row.id = id;
        fill_upstream_fec_fields(udp, &row.fec, &row.fec_k, &row.fec_n);
        if (udp != nullptr)
        {
            row.txbyt = udp->app_tx_bytes();
            row.txpkt = udp->app_tx_packets();
            row.fec_txbyt = udp->fec_air_tx_bytes();
            row.fec_txpkt = udp->fec_air_tx_packets();
            udp->tx_pending_stats(&row.tx_pending_pkt, &row.tx_pending_byt);
        }
        out->push_back(row);
    }
    return true;
}

void App::refresh_host_metrics()
{
    metrics_registry_.get_metrics<MetricU64>("rx_drop_domain")
        ->store(rx_demux_.rx_drop_domain());
    metrics_registry_.get_metrics<MetricU64>("rx_drop_bus")
        ->store(rx_demux_.rx_drop_bus());
    metrics_registry_.get_metrics<MetricU64>("tx_send_fail_mpdu")
        ->store(tx_mux_.tx_send_fail_mpdu());
    metrics_registry_.get_metrics<MetricU64>("tx_send_fail_byt")
        ->store(tx_mux_.tx_send_fail_byt());
    metrics_registry_.get_metrics<MetricU64>("tx_pacing_txtime_us")
        ->store(tx_mux_.pacing_txtime_full_us());
    metrics_registry_.get_metrics<MetricU64>("tx_pacing_gap_us")
        ->store(tx_mux_.pacing_gap_us());
    if (radio != nullptr)
    {
        const WifiUdp::counters_s rc = radio->peek_counters();
        metrics_registry_.get_metrics<MetricU64>("radio_rx_pkt")
            ->store(rc.rx_pkt);
        metrics_registry_.get_metrics<MetricU64>("radio_rx_byt")
            ->store(rc.rx_byte);
        metrics_registry_.get_metrics<MetricU64>("radio_tx_pkt")
            ->store(rc.tx_pkt);
        metrics_registry_.get_metrics<MetricU64>("radio_tx_byt")
            ->store(rc.tx_byte);
        metrics_registry_.get_metrics<MetricU64>("radio_fcs_err_pkt")
            ->store(rc.fcs_error_pkt);
        metrics_registry_.get_metrics<MetricU64>("radio_fcs_unknown_pkt")
            ->store(rc.fcs_unknown_pkt);
        const RadioFcsMode fcs = radio->fcs_mode();
        metrics_registry_.get_metrics<MetricU64>("radio_fcs_mode")
            ->store(static_cast<uint64_t>(fcs));
    }
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
        metrics_registry_.get_metrics<MetricU64>(gap_key)->store(
            up->stats().air_rx_gap_loss);
        const auto* udp = dynamic_cast<const UdpEndpoint*>(up);
        if (udp != nullptr)
        {
            const std::string oversize_key =
                "upstream_" + std::to_string(view.id) + "_app_rx_oversize_pkt";
            metrics_registry_.get_metrics<MetricU64>(oversize_key)
                ->store(udp->app_rx_oversize_pkt());
        }
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
            *error = "INVALID_ARGUMENT";
        }
        return false;
    }
    refresh_host_metrics();
    const std::map<std::string, Metrics> snapshot =
        metrics_registry_.getMetrics(keys);
    out->clear();
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

std::string mplane_err_string(const std::string& err)
{
    std::string token;
    if (mplane_nok_token(err, &token))
    {
        return token;
    }
    return err.empty() ? "error" : err;
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
    if (cfg.radio_fcs == RadioFcsConfig::signal && mode != RadioFcsMode::signal)
    {
        LOG_WRN(
            "winject.radio_fcs=signal but radio reports fcs=%s; using radio",
            radio_fcs_mode_name(mode));
    }
    else if (cfg.radio_fcs == RadioFcsConfig::actual &&
             mode != RadioFcsMode::actual)
    {
        LOG_WRN(
            "winject.radio_fcs=actual but radio reports fcs=%s; using radio",
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

void App::console_radio_info(ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        reply.send_nok("NOT_FOUND");
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
                radio_manager_.set_actual_phy(view);
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
                    reply.send_nok("NOT_FOUND");
                    return;
                }
                reply.send_text(format_radio_caps_ok_line(radio->fcs_mode()));
            });
        return;
    }
    if (radio == nullptr || radio->fcs_mode() == RadioFcsMode::unknown)
    {
        reply.send_nok("NOT_FOUND");
        return;
    }
    reply.send_text(format_radio_caps_ok_line(radio->fcs_mode()));
}

void App::console_radio_tx(const ManagerRadioUpdate& patch,
                           ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        reply.send_nok("NOT_FOUND");
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
            reply.send_nok("INVALID_ARGUMENT");
            return;
        }
        if (!kv.empty())
        {
            kv += ' ';
        }
        kv += "modulation=" + canonical;
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
                radio_manager_.sync_pacing_for_modulation(cfg.modulation, true);
            }
            ManagerRadioView view;
            if (parse_radio_info_body(mplane_join_body(r1), &view))
            {
                radio_manager_.set_actual_phy(view);
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

void App::console_radio_reset(uint8_t id, ManagerConsoleReply reply)
{
    if (!console_ok)
    {
        reply.send_nok("NOT_FOUND");
        return;
    }
    console.send_radio_reset(
        id,
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
        reply.send_nok("NOT_FOUND");
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
    if (cfg.manager_console_in.empty())
    {
        return true;
    }
    sockaddr_in in_addr = {};
    sockaddr_in out_addr = {};
    if (!parse_host_port(cfg.manager_console_in, &in_addr) ||
        !parse_host_port(cfg.manager_console_out, &out_addr))
    {
        LOG_ERR("manager console: invalid console_in/out");
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
    handlers.radio_reset = [this](uint8_t id, ManagerConsoleReply reply)
    {
        console_radio_reset(id, reply);
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
    std::string err;
    if (!mgr_console.start(reactor, in_addr, out_addr, std::move(handlers),
                           &err))
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
                if (done)
                {
                    done(false);
                }
                return;
            }
            radio_manager_.on_phy_programmed();
            if (done)
            {
                done(true);
            }
        },
        [this](const MplaneResult& caps)
        {
            apply_radio_caps_mplane(caps);
        });
}

bool App::apply_console()
{
    if (cfg.local_ip.empty())
    {
        local_ip = console.local_ip();
    }
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
            radio_manager_.on_phy_programmed();
        });
    return true;
}

bool App::hold_console()
{
    console.clear_pending();
    console_ok = true;
    if (!reactor.add_read_rdy(console.fd(),
                              [this]()
                              {
                                  on_console();
                              }))
    {
        LOG_ERR("console epoll add failed");
        return false;
    }
    return true;
}

void App::drop_console()
{
    const int fd = console.fd();
    if (fd >= 0)
    {
        reactor.rem_read_rdy(fd);
        reactor.rem_write_rdy(fd);
    }
    console.cancel_pending();
    console.close();
    console_ok = false;
}

void App::begin_console()
{
    if (console_ok)
    {
        return;
    }
    std::string err;
    if (!console.start_connect(cfg, &err) || !console.finish_connect(&err))
    {
        LOG_ERR("console: %s", err.c_str());
        drop_console();
        return;
    }
    reconnect_ticks = 0;
    LOG_INF("console ready %s:%u local %s", cfg.device.c_str(),
            cfg.console_port, ipv4_to_string(console.local_ip()).c_str());
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
    std::string line;
    while (console.pop_line(&line))
    {
        console.on_line(line);
    }
}

void App::reconnect_tick()
{
    if (cfg.skip_console)
    {
        return;
    }
    if (console_ok)
    {
        reconnect_ticks = 0;
        console.poll_deadlines(std::chrono::steady_clock::now());
        radio_manager_.periodic_tick(console_ok);
        radio_manager_.heartbeat_tick(console_ok);
        return;
    }
    reconnect_ticks++;
    if (reconnect_ticks < k_reconnect_ticks)
    {
        return;
    }
    reconnect_ticks = 0;
    LOG_INF("retrying console");
    begin_console();
}

void App::arm_tick()
{
    reactor.get_timer().wait_us(250,
                                [this]()
                                {
                                    reconnect_tick();
                                    stats_tick();
                                    arm_tick();
                                });
}

void App::stats_tick()
{
    if (cfg.stats_sec == 0)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_stats.time_since_epoch().count() == 0)
    {
        last_stats = now;
        return;
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats)
            .count();
    if (elapsed_ms < static_cast<long>(cfg.stats_sec) * 1000)
    {
        return;
    }
    last_stats = now;
    tx_mux_.log_stats(elapsed_ms / 1000.0);
}

void App::stop()
{
    reactor.stop();
}

void App::flush_shutdown()
{
    for (auto& up : upstreams)
    {
        if (up)
        {
            up->announce_down();
        }
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    size_t queued_bytes = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        bool pending = false;
        for (const auto& up : upstreams)
        {
            if (up != nullptr && up->has_tx())
            {
                pending = true;
                queued_bytes += up->get_tx_size();
            }
        }
        if (!pending)
        {
            break;
        }
        tx_mux_.sync_tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (queued_bytes > 0)
    {
        LOG_INF("shutdown flush ended with ~%zu bytes still queued",
                queued_bytes);
    }
}

int App::run()
{
    if (!setup_upstreams())
    {
        return 1;
    }
    radio_manager_.set_console_ops(
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
                    radio_manager_.set_actual_phy(view);
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
    radio_manager_.set_reconcile_filter(
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
    if (!cfg.local_ip.empty() && parse_host(cfg.local_ip, &local_ip))
    {
        // keep configured local_ip for set_upstream_rx
    }
    if (cfg.skip_console)
    {
        LOG_WRN("skip_console=1: radio rx_filter_addr3 is not managed");
    }
    if (!cfg.skip_console)
    {
        std::string err;
        if (!console.start_connect(cfg, &err) || !console.finish_connect(&err))
        {
            LOG_ERR("%s", err.c_str());
            reconnect_ticks = k_reconnect_ticks;
        }
        else if (!hold_console())
        {
            drop_console();
            reconnect_ticks = k_reconnect_ticks;
        }
        else
        {
            console.apply_radio(
                cfg, cfg.config_slot,
                [this](MplaneResult r)
                {
                    if (!r.ok)
                    {
                        LOG_ERR("radio program failed: %s", r.error.c_str());
                        drop_console();
                        reconnect_ticks = k_reconnect_ticks;
                        return;
                    }
                    if (cfg.local_ip.empty())
                    {
                        local_ip = console.local_ip();
                    }
                    radio_manager_.on_phy_programmed();
                    LOG_INF("console ready %s:%u", cfg.device.c_str(),
                            cfg.console_port);
                },
                [this](const MplaneResult& caps)
                {
                    apply_radio_caps_mplane(caps);
                });
        }
        if (!console_ok)
        {
            LOG_WRN("waiting for console %s:%u", cfg.device.c_str(),
                    cfg.console_port);
        }
    }
    else if (!cfg.local_ip.empty())
    {
        parse_host(cfg.local_ip, &local_ip);
    }
    else
    {
        LOG_ERR("winject.skip_console requires winject.local_ip");
        return 1;
    }
    LOG_INF(
        "manager running local %s forward_base %u max_rate %u kbps "
        "burst=%zu burst_us=%u (%s)",
        ipv4_to_string(local_ip).c_str(), cfg.forward_base, cfg.max_rate_kbps,
        cfg.tx_burst_size, cfg.tx_burst_interval_us, cfg.modulation.c_str());
    last_stats = std::chrono::steady_clock::now();
    if (cfg.stats_sec > 0)
    {
        LOG_INF(
            "stats logging every %u s (WINJECT_STATS_SEC or winject.stats_sec)",
            cfg.stats_sec);
    }
    tx_mux_.start();
    arm_tick();
    reactor.run();
    tx_mux_.stop();
    flush_shutdown();
    return 0;
}

}  // namespace winject
