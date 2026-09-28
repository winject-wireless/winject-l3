#ifndef WINJECT_MANAGER_APP_H_
#define WINJECT_MANAGER_APP_H_

#include "Config.h"
#include "console/ConsoleClient.h"
#include "console/ConsoleService.h"
#include "endpoint/Upstream.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RxDemux.h"
#include "radio/TxMux.h"
#include "radio/WifiUdp.h"
#include "utils/IOReactor.h"

#include <chrono>
#include <cstdint>
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
    bool hold_console();
    void begin_console();
    void drop_console();
    void on_console();
    void reconnect_tick();
    void heartbeat_tick();
    void arm_tick();
    void stats_tick();
    void flush_shutdown();

    bool set_upstream_fec(size_t index, FecType type, int k, int n,
                          std::string* error);
    bool get_upstream_fec(size_t index, FecType* type, int* k, int* n,
                          std::string* error);
    bool set_upstream_scheduler_budget(size_t index, size_t budget,
                                       std::string* error);
    bool get_upstream_scheduler_budget(size_t index, size_t* budget,
                                       std::string* error);
    bool set_modulation(const std::string& name, std::string* error);
    bool get_modulation(std::string* name, std::string* error);
    bool set_tx_pacing(const uint32_t* max_rate_kbps,
                       const size_t* max_data_per_tick,
                       const size_t* tx_burst_size,
                       const uint32_t* tx_burst_interval_us,
                       std::string* error);
    bool get_tx_pacing(uint32_t* max_rate_kbps, size_t* max_data_per_tick,
                       size_t* tx_burst_size, uint32_t* tx_burst_interval_us,
                       std::string* error) const;
    void fill_ci_view(ChannelInfoView* out) const;

    static constexpr int k_ping_interval_ticks = 1000;
    static constexpr int k_pong_timeout_ticks = 2000;
    static constexpr int k_reconnect_ticks = 4000;

    Config cfg;
    IOReactor reactor;
    ConsoleClient console;
    ConsoleService mgr_console;
    RadioUpstreamTable radio_upstream_table_;
    TxMux tx_mux_{radio_upstream_table_};
    RxDemux rx_demux_{radio_upstream_table_};
    std::shared_ptr<WifiUdp> radio;
    std::vector<std::shared_ptr<Upstream>> upstreams;
    in_addr device_ip{};
    in_addr local_ip{};
    int reconnect_ticks = 0;
    int heartbeat_ticks = 0;
    bool console_ok = false;
    bool awaiting_pong = false;
    std::chrono::steady_clock::time_point last_stats{};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_APP_H_
