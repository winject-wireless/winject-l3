#include "endpoint/HostEndpoint.h"

namespace winject
{

void HostEndpoint::on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec)
{
    if (pkt.empty() || !can_deliver_app())
    {
        return;
    }
    air_.on_radio_rx(std::move(pkt), is_fec);
}

bool HostEndpoint::has_tx()
{
    return air_.has_tx();
}

size_t HostEndpoint::get_tx_size()
{
    return air_.get_tx_size();
}

bfc::sized_buffer HostEndpoint::pull_tx(size_t max, bool* is_fec)
{
    return air_.pull_tx(max, is_fec);
}

void HostEndpoint::announce_down()
{
    air_.announce_down();
}

bool HostEndpoint::set_fec(FecType type, int k, int n, std::string* error)
{
    return air_.set_fec(type, k, n, error);
}

void HostEndpoint::get_fec(FecType* type, int* k, int* n) const
{
    air_.get_fec(type, k, n);
}

bool HostEndpoint::set_fec_timeout_ms(int timeout_ms, std::string* error)
{
    return air_.set_fec_timeout_ms(timeout_ms, error);
}

int HostEndpoint::fec_timeout_ms() const
{
    return air_.fec_timeout_ms();
}

uint64_t HostEndpoint::app_rx_bytes() const
{
    return air_.app_rx_bytes();
}

uint64_t HostEndpoint::app_rx_packets() const
{
    return air_.app_rx_packets();
}

uint64_t HostEndpoint::app_rx_oversize_pkt() const
{
    return air_.app_rx_oversize_pkt();
}

uint64_t HostEndpoint::app_tx_bytes() const
{
    return air_.app_tx_bytes();
}

uint64_t HostEndpoint::app_tx_packets() const
{
    return air_.app_tx_packets();
}

uint64_t HostEndpoint::fec_air_rx_bytes() const
{
    return air_.fec_air_rx_bytes();
}

uint64_t HostEndpoint::fec_air_rx_packets() const
{
    return air_.fec_air_rx_packets();
}

uint64_t HostEndpoint::fec_air_rx_unexpected() const
{
    return air_.fec_air_rx_unexpected();
}

uint64_t HostEndpoint::fec_air_tx_bytes() const
{
    return air_.fec_air_tx_bytes();
}

uint64_t HostEndpoint::fec_air_tx_packets() const
{
    return air_.fec_air_tx_packets();
}

void HostEndpoint::tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
{
    air_.tx_pending_stats(pkt, byt);
}

uint64_t HostEndpoint::fec_recovered() const
{
    return air_.fec_recovered();
}

uint64_t HostEndpoint::fec_decode_fail() const
{
    return air_.fec_decode_fail();
}

bool HostEndpoint::start_air(DatagramAir::Wake tx_wake,
                             DatagramAir::FecTimer* fec_timer,
                             DatagramAir::Wake timer_wake,
                             const UpstreamConfig& cfg, const char* log_name)
{
    return air_.start(
        std::move(tx_wake),
        [this](const uint8_t* data, size_t len)
        {
            deliver_app(data, len);
        },
        fec_timer, std::move(timer_wake), cfg, log_name);
}

void HostEndpoint::stop_air()
{
    air_.stop();
}

void HostEndpoint::accept_app_datagram(const uint8_t* data, size_t len)
{
    air_.accept_app_datagram(data, len);
}

}  // namespace winject
