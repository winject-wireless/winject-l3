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
class RadioDplane;

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

    void bind(Config* cfg, TxMux* tx_mux, RadioDplane* wifi,
              IOReactor* reactor);
    void set_console_ops(QueryRadioInfoFn query, ReapplyRadioFn reapply,
                         PingFn ping, DropConsoleFn drop_console);
    void set_reconcile_filter(ReconcileFilterFn reconcile);

    void configure_tx_mux();
    void sync_pacing_for_modulation(const std::string& modulation);
    void on_phy_programmed();
    // Console up: start the heartbeat, forward re-registration and PHY/filter
    // checks on the reactor timer. Console down: cancel them; replies still
    // in flight are ignored.
    void on_console_up();
    void on_console_down();

    const ManagerRadioView& actual_phy() const
    {
        return actual;
    }
    void set_actual_phy(const ManagerRadioView& view)
    {
        actual = view;
    }

private:
    void on_ping_due();
    void on_pong_timeout();
    void on_register_due();
    void on_info_due();

    Config* cfg = nullptr;
    TxMux* tx_mux = nullptr;
    RadioDplane* wifi = nullptr;
    QueryRadioInfoFn query_info;
    ReapplyRadioFn reapply;
    PingFn ping;
    DropConsoleFn drop_console;
    ReconcileFilterFn reconcile_filter;
    ManagerRadioView actual{};
    // Bumped on console up and down; a reply from an older console session
    // does nothing.
    uint32_t gen = 0;
    OneShotTimer ping_timer;
    OneShotTimer pong_timer;
    OneShotTimer register_timer;
    OneShotTimer info_timer;
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
