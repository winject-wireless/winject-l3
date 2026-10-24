#ifndef WINJECT_MANAGER_DATAGRAM_AIR_H_
#define WINJECT_MANAGER_DATAGRAM_AIR_H_

#include "Config.h"
#include "endpoint/UpstreamQueues.h"
#include "endpoint/UpstreamStats.h"
#include "fec/RsBlockErasure.h"

#include <atomic>
#include <bfc/cv_reactor.hpp>
#include <bfc/shared_sized_buffer.hpp>
#include <bfc/sized_buffer.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace winject
{

// Optional RS FEC shared by UDP and queue upstreams. Encoded packets are
// pushed immediately into the caller-supplied, reactor-bound TX queue.
// One app datagram in, zero or more air SDUs pushed to tx_queue; air SDUs in
// become app datagrams via the deliver callback. The shared CV reactor owns
// the FEC timers and is woken whenever a new deadline is armed.
class DatagramAir
{
public:
    using DeliverApp = std::function<void(const uint8_t* data, size_t len)>;
    using Reactor = bfc::cv_reactor<>;
    using FecTimer = bfc::timer<bfc::light_function<void()>>;

    DatagramAir() = default;
    ~DatagramAir();

    DatagramAir(const DatagramAir&) = delete;
    DatagramAir& operator=(const DatagramAir&) = delete;

    // log_name is a string literal ("udp", "queue") used in logs.
    bool start(std::shared_ptr<BoundedLossyQueue> tx_queue,
               DeliverApp deliver_app, std::shared_ptr<Reactor> reactor,
               const UpstreamConfig& cfg, uint8_t route_bus,
               UpstreamStats* tx_stats, std::mutex* tx_stats_mu,
               const char* log_name);
    void stop();
    void set_lifetime(std::weak_ptr<void> owner)
    {
        timer_guard->lifetime = std::move(owner);
        timer_guard->managed = true;
    }

    // UDP truncation is known before enqueue; account it without touching FEC.
    void reject_oversize_datagram(size_t len)
    {
        app_tx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
        app_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
        app_rx_oversize_pkt_acc.fetch_add(1, std::memory_order_relaxed);
    }
    void accept_app_datagram(const uint8_t* data, size_t len);
    void on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec);

    void announce_down();

    bool set_fec(FecType type, int k, int n, std::string* error);
    void get_fec(FecType* type, int* k, int* n) const;
    bool set_fec_timeout_ms(int timeout_ms, std::string* error);
    int fec_timeout_ms() const;
    uint64_t tx_queue_drops() const
    {
        return tx_queue_drops_acc.load();
    }

    uint64_t app_rx_bytes() const
    {
        return app_rx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_rx_packets() const
    {
        return app_rx_packets_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_rx_oversize_pkt() const
    {
        return app_rx_oversize_pkt_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_tx_bytes() const
    {
        return app_tx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t app_tx_packets() const
    {
        return app_tx_packets_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_bytes() const
    {
        return fec_air_rx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_packets() const
    {
        return fec_air_rx_packets_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_unexpected() const
    {
        return fec_air_rx_unexpected_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_tx_bytes() const
    {
        return fec_air_tx_bytes_acc.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_tx_packets() const
    {
        return fec_air_tx_packets_acc.load(std::memory_order_relaxed);
    }
    void tx_pending_stats(uint64_t* pkt, uint64_t* byt) const;
    uint64_t fec_recovered() const;
    uint64_t fec_decode_fail() const;

private:
    template <typename F>
    void queue_tx(F&& queue);

    void push_tx(bfc::sized_buffer pkt, bool is_fec);
    void enqueue_fec(const std::vector<std::vector<uint8_t>>& shards);
    void deliver(const uint8_t* data, size_t len);
    void arm_rx_timer();
    void cancel_fec_timer_locked();
    void on_fec_timer(uint64_t generation);
    void on_rx_timer();
    void flush_fec_locked();
    void sync_fec_timer_after_push();

    using FecTimerId = FecTimer::timer_id_t;

    std::shared_ptr<BoundedLossyQueue> tx_queue;
    uint8_t route_bus = 0;
    UpstreamStats* tx_stats = nullptr;
    std::mutex* tx_stats_mu = nullptr;
    DeliverApp deliver_app;
    std::shared_ptr<Reactor> reactor;
    const char* log_name = "upstream";
    mutable std::mutex tx_mu;
    RsBlockErasure fec_tx;
    RsBlockErasure fec_rx;
    bool running = false;
    // Timers capture this shared guard, never an unguarded endpoint pointer.
    // Lock order: guard mutex then tx_mu. stop() fences an in-flight timer.
    struct TimerGuard
    {
        std::mutex mutex;
        DatagramAir* owner = nullptr;
        std::weak_ptr<void> lifetime;
        bool managed = false;
    };
    std::shared_ptr<TimerGuard> timer_guard;
    uint64_t tx_timer_generation = 0;
    bool rx_timer_armed = false;
    FecTimerId rx_timer_id{};
    std::atomic<uint64_t> tx_queue_drops_acc{0};
    int fec_timeout_setting = RsBlockErasure::k_default_timeout_ms;
    std::atomic<uint64_t> app_rx_bytes_acc{0};
    std::atomic<uint64_t> app_rx_packets_acc{0};
    std::atomic<uint64_t> app_rx_oversize_pkt_acc{0};
    std::atomic<uint64_t> app_tx_bytes_acc{0};
    std::atomic<uint64_t> app_tx_packets_acc{0};
    std::atomic<uint64_t> fec_air_rx_bytes_acc{0};
    std::atomic<uint64_t> fec_air_rx_packets_acc{0};
    std::atomic<uint64_t> fec_air_rx_unexpected_acc{0};
    std::atomic<uint64_t> fec_air_tx_bytes_acc{0};
    std::atomic<uint64_t> fec_air_tx_packets_acc{0};
    bool fec_timer_armed = false;
    // Set under tx_mu when a timer is armed. queue_tx wakes the reactor
    // after releasing the mutex.
    bool timer_wake_pending = false;
    FecTimerId fec_timer_id{};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_DATAGRAM_AIR_H_
