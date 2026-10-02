#include "radio/RadioManager.h"

#include "Config.h"
#include "radio/PhyAirtime.h"
#include "radio/RadioMplaneParse.h"
#include "radio/TxMux.h"
#include "radio/WifiUdp.h"
#include "utils/Log.h"

namespace winject
{

void RadioManager::bind(Config* cfg, TxMux* tx_mux, WifiUdp* wifi)
{
    cfg_ = cfg;
    tx_mux_ = tx_mux;
    wifi_ = wifi;
}

void RadioManager::set_console_ops(QueryRadioInfoFn query, ReapplyRadioFn reapply,
                                   PingFn ping, DropConsoleFn drop_console)
{
    query_info_ = std::move(query);
    reapply_ = std::move(reapply);
    ping_ = std::move(ping);
    drop_console_ = std::move(drop_console);
}

void RadioManager::set_reconcile_filter(ReconcileFilterFn reconcile)
{
    reconcile_filter_ = std::move(reconcile);
}

void RadioManager::configure_tx_mux()
{
    if (cfg_ == nullptr || tx_mux_ == nullptr)
    {
        return;
    }
    tx_mux_->configure(cfg_->domain, cfg_->max_data_per_tick, cfg_->tx_burst_size,
                       cfg_->tx_burst_interval_us);
    sync_pacing_for_modulation(cfg_->modulation, false);
}

void RadioManager::sync_pacing_for_modulation(const std::string& modulation,
                                              bool runtime_change)
{
    if (cfg_ == nullptr || tx_mux_ == nullptr)
    {
        return;
    }
    const std::string canonical = Config::canonical_modulation(modulation);
    if (canonical.empty())
    {
        return;
    }
    PhyMode mode;
    if (!phy_mode_from_name(canonical, cfg_->channel, &mode))
    {
        return;
    }
    uint32_t cap = 0;
    if (cfg_->max_rate_kbps_explicit)
    {
        cap = cfg_->max_rate_kbps;
    }
    else if (!runtime_change)
    {
        cap = 0;
    }
    tx_mux_->set_phy_mode(mode, cfg_->tx_gap_us, cap);
    LOG_INF("tx pacing for %s (ch=%u)", canonical.c_str(),
            static_cast<unsigned>(cfg_->channel));
}

void RadioManager::on_phy_programmed()
{
    if (cfg_ == nullptr)
    {
        return;
    }
    if (wifi_ != nullptr)
    {
        wifi_->register_forward();
    }
    sync_pacing_for_modulation(cfg_->modulation, false);
}

void RadioManager::periodic_tick(bool console_ok)
{
    if (!console_ok || cfg_ == nullptr || !query_info_ || !reapply_)
    {
        info_ticks_ = 0;
        return;
    }
    info_ticks_++;
    if (info_ticks_ < k_radio_info_interval_ticks)
    {
        return;
    }
    info_ticks_ = 0;

    if (reconcile_filter_)
    {
        reconcile_filter_(
            [](bool ok)
            {
                if (!ok)
                {
                    LOG_WRN("rx_filter reconcile failed");
                }
            });
    }

    query_info_(
        [this](bool ok, const ManagerRadioView& actual)
        {
            if (!ok)
            {
                LOG_WRN("radio_info poll failed");
                return;
            }
            actual_ = actual;
            if (radio_phy_matches_desired(actual_, cfg_->channel, cfg_->power_dbm,
                                          cfg_->modulation))
            {
                return;
            }
            LOG_WRN(
                "radio PHY mismatch (want ch=%u pwr=%d mod=%s; have ch=%u pwr=%d "
                "mod=%s), reapplying",
                static_cast<unsigned>(cfg_->channel),
                static_cast<int>(cfg_->power_dbm), cfg_->modulation.c_str(),
                static_cast<unsigned>(actual_.channel), actual_.tx_power,
                actual_.modulation.c_str());
            reapply_(
                [](bool reapplied)
                {
                    if (!reapplied)
                    {
                        LOG_ERR("radio reapply failed");
                    }
                });
        });
}

void RadioManager::heartbeat_tick(bool console_ok)
{
    if (!console_ok || !ping_ || !drop_console_)
    {
        heartbeat_ticks_ = 0;
        register_ticks_ = 0;
        ping_outstanding_ = false;
        return;
    }
    if (wifi_ != nullptr && ++register_ticks_ >= k_register_interval_ticks)
    {
        register_ticks_ = 0;
        wifi_->register_forward();
    }
    heartbeat_ticks_++;
    if (ping_outstanding_)
    {
        if (heartbeat_ticks_ >= k_pong_timeout_ticks)
        {
            LOG_WRN("console ping timeout");
            ping_outstanding_ = false;
            heartbeat_ticks_ = 0;
            drop_console_();
        }
        return;
    }
    if (heartbeat_ticks_ < k_ping_interval_ticks)
    {
        return;
    }
    heartbeat_ticks_ = 0;
    ping_outstanding_ = true;
    ping_(
        [this](bool ok)
        {
            ping_outstanding_ = false;
            heartbeat_ticks_ = 0;
            if (!ok)
            {
                LOG_WRN("console ping failed");
                if (drop_console_)
                {
                    drop_console_();
                }
            }
        });
}

}  // namespace winject
