#ifndef WINJECT_MANAGER_APP_H_
#define WINJECT_MANAGER_APP_H_

#include "Config.h"
#include "console/ConsoleClient.h"
#include "console/ConsoleService.h"
#include "console/ManagerConsoleTypes.h"
#include "endpoint/QueueUpstream.h"
#include "endpoint/UDPUpstream.h"
#include "radio/RadioManager.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RxDemux.h"
#include "radio/TxMux.h"
#include "radio/WifiUdp.h"
#include "utils/IOReactor.h"
#include "utils/MetricsRegistry.h"
#include "utils/OneShotTimer.h"
#include "utils/Version.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace winject
{

class App
{
public:
    ~App();

    // Copies config after Config::validate. The host parses a file or fills
    // the struct; this does not open a file.
    bool load(Config config);
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
    void schedule_reconnect(std::chrono::milliseconds after);
    void on_console_request_deadline(
        std::chrono::steady_clock::time_point deadline);
    void arm_console_deadline();
    void arm_stats();
    void begin_shutdown();
    void refresh_host_metrics();

    // resume continues a predecessor's TX sequences (hot-swap).
    std::shared_ptr<Upstream> construct_upstream(
        const UpstreamConfig& config,
        const std::shared_ptr<BoundedLossyQueue>& tx_queue,
        const Upstream::TxSequences* resume = nullptr);
    bool replace_upstream(size_t index, const UpstreamConfig& config,
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
    void console_radio_reset(ManagerConsoleReply reply);
    void console_config_slot(uint8_t slot, ManagerConsoleReply reply);
    bool console_radio_device(const ManagerRadioDeviceUpdate& patch,
                              ManagerRadioDeviceView* out, std::string* error);
    void apply_radio_dplane();
    void apply_configured_fcs();
    void send_radio_unavailable_nok(ManagerConsoleReply reply) const;
    void on_radio_version(bool known, const WinjectVersion& v);
    void log_proto_mismatch_once(const WinjectVersion& radio_ver);

    static constexpr std::chrono::milliseconds k_reconnect_interval{1000};

    Config cfg;
    std::shared_ptr<IOReactor> reactor = std::make_shared<IOReactor>();
    ConsoleClient console;
    ConsoleService mgr_console;
    RadioUpstreamTable radio_upstream_table;
    TxMux tx_mux{radio_upstream_table};
    RadioManager radio_manager;
    RxDemux rx_demux{radio_upstream_table};
    std::shared_ptr<WifiUdp> radio;
    std::vector<std::shared_ptr<Upstream>> upstreams;
    std::shared_ptr<Upstream::Reactor> upstream_reactor =
        std::make_shared<Upstream::Reactor>();
    std::thread upstream_reactor_thread;
    OneShotTimer reconnect_timer;
    // Runs ConsoleClient::poll_deadlines() at the earliest request deadline.
    OneShotTimer console_deadline_timer;
    std::chrono::steady_clock::time_point console_deadline_at{};
    OneShotTimer stats_timer;
    // Set by stop(); checked once the reactor runs, in case stop() came
    // before reactor.run() set it running.
    std::atomic<bool> stop_requested{false};
    bool console_ok = false;
    bool radio_version_known = false;
    WinjectVersion radio_version{};
    bool radio_proto_ok = false;
    bool proto_mismatch_logged = false;
    std::chrono::steady_clock::time_point last_stats{};
    MetricsRegistry metrics_registry;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_APP_H_
