#ifndef WINJECT_MANAGER_APP_H_
#define WINJECT_MANAGER_APP_H_

#include "Config.h"
#include "console/ConsoleClient.h"
#include "console/ConsoleService.h"
#include "console/ManagerConsoleTypes.h"
#include "endpoint/Upstream.h"
#include "radio/RadioManager.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RxDemux.h"
#include "radio/TxMux.h"
#include "radio/WifiUdp.h"
#include "utils/IOReactor.h"
#include "utils/MetricsRegistry.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace winject
{

class App
{
public:
    bool load(const std::string& path);
    int run();
    void stop();

private:
    bool add_upstream(const UpstreamConfig& uc);
    bool setup_radio();
    bool setup_upstreams();
    bool start_manager_console();
    bool apply_console();
    void reapply_radio_console(std::function<void(bool ok)> done);
    bool hold_console();
    void begin_console();
    void drop_console();
    void on_console();
    void reconnect_tick();
    void arm_tick();
    void stats_tick();
    void flush_shutdown();
    void refresh_host_metrics();

    bool set_upstream_fec(size_t index, FecType type, int k, int n,
                          std::string* error);
    bool get_upstream_fec(size_t index, FecType* type, int* k, int* n,
                          std::string* error);
    bool set_upstream_scheduler_budget(size_t index, size_t budget,
                                       std::string* error);
    bool fill_upstream_view(size_t index, ManagerUpstreamView* out) const;
    bool console_add_upstream(const ManagerUpstreamView& spec,
                              std::string* error);
    bool console_remove_upstream(uint8_t id, std::string* error);
    bool console_list_upstream(const std::vector<uint8_t>& ids,
                               std::vector<ManagerUpstreamView>* out,
                               std::string* error) const;
    bool console_update_upstream(const ManagerUpstreamUpdate& patch,
                                 ManagerUpstreamView* out, std::string* error);
    bool console_list_upstream_rx_stat(
        const std::vector<uint8_t>& ids,
        std::vector<ManagerUpstreamRxStatView>* out, std::string* error) const;
    bool console_list_upstream_tx_stat(
        const std::vector<uint8_t>& ids,
        std::vector<ManagerUpstreamTxStatView>* out, std::string* error) const;
    bool console_get_metrics(const std::vector<std::string>& keys,
                             std::vector<ManagerMetricView>* out,
                             std::string* error);
    void apply_radio_caps_mplane(const MplaneResult& r);
    void console_radio_info(ManagerConsoleReply reply);
    void console_radio_caps_info(ManagerConsoleReply reply);
    void console_radio_stats(ManagerConsoleReply reply);
    void console_radio_tx(const ManagerRadioUpdate& patch,
                          ManagerConsoleReply reply);
    void console_radio_reset(uint8_t id, ManagerConsoleReply reply);
    void console_config_slot(uint8_t slot, ManagerConsoleReply reply);

    static constexpr int k_reconnect_ticks = 4000;

    Config cfg;
    IOReactor reactor;
    ConsoleClient console;
    ConsoleService mgr_console;
    RadioUpstreamTable radio_upstream_table_;
    TxMux tx_mux_{radio_upstream_table_};
    RadioManager radio_manager_;
    RxDemux rx_demux_{radio_upstream_table_};
    std::shared_ptr<WifiUdp> radio;
    std::vector<std::shared_ptr<Upstream>> upstreams;
    in_addr device_ip{};
    in_addr local_ip{};
    int reconnect_ticks = 0;
    bool console_ok = false;
    std::chrono::steady_clock::time_point last_stats{};
    MetricsRegistry metrics_registry_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_APP_H_
