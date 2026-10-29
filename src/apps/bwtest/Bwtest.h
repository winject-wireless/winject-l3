#ifndef WINJECT_APPS_BWTEST_BWTEST_H_
#define WINJECT_APPS_BWTEST_BWTEST_H_

#include "endpoint/UpstreamQueue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Bandwidth tester on an L3-owned queue upstream (Config::on_upstream_queue).
// Sends paced fixed-size datagrams on request and counts what arrives; the
// peer runs the same app on the same upstream with the buses swapped. Air
// loss is the peer's L3 `lur` rxgap; this app counts only its own queue ends.
namespace app_bwtest
{

// An app command on the tester's console: gets the text after the command
// name, returns the reply (newline-terminated).
using Command = std::function<std::string(const std::string& args)>;

struct Config
{
    std::shared_ptr<winject::UpstreamQueue> upstream;
    // UDP console, one command per datagram, reply to the sender.
    std::string console = "0.0.0.0:5100";
    // Datagram size including timestamp header; 16..k_stream_payload_max.
    size_t payload_bytes = 1400;
    // Extra console commands from the host app (e.g. video_start). Names of
    // the tester's own commands are ignored.
    std::map<std::string, Command> commands;
};

// Timestamped packets need a 16-byte header. Clocks must share Unix time.
constexpr size_t k_timestamp_header_bytes = 16;
using NowUs = std::function<int64_t()>;

struct Stats
{
    // Datagrams offered to L3 by test_send, including dropped ones.
    uint64_t snd_pkt = 0;
    uint64_t snd_byt = 0;
    // Offered datagrams L3 never took: rejected by try_send, or evicted from
    // the full app TX queue before L3 drained it.
    uint64_t snd_drop = 0;
    uint64_t rcv_pkt = 0;
    uint64_t rcv_byt = 0;
    uint64_t lat_samples = 0;
    uint64_t lat_invalid = 0; // legacy/short packets or future/zero timestamps
    uint64_t lat_last_us = 0;
    uint64_t lat_min_us = 0;
    uint64_t lat_avg_us = 0;
    uint64_t lat_max_us = 0;
};

class Bwtest
{
public:
    Bwtest(std::shared_ptr<winject::UpstreamQueue> upstream,
           size_t payload_bytes, NowUs now_us = {});
    ~Bwtest();
    Bwtest(const Bwtest&) = delete;
    Bwtest& operator=(const Bwtest&) = delete;

    // Starts a send of offer_kbps for duration_ms on its own thread. False
    // when a send is running, an argument is 0, or payload size is invalid.
    bool start_send(uint32_t offer_kbps, uint32_t duration_ms);
    bool sending() const;
    // Drains the upstream's receive queue once; returns datagrams taken.
    size_t poll_receive();
    Stats stats() const;
    void clear();
    // Adds an app command; false (and ignored) for a built-in name.
    bool add_command(const std::string& name, Command command);
    // One console command in, its reply out (newline-terminated).
    std::string handle_command(const std::string& line);

private:
    void run_send(uint32_t offer_kbps, uint32_t duration_ms);
    uint64_t evictions() const;

    std::shared_ptr<winject::UpstreamQueue> upstream_;
    size_t payload_bytes_;
    std::mutex send_mu_;
    std::thread send_thread_;
    std::atomic<bool> sending_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> snd_pkt_{0};
    std::atomic<uint64_t> snd_byt_{0};
    std::atomic<uint64_t> snd_reject_{0};
    // UpstreamQueue::ingress_evictions at the last clear().
    std::atomic<uint64_t> eviction_base_{0};
    std::atomic<uint64_t> rcv_pkt_{0};
    std::atomic<uint64_t> rcv_byt_{0};
    uint32_t send_seq_ = 0;
    NowUs now_us_;
    mutable std::mutex latency_mu_;
    Stats latency_;
    uint64_t latency_sum_us_ = 0;
    std::map<std::string, Command> commands_;
};

// Runs the console and the receive poller until stop(). Blocks; returns 0,
// or 1 when the config or console socket is unusable. Threads it starts
// inherit the caller's thread settings (on ESP-IDF, esp_pthread_set_cfg with
// inherit_cfg).
int main(const Config& config);

// Asks a running main() to return. Safe from any thread.
void stop();

}  // namespace app_bwtest

#endif  // WINJECT_APPS_BWTEST_BWTEST_H_
