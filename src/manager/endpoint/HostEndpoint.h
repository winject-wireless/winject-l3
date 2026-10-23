#ifndef WINJECT_MANAGER_HOST_ENDPOINT_H_
#define WINJECT_MANAGER_HOST_ENDPOINT_H_

#include "endpoint/DatagramAir.h"
#include "endpoint/Upstream.h"
#include "utils/IOReactor.h"

#include <memory>
#include <mutex>
#include <string>

namespace winject
{

// Application-facing upstream: datagrams in from the host, air SDUs out to
// TxMux, and the reverse. UDP and queue transports differ only in how a
// datagram is carried.
class HostEndpoint final : public Upstream
{
public:
    HostEndpoint() = default;
    ~HostEndpoint() override;

    // open/close are serialized by the owner on the manager IO reactor thread
    // (or while it is stopped). A second open fails without disturbing the
    // session.
    bool open(IOReactor& reactor, const UpstreamConfig& cfg,
              std::function<void()> tx_wake = {});
    bool is_open() const;

    void on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec) override;
    bool has_tx() override;
    size_t get_tx_size() override;
    bfc::sized_buffer pull_tx(size_t max, bool* is_fec) override;
    void announce_down() override;

    void close();

    bool set_fec(FecType type, int k, int n, std::string* error);
    void get_fec(FecType* type, int* k, int* n) const;
    bool set_fec_timeout_ms(int timeout_ms, std::string* error);
    int fec_timeout_ms() const;
    uint64_t app_rx_bytes() const;
    uint64_t app_rx_packets() const;
    uint64_t app_rx_oversize_pkt() const;
    uint64_t app_tx_bytes() const;
    uint64_t app_tx_packets() const;
    uint64_t fec_air_rx_bytes() const;
    uint64_t fec_air_rx_packets() const;
    uint64_t fec_air_rx_unexpected() const;
    uint64_t fec_air_tx_bytes() const;
    uint64_t fec_air_tx_packets() const;
    void tx_pending_stats(uint64_t* pkt, uint64_t* byt) const;
    uint64_t fec_recovered() const;
    uint64_t fec_decode_fail() const;
    uint64_t tx_queue_drops() const;

    const char* type_name() const;
    std::string endpoint_rx() const;
    std::string endpoint_tx() const;

    // Local transport outcomes, per open session. Acceptance does not mean peer
    // consumption. A queue can accept a new datagram while evicting an older
    // one.
    struct TransportCounters
    {
        uint64_t delivery_attempts = 0;
        uint64_t accepted_packets = 0;
        uint64_t accepted_bytes = 0;
        uint64_t no_peer_drops = 0;
        uint64_t closed_drops = 0;
        uint64_t send_errors = 0;
        uint64_t queue_evictions = 0;
    };
    TransportCounters transport_counters() const;

private:
    struct Session;
    std::shared_ptr<Session> session() const;
    mutable std::mutex session_mu_;
    std::shared_ptr<Session> session_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_HOST_ENDPOINT_H_
