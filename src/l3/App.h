#ifndef WINJECT_MANAGER_APP_H_
#define WINJECT_MANAGER_APP_H_

#include "Config.h"
#include "Control.h"
#include "ControlTypes.h"
#include "console/ConsoleService.h"
#include "endpoint/QueueUpstream.h"
#include "endpoint/UDPUpstream.h"
#include "mplane/RadioMplaneClient.h"
#include "radio/RadioDplane.h"
#include "radio/RadioSupervisor.h"
#include "radio/RadioUpstreamTable.h"
#include "radio/RxDemux.h"
#include "radio/TxMux.h"
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

    // Function-call m-plane for this node; attached while run() serves.
    // Set before run() to share a handle (winject_l3::control()).
    void set_control(winject_l3::Control c)
    {
        control_handle = std::move(c);
    }
    const winject_l3::Control& control() const
    {
        return control_handle;
    }

private:
    bool add_upstream(const UpstreamConfig& uc);
    bool setup_radio();
    bool setup_upstreams();
    ControlOps make_control_ops();
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
    std::shared_ptr<UpstreamLink> construct_upstream(
        const UpstreamConfig& config,
        const std::shared_ptr<BoundedLossyQueue>& tx_queue,
        const UpstreamLink::TxSequences* resume = nullptr);
    bool replace_upstream(size_t index, const UpstreamConfig& config,
                          std::string* error);
    bool set_upstream_scheduler_budget(size_t index, size_t budget,
                                       std::string* error);
    bool fill_upstream_view(size_t index, UpstreamView* out) const;
    bool console_add_upstream(const UpstreamView& spec, std::string* error);
    bool console_remove_upstream(uint8_t id, std::string* error);
    bool console_list_upstream(const std::vector<uint8_t>& ids,
                               std::vector<UpstreamView>* out,
                               std::string* error) const;
    bool console_update_upstream(const UpstreamUpdate& patch, UpstreamView* out,
                                 std::string* error);
    bool console_list_upstream_rx_stat(const std::vector<uint8_t>& ids,
                                       std::vector<UpstreamRxStatView>* out,
                                       std::string* error) const;
    bool console_list_upstream_tx_stat(const std::vector<uint8_t>& ids,
                                       std::vector<UpstreamTxStatView>* out,
                                       std::string* error) const;
    bool console_get_metrics(const std::vector<std::string>& keys,
                             std::vector<MetricView>* out, std::string* error);
    void apply_radio_caps_mplane(const RadioCapsReply& caps);
    void console_radio_info(ControlReply reply);
    void console_radio_caps_info(ControlReply reply);
    void console_radio_stats(ControlReply reply);
    void console_radio_mplane(const std::string& line, ControlReply reply);
    void console_radio_tx(const RadioUpdate& patch, ControlReply reply);
    void console_radio_reset(ControlReply reply);
    void console_config_slot(uint8_t slot, ControlReply reply);
    bool console_radio_device(const RadioDeviceUpdate& patch,
                              RadioDeviceView* out, std::string* error);
    void apply_radio_dplane();
    void close_radio();
    void apply_configured_fcs();
    void send_radio_unavailable_nok(ControlReply reply) const;
    void on_radio_version(bool known, const WinjectVersion& v);
    void log_proto_mismatch_once(const WinjectVersion& radio_ver);

    static constexpr std::chrono::milliseconds k_reconnect_interval{1000};

    Config cfg;
    std::shared_ptr<IOReactor> reactor = std::make_shared<IOReactor>();
    RadioMplaneClient console;
    ConsoleService mgr_console;
    RadioUpstreamTable radio_upstream_table;
    TxMux tx_mux{radio_upstream_table};
    RadioSupervisor radio_supervisor;
    RxDemux rx_demux{radio_upstream_table};
    std::shared_ptr<RadioDplane> radio;
    // Queue radio TX shares the host's blocking data-plane reactor. In
    // particular, ESP-IDF poll() must not spin on sub-ms pacing deadlines.
    std::shared_ptr<UpstreamLink::Reactor> radio_queue_reactor;
    std::vector<std::shared_ptr<UpstreamLink>> upstreams;
    std::shared_ptr<UpstreamLink::Reactor> upstream_reactor =
        std::make_shared<UpstreamLink::Reactor>();
    std::thread upstream_reactor_thread;
    OneShotTimer reconnect_timer;
    // Runs RadioMplaneClient::poll_deadlines() at the earliest request
    // deadline.
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
    winject_l3::Control control_handle;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_APP_H_
