#include "App.h"

#include "endpoint/UdpEndpoint.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"
#include "frames/Mpdu.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

namespace winject
{

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
                  [this]() { tx_mux_.request_tick(); }))
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
    LOG_INF("radio inject=%u forward=%u", radio->inject_port(),
            radio->forward_port());
    return true;
}

bool App::setup_upstreams()
{
    tx_mux_.configure(cfg.max_rate_kbps, cfg.domain, cfg.max_data_per_tick,
                      cfg.tx_burst_size, cfg.tx_burst_interval_us);
    if (!setup_radio())
    {
        return false;
    }
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

bool App::get_upstream_scheduler_budget(size_t index, size_t* budget,
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
    if (!radio_upstream_table_.get_budget(index, budget))
    {
        return fail("invalid upstream index");
    }
    return true;
}

bool App::set_modulation(const std::string& name, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    const std::string canonical = Config::canonical_modulation(name);
    if (canonical.empty())
    {
        return fail("unknown modulation");
    }
    if (!Config::modulation_ok_for_channel(canonical, cfg.channel))
    {
        return fail("channel 14 requires DSSS/CCK modulation");
    }
    if (!console_ok)
    {
        return fail("console not connected");
    }
    std::string err;
    if (!console.set_modulation(canonical, &err))
    {
        if (console.take_pong())
        {
            awaiting_pong = false;
        }
        if (err.find(" -> error") == std::string::npos)
        {
            drop_console();
        }
        return fail(err.empty() ? "failed" : err.c_str());
    }
    if (console.take_pong())
    {
        awaiting_pong = false;
    }
    cfg.modulation = canonical;
    LOG_INF("modulation=%s", canonical.c_str());
    return true;
}

bool App::get_modulation(std::string* name, std::string* error)
{
    if (name == nullptr)
    {
        if (error != nullptr)
        {
            *error = "null out";
        }
        return false;
    }
    *name = cfg.modulation;
    return true;
}

bool App::set_tx_pacing(const uint32_t* max_rate_kbps,
                        const size_t* max_data_per_tick,
                        const size_t* tx_burst_size,
                        const uint32_t* tx_burst_interval_us,
                        std::string* error)
{
    if (max_rate_kbps != nullptr)
    {
        cfg.max_rate_kbps = *max_rate_kbps;
        tx_mux_.set_max_rate_kbps(*max_rate_kbps);
    }
    if (max_data_per_tick != nullptr)
    {
        cfg.max_data_per_tick = *max_data_per_tick;
        tx_mux_.set_max_data_per_tick(*max_data_per_tick);
    }
    if (tx_burst_size != nullptr || tx_burst_interval_us != nullptr)
    {
        const size_t bs = tx_burst_size != nullptr ? *tx_burst_size
                                                   : tx_mux_.tx_burst_size();
        const uint32_t bi = tx_burst_interval_us != nullptr
                                ? *tx_burst_interval_us
                                : tx_mux_.tx_burst_interval_us();
        cfg.tx_burst_size = bs;
        cfg.tx_burst_interval_us = bi;
        tx_mux_.set_tx_burst_pacing(bs, bi);
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

bool App::get_tx_pacing(uint32_t* max_rate_kbps, size_t* max_data_per_tick,
                        size_t* tx_burst_size, uint32_t* tx_burst_interval_us,
                        std::string* error) const
{
    if (max_rate_kbps == nullptr || max_data_per_tick == nullptr ||
        tx_burst_size == nullptr || tx_burst_interval_us == nullptr)
    {
        if (error != nullptr)
        {
            *error = "null out";
        }
        return false;
    }
    *max_rate_kbps = tx_mux_.max_rate_kbps();
    *max_data_per_tick = tx_mux_.max_data_per_tick();
    *tx_burst_size = tx_mux_.tx_burst_size();
    *tx_burst_interval_us = tx_mux_.tx_burst_interval_us();
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

void App::fill_ci_view(ChannelInfoView* out) const
{
    if (out == nullptr)
    {
        return;
    }
    out->flow_valid = false;
    out->tx_queue_size = 0;
    out->tx_queue_capacity = 0;
    snprintf(out->flow_t, sizeof(out->flow_t), "-");
    out->air_valid = false;
    out->rssi = 0;
    out->snr = 0;
    snprintf(out->rssi_t, sizeof(out->rssi_t), "-");
    out->tx_byte = 0;
    out->rx_byte = 0;
    out->tx_pkt = 0;
    out->rx_pkt = 0;
    out->rx_pkt_loss = 0;
    for (size_t i = 0; i < upstreams.size(); i++)
    {
        uint64_t seq_lost = 0;
        if (radio_upstream_table_.peek_seq_lost(i, &seq_lost))
        {
            out->rx_pkt_loss += seq_lost;
        }
    }
    if (radio)
    {
        const auto c = radio->peek_counters();
        out->tx_pkt = c.tx_pkt;
        out->rx_pkt = c.rx_pkt;
        out->tx_byte = c.tx_byte;
        out->rx_byte = c.rx_byte;
    }
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
    std::string err;
    if (!mgr_console.start(
            reactor, in_addr, out_addr,
            [this](size_t index, FecType type, int k, int n, std::string* error)
            {
                return set_upstream_fec(index, type, k, n, error);
            },
            [this](size_t index, size_t budget, std::string* error)
            {
                return set_upstream_scheduler_budget(index, budget, error);
            },
            [this](size_t index, FecType* type, int* k, int* n,
                   std::string* error)
            {
                return get_upstream_fec(index, type, k, n, error);
            },
            [this](size_t index, size_t* budget, std::string* error)
            {
                return get_upstream_scheduler_budget(index, budget, error);
            },
            [this](ChannelInfoView* out, std::string* error)
            {
                if (out == nullptr)
                {
                    if (error != nullptr)
                    {
                        *error = "null out";
                    }
                    return false;
                }
                fill_ci_view(out);
                return true;
            },
            [this](const std::string& name, std::string* error)
            {
                return set_modulation(name, error);
            },
            [this](std::string* name, std::string* error)
            {
                return get_modulation(name, error);
            },
            [this](const uint32_t* max_rate_kbps,
                   const size_t* max_data_per_tick, const size_t* tx_burst_size,
                   const uint32_t* tx_burst_interval_us, std::string* error)
            {
                return set_tx_pacing(max_rate_kbps, max_data_per_tick,
                                     tx_burst_size, tx_burst_interval_us,
                                     error);
            },
            [this](uint32_t* max_rate_kbps, size_t* max_data_per_tick,
                   size_t* tx_burst_size, uint32_t* tx_burst_interval_us,
                   std::string* error)
            {
                return get_tx_pacing(max_rate_kbps, max_data_per_tick,
                                     tx_burst_size, tx_burst_interval_us,
                                     error);
            },
            &err))
    {
        LOG_ERR("manager console: %s", err.c_str());
        return false;
    }
    return true;
}

bool App::apply_console()
{
    std::string err;
    if (cfg.local_ip.empty())
    {
        local_ip = console.local_ip();
    }
    if (!console.apply_radio(cfg, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    if (!radio)
    {
        LOG_ERR("radio not open");
        return false;
    }
    if (!console.apply_upstream(cfg, radio->inject_port(),
                                radio->forward_port(), local_ip, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    return hold_console();
}

bool App::hold_console()
{
    console.clear_pending();
    awaiting_pong = false;
    heartbeat_ticks = 0;
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
    console.close();
    console_ok = false;
    awaiting_pong = false;
    heartbeat_ticks = 0;
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
    while (true)
    {
        char buf[2048];
        const ssize_t n = recv(console.fd(), buf, sizeof(buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            break;
        }
        if (n < 0)
        {
            LOG_WRN("console recv: %s", strerror(errno));
            drop_console();
            return;
        }
        if (n == 0)
        {
            continue;
        }
        console.append_recv(buf, static_cast<size_t>(n));
    }

    std::string line;
    while (console.pop_line(&line))
    {
        if (line == "pong")
        {
            awaiting_pong = false;
            heartbeat_ticks = 0;
            continue;
        }
        // Ignore unsolicited lines while holding the console.
    }
}

void App::heartbeat_tick()
{
    if (!console_ok)
    {
        return;
    }
    heartbeat_ticks++;
    if (awaiting_pong)
    {
        if (heartbeat_ticks >= k_pong_timeout_ticks)
        {
            LOG_WRN("console ping timeout");
            drop_console();
        }
        return;
    }
    if (heartbeat_ticks < k_ping_interval_ticks)
    {
        return;
    }
    heartbeat_ticks = 0;
    std::string err;
    if (!console.send_ping(&err))
    {
        LOG_WRN("console ping failed: %s", err.c_str());
        drop_console();
        return;
    }
    awaiting_pong = true;
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
        heartbeat_tick();
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
    // CLOSE is ctrl (not rate-limited). One pass injects the repeats.
    tx_mux_.sync_tick();
}

int App::run()
{
    if (!setup_upstreams())
    {
        return 1;
    }
    if (!start_manager_console())
    {
        return 1;
    }
    std::vector<uint16_t> inject_ports;
    std::vector<uint16_t> forward_ports;
    if (radio)
    {
        inject_ports.push_back(radio->inject_port());
        forward_ports.push_back(radio->forward_port());
    }
    if (!cfg.local_ip.empty() && parse_host(cfg.local_ip, &local_ip))
    {
        // keep configured local_ip for set_upstream_rx
    }
    std::string err;
    if (!cfg.skip_console)
    {
        in_addr console_local = {};
        bool held = false;
        if (!console.program(cfg, inject_ports, forward_ports, &console_local,
                             &err))
        {
            LOG_ERR("%s", err.c_str());
        }
        else
        {
            if (cfg.local_ip.empty())
            {
                local_ip = console_local;
            }
            held = hold_console();
            if (!held)
            {
                drop_console();
            }
        }
        if (held)
        {
            LOG_INF("console ready %s:%u", cfg.device.c_str(),
                    cfg.console_port);
        }
        else
        {
            LOG_WRN("waiting for console %s:%u", cfg.device.c_str(),
                    cfg.console_port);
            reconnect_ticks = k_reconnect_ticks;
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
    flush_shutdown();
    tx_mux_.stop();
    return 0;
}

}  // namespace winject
