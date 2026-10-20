#ifndef WINJECT_MANAGER_HOST_ENDPOINT_H_
#define WINJECT_MANAGER_HOST_ENDPOINT_H_

#include "endpoint/DatagramAir.h"
#include "endpoint/Upstream.h"

#include <string>

namespace winject
{

// Application-facing upstream: datagrams in from the host, air SDUs out to
// TxMux, and the reverse. UDP and queue transports differ only in how a
// datagram is carried.
class HostEndpoint : public Upstream
{
public:
    ~HostEndpoint() override = default;

    void on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec) override;
    bool has_tx() override;
    size_t get_tx_size() override;
    bfc::sized_buffer pull_tx(size_t max, bool* is_fec) override;
    void announce_down() override;

    virtual void close() = 0;

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

    virtual const char* type_name() const = 0;
    virtual std::string endpoint_rx() const = 0;
    virtual std::string endpoint_tx() const = 0;

protected:
    bool start_air(DatagramAir::Wake tx_wake, DatagramAir::FecTimer* fec_timer,
                   DatagramAir::Wake timer_wake, const UpstreamConfig& cfg,
                   const char* log_name);
    void stop_air();
    void accept_app_datagram(const uint8_t* data, size_t len);

    virtual bool can_deliver_app() const = 0;
    virtual void deliver_app(const uint8_t* data, size_t len) = 0;

private:
    DatagramAir air_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_HOST_ENDPOINT_H_
