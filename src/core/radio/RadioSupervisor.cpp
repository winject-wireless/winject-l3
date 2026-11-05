#include "radio/RadioSupervisor.h"

#include "radio/PhyAirtime.h"
#include "radio/RadioConfig.h"
#include "radio/RadioDplane.h"
#include "radio/RadioMplaneParse.h"
#include "radio/TxMux.h"
#include "utils/Log.h"

namespace winject::core
{

void RadioSupervisor::bind(RadioConfig* cfg, TxMux* tx_mux, RadioDplane* wifi,
                        IOReactor* reactor)
{
    this->cfg = cfg;
    this->tx_mux = tx_mux;
    this->wifi = wifi;
    ping_timer.bind(reactor);
    pong_timer.bind(reactor);
    register_timer.bind(reactor);
    info_timer.bind(reactor);
}

void RadioSupervisor::set_console_ops(QueryRadioInfoFn query, ReapplyRadioFn reapply,
                                   PingFn ping, DropConsoleFn drop_console)
{
    query_info = std::move(query);
    this->reapply = std::move(reapply);
    this->ping = std::move(ping);
    this->drop_console = std::move(drop_console);
}

void RadioSupervisor::set_reconcile_filter(ReconcileFilterFn reconcile)
{
    reconcile_filter = std::move(reconcile);
}

void RadioSupervisor::configure_tx_mux()
{
    if (cfg == nullptr || tx_mux == nullptr)
    {
        return;
    }
    tx_mux->configure(cfg->domain);
    sync_pacing_for_modulation(cfg->modulation);
}

void RadioSupervisor::sync_pacing_for_modulation(const std::string& modulation)
{
    if (cfg == nullptr || tx_mux == nullptr)
    {
        return;
    }
    const std::string canonical = phy_canonical_name(modulation);
    if (canonical.empty())
    {
        return;
    }
    PhyMode mode;
    if (!phy_mode_from_name(canonical, cfg->channel, &mode))
    {
        return;
    }
    tx_mux->set_phy_mode(mode);
    LOG_INF("tx pacing for %s (ch=%u)", canonical.c_str(),
            static_cast<unsigned>(cfg->channel));
}

void RadioSupervisor::on_phy_programmed()
{
    if (cfg == nullptr)
    {
        return;
    }
    if (wifi != nullptr)
    {
        wifi->register_forward();
    }
    sync_pacing_for_modulation(cfg->modulation);
}

void RadioSupervisor::on_console_up()
{
    on_console_down();
    ping_timer.arm(k_ping_interval,
                    [this]()
                    {
                        on_ping_due();
                    });
    register_timer.arm(k_register_interval,
                        [this]()
                        {
                            on_register_due();
                        });
    info_timer.arm(k_radio_info_interval,
                    [this]()
                    {
                        on_info_due();
                    });
}

void RadioSupervisor::on_console_down()
{
    gen++;
    ping_timer.cancel();
    pong_timer.cancel();
    register_timer.cancel();
    info_timer.cancel();
}

void RadioSupervisor::on_ping_due()
{
    if (!ping || !drop_console)
    {
        return;
    }
    pong_timer.arm(k_pong_timeout,
                    [this]()
                    {
                        on_pong_timeout();
                    });
    const uint32_t ping_gen = this->gen;
    ping(
        [this, ping_gen](bool ok)
        {
            if (ping_gen != this->gen)
            {
                return;
            }
            pong_timer.cancel();
            if (!ok)
            {
                LOG_WRN("console ping failed");
                drop_console();
                return;
            }
            ping_timer.arm(k_ping_interval,
                            [this]()
                            {
                                on_ping_due();
                            });
        });
}

void RadioSupervisor::on_pong_timeout()
{
    LOG_WRN("console ping timeout");
    drop_console();
}

void RadioSupervisor::on_register_due()
{
    if (wifi != nullptr)
    {
        wifi->register_forward();
    }
    register_timer.arm(k_register_interval,
                        [this]()
                        {
                            on_register_due();
                        });
}

void RadioSupervisor::on_info_due()
{
    info_timer.arm(k_radio_info_interval,
                    [this]()
                    {
                        on_info_due();
                    });
    if (cfg == nullptr || !query_info || !reapply)
    {
        return;
    }

    if (reconcile_filter)
    {
        reconcile_filter(
            [](bool ok)
            {
                if (!ok)
                {
                    LOG_WRN("rx_filter reconcile failed");
                }
            });
    }

    const uint32_t info_gen = this->gen;
    query_info(
        [this, info_gen](bool ok, const RadioPhyView& actual)
        {
            if (info_gen != this->gen)
            {
                return;
            }
            if (!ok)
            {
                LOG_WRN("radio_info poll failed");
                return;
            }
            this->actual = actual;
            if (radio_phy_matches_desired(actual, cfg->channel, cfg->power_dbm,
                                          cfg->modulation))
            {
                return;
            }
            LOG_WRN(
                "radio PHY mismatch (want ch=%u pwr=%d mod=%s; have ch=%u pwr=%d "
                "mod=%s), reapplying",
                static_cast<unsigned>(cfg->channel),
                static_cast<int>(cfg->power_dbm), cfg->modulation.c_str(),
                static_cast<unsigned>(actual.channel), actual.tx_power,
                actual.modulation.c_str());
            reapply(
                [](bool reapplied)
                {
                    if (!reapplied)
                    {
                        LOG_ERR("radio reapply failed");
                    }
                });
        });
}

}  // namespace winject::core
