#include "radio/WifiUdp.h"

#include "radio/WifiFcs.h"
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
    // D-plane RX is server mode on the radio: register with an empty datagram
    // so the radio learns our return address for forwarded MPDUs.
    if (!register_forward())
    {
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

bool WifiUdp::register_forward()
{
    if (sock.fd() < 0 || forward_port_ == 0)
    {
        return false;
    }
    sockaddr_in reg = inject;
    reg.sin_port = htons(forward_port_);
    static const uint8_t k_reg = 0;
    const bfc::const_buffer_view reg_view(
        reinterpret_cast<const std::byte*>(&k_reg), 1);
    const ssize_t sent = sock.send(
        reg_view, 0, reinterpret_cast<const sockaddr*>(&reg), sizeof(reg));
    if (sent < 0)
    {
        LOG_ERR("radio dplane_rx register %u failed: %s", forward_port_,
                strerror(errno));
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
    tx_pkt_.fetch_add(1, std::memory_order_relaxed);
    tx_byte_.fetch_add(len, std::memory_order_relaxed);
    return true;
}

void WifiUdp::on_forward()
{
    bool any = false;
    constexpr int k_max_rx_per_wakeup = 8;
    for (int drained = 0; drained < k_max_rx_per_wakeup; ++drained)
    {
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        if (rx_buf_.capacity() < k_recv_capacity)
        {
            rx_buf_.reserve(k_recv_capacity);
        }
        rx_buf_.resize(k_recv_capacity);
        const ssize_t n =
            sock.recv(rx_buf_, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&peer),
                      &peer_len);
        if (n >= 0)
        {
            rx_buf_.resize(static_cast<size_t>(n));
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_ERR("radio recv: %s", strerror(errno));
            break;
        }
        if (n == 0)
        {
            continue;
        }
        any = true;
        rx_pkt_.fetch_add(1, std::memory_order_relaxed);
        rx_byte_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        const size_t len = static_cast<size_t>(n);
        if (len < 28 || len > k_mpdu_max + 4)
        {
            continue;
        }
        const uint8_t* frame = reinterpret_cast<const uint8_t*>(rx_buf_.data());
        if (!wifi_fcs_matches(frame, len))
        {
            fcs_error_pkt_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        rx_buf_.resize(len - 4);
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
