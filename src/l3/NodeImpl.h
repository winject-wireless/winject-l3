#ifndef WINJECT_L3_NODE_IMPL_H_
#define WINJECT_L3_NODE_IMPL_H_

#include "Config.h"
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
#include "winject_l3/Node.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace winject_l3::detail
{
struct NodeCore;
}

namespace winject
{

// The l3 node: reactor, data path and the radio. Every method below runs on
// the node's reactor thread; the public handles (winject_l3::Node, Upstream,
// Radio, ...) post to it. The node never calls user code there except
// UpstreamSpec::on_queue, and never waits on another thread.
class NodeImpl
{
public:
    template <class T>
    using Result = winject_l3::Result<T>;
    using Status = winject_l3::Status;
    template <class T>
    using Reply = std::function<void(Result<T>)>;
    struct Empty
    {
    };

    explicit NodeImpl(unsigned stats_sec);
    ~NodeImpl();
    NodeImpl(const NodeImpl&) = delete;
    NodeImpl& operator=(const NodeImpl&) = delete;

    // Starts the upstream reactor thread and the node's thread, attached to
    // core. thread_setup runs on this thread before each one starts.
    void start(winject_l3::detail::NodeCore* core,
               const winject_l3::ThreadSetup& thread_setup);
    // Waits for the node's thread, which exits after stop().
    void join();
    void stop();
    winject::IOReactor& io_reactor()
    {
        return *reactor;
    }

    // Upstreams.
    Status add_upstream(const winject_l3::UpstreamSpec& spec);
    Status remove_upstream(uint8_t id);
    bool has_upstream(uint8_t id) const;
    std::vector<uint8_t> upstream_ids() const;
    Result<winject_l3::UpstreamSpec> upstream_spec(uint8_t id) const;
    Result<winject_l3::UpstreamSpec> update_upstream(
        uint8_t id, const winject_l3::UpstreamPatch& patch);
    Result<winject_l3::UpstreamRxStats> upstream_rx_stats(uint8_t id) const;
    Result<winject_l3::UpstreamTxStats> upstream_tx_stats(uint8_t id) const;
    Result<std::vector<winject_l3::Metric>> metrics(
        const std::vector<std::string>& keys);

    // The radio.
    Status create_radio(const winject_l3::RadioSpec& spec);
    Status add_radio();
    Status remove_radio();
    bool has_radio() const
    {
        return radio_created;
    }
    Result<winject_l3::RadioState> radio_state() const;
    Result<winject_l3::RadioSpec> radio_spec() const;
    Result<winject_l3::RadioDevice> radio_device() const;
    Status set_radio_device(const winject_l3::RadioDevice& device);
    void set_radio_phy(const winject_l3::RadioPhyPatch& patch,
                       Reply<winject_l3::RadioInfo> reply);
    void radio_info(Reply<winject_l3::RadioInfo> reply);
    void radio_caps(Reply<winject_l3::RadioCaps> reply);
    void radio_stats(Reply<winject_l3::RadioStats> reply);
    void set_config_slot(uint8_t slot, Reply<Empty> reply);
    void radio_reset(Reply<Empty> reply);
    void radio_mplane(const std::string& line, Reply<std::string> reply);

private:
    void loop();
    bool add_upstream_config(const UpstreamConfig& uc);
    // nullptr, or the errno name of what failed.
    const char* setup_radio();
    void reapply_radio_console(std::function<void(bool ok)> done);
    bool apply_console();
    bool hold_console();
    void begin_console();
    void drop_console();
    void close_console();
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
    size_t upstream_index(uint8_t id) const;
    winject_l3::UpstreamSpec upstream_spec_at(size_t index) const;
    void apply_radio_caps(const RadioCapsReply& caps);
    void apply_radio_dplane();
    void close_radio();
    void apply_configured_fcs();
    const char* radio_unavailable_error() const;
    winject_l3::RadioInfo radio_info_from(const RadioPhyReply& reply) const;
    void on_radio_version(bool known, const WinjectVersion& v);
    void log_proto_mismatch_once(const WinjectVersion& radio_ver);

    static constexpr std::chrono::milliseconds k_reconnect_interval{1000};

    winject_l3::detail::NodeCore* core = nullptr;
    // The radio and upstreams as configured: the radio fields from
    // RadioSpec, upstreams in add order.
    Config cfg;
    std::map<size_t, std::function<void(std::shared_ptr<UpstreamQueue>)>>
        on_queue;
    std::shared_ptr<IOReactor> reactor = std::make_shared<IOReactor>();
    RadioMplaneClient console;
    RadioUpstreamTable radio_upstream_table;
    TxMux tx_mux{radio_upstream_table};
    RadioSupervisor radio_supervisor;
    RxDemux rx_demux{radio_upstream_table};
    std::shared_ptr<RadioDplane> radio;
    bool radio_created = false;
    bool radio_added = false;
    // Queue radio TX shares the host's blocking data-plane reactor. In
    // particular, ESP-IDF poll() must not spin on sub-ms pacing deadlines.
    std::shared_ptr<UpstreamLink::Reactor> radio_queue_reactor;
    std::vector<std::shared_ptr<UpstreamLink>> upstreams;
    std::shared_ptr<UpstreamLink::Reactor> upstream_reactor =
        std::make_shared<UpstreamLink::Reactor>();
    std::thread upstream_reactor_thread;
    std::thread node_thread;
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
    // Set once the radio took the config after a connect (apply_console).
    bool radio_programmed = false;
    bool radio_version_known = false;
    WinjectVersion radio_version{};
    bool radio_proto_ok = false;
    bool proto_mismatch_logged = false;
    std::chrono::steady_clock::time_point last_stats{};
    MetricsRegistry metrics_registry;
};

}  // namespace winject

#endif  // WINJECT_L3_NODE_IMPL_H_
