#ifndef WINJECT_MANAGER_DATAGRAM_AIR_H_
#define WINJECT_MANAGER_DATAGRAM_AIR_H_

#include "Config.h"
#include "fec/RsBlockErasure.h"

#include <atomic>
#include <bfc/shared_sized_buffer.hpp>
#include <bfc/sized_buffer.hpp>
#include <bfc/timer.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace winject
{

// Air-side queue and optional RS FEC shared by UDP and queue upstreams.
// One app datagram in, zero or more air SDUs out via pull_tx; air SDUs in
// become app datagrams via the deliver callback. The FEC flush timer belongs
// to the caller. timer_wake runs after a timer is armed so a reactor blocked
// in its wait observes the new deadline.
class DatagramAir
{
public:
    using DeliverApp = std::function<void(const uint8_t* data, size_t len)>;
    using Wake = std::function<void()>;
    using FecTimer = bfc::timer<std::function<void()>>;

    DatagramAir() = default;
    ~DatagramAir();

    DatagramAir(const DatagramAir&) = delete;
    DatagramAir& operator=(const DatagramAir&) = delete;

    // log_name is a string literal ("udp", "queue") used in logs.
    bool start(Wake tx_wake, DeliverApp deliver_app, FecTimer* fec_timer,
               Wake timer_wake, const UpstreamConfig& cfg,
               const char* log_name);
    void stop();

    void accept_app_datagram(const uint8_t* data, size_t len);
    void on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec);

    bool has_tx();
    size_t get_tx_size();
    bfc::sized_buffer pull_tx(size_t max, bool* is_fec);
    void announce_down();

    bool set_fec(FecType type, int k, int n, std::string* error);
    void get_fec(FecType* type, int* k, int* n) const;
    bool set_fec_timeout_ms(int timeout_ms, std::string* error);
    int fec_timeout_ms() const;
    uint64_t tx_queue_drops() const
    {
        return tx_queue_drops_.load();
    }

    uint64_t app_rx_bytes() const
    {
        return app_rx_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t app_rx_packets() const
    {
        return app_rx_packets_.load(std::memory_order_relaxed);
    }
    uint64_t app_rx_oversize_pkt() const
    {
        return app_rx_oversize_pkt_.load(std::memory_order_relaxed);
    }
    uint64_t app_tx_bytes() const
    {
        return app_tx_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t app_tx_packets() const
    {
        return app_tx_packets_.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_bytes() const
    {
        return fec_air_rx_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_packets() const
    {
        return fec_air_rx_packets_.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_rx_unexpected() const
    {
        return fec_air_rx_unexpected_.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_tx_bytes() const
    {
        return fec_air_tx_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t fec_air_tx_packets() const
    {
        return fec_air_tx_packets_.load(std::memory_order_relaxed);
    }
    void tx_pending_stats(uint64_t* pkt, uint64_t* byt) const;
    uint64_t fec_recovered() const;
    uint64_t fec_decode_fail() const;

private:
    struct TxItem
    {
        bfc::sized_buffer pkt;
        bool is_fec = false;
    };

    template <typename F>
    void queue_tx(F&& queue);

    void push_tx(bfc::sized_buffer pkt, bool is_fec);
    bool has_tx_locked() const;
    void enqueue_fec(const std::vector<std::vector<uint8_t>>& shards);
    void deliver(const uint8_t* data, size_t len);
    void arm_rx_timer();
    void cancel_fec_timer_locked();
    void on_fec_timer(uint64_t generation);
    void on_rx_timer();
    void flush_fec_locked();
    void sync_fec_timer_after_push();

    using FecTimerId = FecTimer::timer_id_t;

    Wake tx_wake_;
    DeliverApp deliver_app_;
    Wake timer_wake_;
    FecTimer* fec_timer_ = nullptr;
    const char* log_name_ = "upstream";
    mutable std::mutex tx_mu_;
    std::deque<TxItem> txq_;
    RsBlockErasure fec_tx_;
    RsBlockErasure fec_rx_;
    bool running_ = false;
    // Timers capture this shared guard, never an unguarded endpoint pointer.
    // Lock order: guard mutex then tx_mu_. stop() fences an in-flight timer.
    struct TimerGuard
    {
        std::mutex mutex;
        DatagramAir* owner = nullptr;
    };
    std::shared_ptr<TimerGuard> timer_guard_;
    uint64_t tx_timer_generation_ = 0;
    bool rx_timer_armed_ = false;
    FecTimerId rx_timer_id_{};
    std::atomic<uint64_t> tx_queue_drops_{0};
    int fec_timeout_ms_ = RsBlockErasure::k_default_timeout_ms;
    std::atomic<uint64_t> app_rx_bytes_{0};
    std::atomic<uint64_t> app_rx_packets_{0};
    std::atomic<uint64_t> app_rx_oversize_pkt_{0};
    std::atomic<uint64_t> app_tx_bytes_{0};
    std::atomic<uint64_t> app_tx_packets_{0};
    std::atomic<uint64_t> fec_air_rx_bytes_{0};
    std::atomic<uint64_t> fec_air_rx_packets_{0};
    std::atomic<uint64_t> fec_air_rx_unexpected_{0};
    std::atomic<uint64_t> fec_air_tx_bytes_{0};
    std::atomic<uint64_t> fec_air_tx_packets_{0};
    bool fec_timer_armed_ = false;
    // Set under tx_mu_ when a timer is armed. queue_tx invokes timer_wake_
    // after releasing the mutex.
    bool timer_wake_pending_ = false;
    FecTimerId fec_timer_id_{};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_DATAGRAM_AIR_H_
