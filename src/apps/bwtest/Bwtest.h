#ifndef WINJECT_APPS_BWTEST_BWTEST_H_
#define WINJECT_APPS_BWTEST_BWTEST_H_

#include "endpoint/UpstreamQueue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
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

struct Config
{
    std::shared_ptr<winject::UpstreamQueue> upstream;
    // UDP console, one command per datagram, reply to the sender.
    std::string console = "0.0.0.0:5100";
    // Datagram size; at most winject::k_stream_payload_max.
    size_t payload_bytes = 1400;
};

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
};

class Bwtest
{
public:
    Bwtest(std::shared_ptr<winject::UpstreamQueue> upstream,
           size_t payload_bytes);
    ~Bwtest();
    Bwtest(const Bwtest&) = delete;
    Bwtest& operator=(const Bwtest&) = delete;

    // Starts a send of offer_kbps for duration_ms on its own thread. False
    // when a send is running or an argument is 0.
    bool start_send(uint32_t offer_kbps, uint32_t duration_ms);
    bool sending() const;
    // Drains the upstream's receive queue once; returns datagrams taken.
    size_t poll_receive();
    Stats stats() const;
    void clear();
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
