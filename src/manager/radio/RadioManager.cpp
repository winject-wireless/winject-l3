#include "radio/RadioManager.h"

#include "Config.h"
#include "radio/PhyAirtime.h"
#include "radio/RadioMplaneParse.h"
#include "radio/TxMux.h"
#include "radio/WifiUdp.h"
#include "utils/Log.h"

namespace winject
{

void RadioManager::bind(Config* cfg, TxMux* tx_mux, WifiUdp* wifi,
                        IOReactor* reactor)
{
    cfg_ = cfg;
    tx_mux_ = tx_mux;
    wifi_ = wifi;
    ping_timer_.bind(reactor);
    pong_timer_.bind(reactor);
    register_timer_.bind(reactor);
    info_timer_.bind(reactor);
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
    tx_mux_->configure(cfg_->domain);
    sync_pacing_for_modulation(cfg_->modulation);
}

void RadioManager::sync_pacing_for_modulation(const std::string& modulation)
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
    tx_mux_->set_phy_mode(mode);
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
    sync_pacing_for_modulation(cfg_->modulation);
}

void RadioManager::on_console_up()
{
    on_console_down();
    ping_timer_.arm(k_ping_interval,
                    [this]()
                    {
                        on_ping_due();
                    });
    register_timer_.arm(k_register_interval,
                        [this]()
                        {
                            on_register_due();
                        });
    info_timer_.arm(k_radio_info_interval,
                    [this]()
                    {
                        on_info_due();
                    });
}

void RadioManager::on_console_down()
{
    gen_++;
    ping_timer_.cancel();
    pong_timer_.cancel();
    register_timer_.cancel();
    info_timer_.cancel();
}

void RadioManager::on_ping_due()
{
    if (!ping_ || !drop_console_)
    {
        return;
    }
    pong_timer_.arm(k_pong_timeout,
                    [this]()
                    {
                        on_pong_timeout();
                    });
    const uint32_t gen = gen_;
    ping_(
        [this, gen](bool ok)
        {
            if (gen != gen_)
            {
                return;
            }
            pong_timer_.cancel();
            if (!ok)
            {
                LOG_WRN("console ping failed");
                drop_console_();
                return;
            }
            ping_timer_.arm(k_ping_interval,
                            [this]()
                            {
                                on_ping_due();
                            });
        });
}

void RadioManager::on_pong_timeout()
{
    LOG_WRN("console ping timeout");
    drop_console_();
}

void RadioManager::on_register_due()
{
    if (wifi_ != nullptr)
    {
        wifi_->register_forward();
    }
    register_timer_.arm(k_register_interval,
                        [this]()
                        {
                            on_register_due();
                        });
}

void RadioManager::on_info_due()
{
    info_timer_.arm(k_radio_info_interval,
                    [this]()
                    {
                        on_info_due();
                    });
    if (cfg_ == nullptr || !query_info_ || !reapply_)
    {
        return;
    }

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

}  // namespace winject
