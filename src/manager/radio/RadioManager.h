#ifndef WINJECT_MANAGER_RADIO_RADIO_MANAGER_H_
#define WINJECT_MANAGER_RADIO_RADIO_MANAGER_H_

#include "console/ManagerConsoleTypes.h"
#include "utils/IOReactor.h"
#include "utils/OneShotTimer.h"

#include <chrono>
#include <cstdint>
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

    void bind(Config* cfg, TxMux* tx_mux, WifiUdp* wifi, IOReactor* reactor);
    void set_console_ops(QueryRadioInfoFn query, ReapplyRadioFn reapply,
                         PingFn ping, DropConsoleFn drop_console);
    void set_reconcile_filter(ReconcileFilterFn reconcile);

    void configure_tx_mux();
    void sync_pacing_for_modulation(const std::string& modulation,
                                    bool runtime_change);
    void on_phy_programmed();
    // Console up: start the heartbeat, forward re-registration and PHY/filter
    // checks on the reactor timer. Console down: cancel them; replies still
    // in flight are ignored.
    void on_console_up();
    void on_console_down();

    const ManagerRadioView& actual_phy() const
    {
        return actual_;
    }
    void set_actual_phy(const ManagerRadioView& view)
    {
        actual_ = view;
    }

private:
    void on_ping_due();
    void on_pong_timeout();
    void on_register_due();
    void on_info_due();

    Config* cfg_ = nullptr;
    TxMux* tx_mux_ = nullptr;
    WifiUdp* wifi_ = nullptr;
    QueryRadioInfoFn query_info_;
    ReapplyRadioFn reapply_;
    PingFn ping_;
    DropConsoleFn drop_console_;
    ReconcileFilterFn reconcile_filter_;
    ManagerRadioView actual_{};
    // Bumped on console up and down; a reply from an older console session
    // does nothing.
    uint32_t gen_ = 0;
    OneShotTimer ping_timer_;
    OneShotTimer pong_timer_;
    OneShotTimer register_timer_;
    OneShotTimer info_timer_;
    // Next ping this long after the previous pong.
    static constexpr std::chrono::milliseconds k_ping_interval{250};
    static constexpr std::chrono::milliseconds k_pong_timeout{500};
    // Forward-peer re-registration keeps the radio forwarding to this host.
    static constexpr std::chrono::milliseconds k_register_interval{1000};
    // radio_info PHY drift check and rx_filter reconcile.
    static constexpr std::chrono::milliseconds k_radio_info_interval{5000};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RADIO_MANAGER_H_
