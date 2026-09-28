#include "radio/WifiUdp.h"

#include "utils/Log.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

namespace winject
{

WifiUdp::~WifiUdp()
{
    close();
}

bool WifiUdp::open(IOReactor& reactor, const sockaddr_in& inject,
                   uint16_t forward_port, rx on_rx, idle on_idle)
{
    close();
    this->reactor = &reactor;
    this->inject = inject;
    this->on_rx = std::move(on_rx);
    this->on_idle = std::move(on_idle);
    forward_port_ = forward_port;
    sock = bfc::socket(bfc::create_udp4());
    if (sock.fd() < 0)
    {
        LOG_ERR("radio udp socket failed: %s", strerror(errno));
        close();
        return false;
    }
    // Forward port bind must be exclusive or a stale manager can keep stealing
    // unicast packets while this process shows radio_rx=0.
    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(forward_port_);
    if (sock.bind(bind_addr) < 0)
    {
        LOG_ERR("radio udp bind %u failed: %s", forward_port_, strerror(errno));
        close();
        return false;
    }
    if (!reactor.add_read_rdy(sock.fd(),
                              [this]()
                              {
                                  on_forward();
                              }))
    {
        close();
        return false;
    }
    return true;
}

void WifiUdp::close()
{
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        bfc::socket(std::move(sock));
    }
    forward_port_ = 0;
}

bool WifiUdp::send(const uint8_t* mpdu, size_t len)
{
    if (sock.fd() < 0 || mpdu == nullptr || len == 0)
    {
        return false;
    }
    if (len < 24 || len > k_mpdu_max)
    {
        return false;
    }
    const bfc::const_buffer_view view(reinterpret_cast<const std::byte*>(mpdu),
                                       len);
    const ssize_t sent = sock.send(
        view, 0, reinterpret_cast<const sockaddr*>(&inject), sizeof(inject));
    if (sent != static_cast<ssize_t>(len))
    {
        return false;
    }
    tx_pkt_++;
    tx_byte_ += len;
    return true;
}

void WifiUdp::on_forward()
{
    bool any = false;
    while (true)
    {
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        if (rx_buf_.capacity() < k_recv_capacity)
        {
            rx_buf_.reserve(k_recv_capacity);
        }
        rx_buf_.resize(k_recv_capacity);
        const ssize_t n = sock.recv(
            rx_buf_, 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (n >= 0)
        {
            rx_buf_.resize(static_cast<size_t>(n));
        }
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_ERR("radio recv: %s", strerror(errno));
            break;
        }
        if (n == 0)
        {
            break;
        }
        any = true;
        rx_pkt_++;
        rx_byte_ += static_cast<uint64_t>(n);
        if (on_rx)
        {
            on_rx(std::move(rx_buf_));
        }
    }
    if (any && on_idle)
    {
        on_idle();
    }
}

}  // namespace winject
