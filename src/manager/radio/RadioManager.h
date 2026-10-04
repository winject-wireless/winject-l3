#ifndef WINJECT_MANAGER_RADIO_RADIO_MANAGER_H_
#define WINJECT_MANAGER_RADIO_RADIO_MANAGER_H_

#include "console/ManagerConsoleTypes.h"

#include <chrono>
#include <functional>
#include <string>

namespace winject
{

class Config;
class TxMux;
class WifiUdp;

// Owns TxMux pacing vs modulation, heartbeat, and periodic radio_info
// drift/reapply.
class RadioManager
{
public:
    using QueryRadioInfoFn = std::function<void(
        std::function<void(bool ok, const ManagerRadioView& actual)>)>;
    using ReapplyRadioFn = std::function<void(std::function<void(bool ok)>)>;
    using PingFn = std::function<void(std::function<void(bool ok)>)>;
    using DropConsoleFn = std::function<void()>;
    using ReconcileFilterFn = std::function<void(std::function<void(bool ok)>)>;

    void bind(Config* cfg, TxMux* tx_mux, WifiUdp* wifi);
    void set_console_ops(QueryRadioInfoFn query, ReapplyRadioFn reapply,
                         PingFn ping, DropConsoleFn drop_console);
    void set_reconcile_filter(ReconcileFilterFn reconcile);

    void configure_tx_mux();
    void sync_pacing_for_modulation(const std::string& modulation,
                                    bool runtime_change);
    void on_phy_programmed();
    void periodic_tick(bool console_ok);
    void heartbeat_tick(bool console_ok);

    const ManagerRadioView& actual_phy() const
    {
        return actual_;
    }
    void set_actual_phy(const ManagerRadioView& view)
    {
        actual_ = view;
    }

    static constexpr int k_radio_info_interval_ticks = 20000;

private:
    Config* cfg_ = nullptr;
    TxMux* tx_mux_ = nullptr;
    WifiUdp* wifi_ = nullptr;
    QueryRadioInfoFn query_info_;
    ReapplyRadioFn reapply_;
    PingFn ping_;
    DropConsoleFn drop_console_;
    ReconcileFilterFn reconcile_filter_;
    ManagerRadioView actual_{};
    int info_ticks_ = 0;
    int heartbeat_ticks_ = 0;
    int register_ticks_ = 0;
    bool ping_outstanding_ = false;
    // Forward-peer re-registration (~1 s); every tick floods the radio.
    static constexpr int k_register_interval_ticks = 4000;
    static constexpr int k_ping_interval_ticks = 1000;
    static constexpr int k_pong_timeout_ticks = 2000;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RADIO_MANAGER_H_
