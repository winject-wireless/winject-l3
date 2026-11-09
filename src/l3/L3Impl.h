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
#include "winject_l3/L3.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace winject::l3::detail
{

struct L3Core;

// The core types l3 is built from.
using core::BoundedLossyQueue;
using core::IOReactor;
using core::MetricsRegistry;
using core::OneShotTimer;
using core::RadioCapsReply;
using core::RadioDplane;
using core::RadioMplaneClient;
using core::RadioPhyReply;
using core::RadioSupervisor;
using core::RadioUpstreamTable;
using core::RxDemux;
using core::TxMux;
using core::UpstreamConfig;
using core::UpstreamLink;
using core::WinjectVersion;

// The l3 applet behind the handles: reactor, data path and the radio. Every
// method below runs on l3's reactor thread; the public handles (winject::L3,
// Upstream, Radio, ...) post to it. l3 never calls user code there except
// UpstreamSpec::on_queue, and never waits on another thread.
class L3Impl
{
public:
    template <class T>
    using Result = winject::Result<T>;
    using Status = winject::Status;
    template <class T>
    using Reply = std::function<void(Result<T>)>;
    struct Empty
    {
    };

    explicit L3Impl(unsigned stats_sec);
    ~L3Impl();
    L3Impl(const L3Impl&) = delete;
    L3Impl& operator=(const L3Impl&) = delete;

    // Before start(): run on the host's reactors, by bfc registry name
    // (L3::Reactors); empty keeps a reactor and thread of l3's own. False
    // when a named reactor is not registered.
    bool use_host_reactors(const std::string& io, const std::string& upstream);
    // Starts the upstream reactor thread and l3's thread, attached to
    // core; on a host reactor it posts l3's setup there instead.
    // thread_setup runs on this thread before each thread starts.
    void start(winject::l3::detail::L3Core* core,
               const L3::ThreadSetup& thread_setup);
    // Waits until l3 has shut down after stop(): its thread exited, or on a
    // host reactor its teardown ran and every wake-up queued before it.
    void join();
    void stop();
    winject::core::IOReactor& io_reactor()
    {
        return *reactor;
    }

    // Upstreams.
    Status add_upstream(const winject::UpstreamSpec& spec);
    Status remove_upstream(uint8_t id);
    bool has_upstream(uint8_t id) const;
    std::vector<uint8_t> upstream_ids() const;
    Result<winject::UpstreamSpec> upstream_spec(uint8_t id) const;
    Result<winject::UpstreamSpec> update_upstream(
        uint8_t id, const winject::UpstreamPatch& patch);
    Result<winject::UpstreamRxStats> upstream_rx_stats(uint8_t id) const;
    Result<winject::UpstreamTxStats> upstream_tx_stats(uint8_t id) const;
    Result<std::vector<winject::Metric>> metrics(
        const std::vector<std::string>& keys);

    // The radio.
    Status create_radio(const winject::RadioSpec& spec);
    Status add_radio();
    Status remove_radio();
    bool has_radio() const
    {
        return radio_created;
    }
    Result<winject::RadioState> radio_state() const;
    Result<winject::RadioSpec> radio_spec() const;
    Result<winject::RadioDevice> radio_device() const;
    Status set_radio_device(const winject::RadioDevice& device);
    void set_radio_phy(const winject::RadioPhyPatch& patch,
                       Reply<winject::RadioInfo> reply);
    void radio_info(Reply<winject::RadioInfo> reply);
    void radio_caps(Reply<winject::RadioCaps> reply);
    void radio_stats(Reply<winject::RadioStats> reply);
    void set_config_slot(uint8_t slot, Reply<Empty> reply);
    void radio_reset(Reply<Empty> reply);
    void radio_mplane(const std::string& line, Reply<std::string> reply);

private:
    void loop();
    // loop() without the run: everything before reactor->run() (on l3's
    // reactor thread), and the shutdown after it.
    void setup();
    void finish();
    // finish() on a host IO reactor: once, on its thread; leaves it running.
    void finish_on_host();
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
    winject::UpstreamSpec upstream_spec_at(size_t index) const;
    void apply_radio_caps(const RadioCapsReply& caps);
    void apply_radio_dplane();
    void close_radio();
    void apply_configured_fcs();
    const char* radio_unavailable_error() const;
    winject::RadioInfo radio_info_from(const RadioPhyReply& reply) const;
    void on_radio_version(bool known, const WinjectVersion& v);
    void log_proto_mismatch_once(const WinjectVersion& radio_ver);

    static constexpr std::chrono::milliseconds k_reconnect_interval{1000};

    winject::l3::detail::L3Core* core = nullptr;
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
    std::thread l3_thread;
    // Host reactors (use_host_reactors): no thread of l3's own for them.
    bool host_io = false;
    bool host_upstream = false;
    // Host IO reactor: teardown ran (finish_on_host), and its signal.
    bool host_finished = false;
    std::promise<void> host_done;
    int registered_stop_fd = -1;
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

}  // namespace winject::l3::detail

#endif  // WINJECT_L3_NODE_IMPL_H_
