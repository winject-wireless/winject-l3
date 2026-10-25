#include "radio/WifiUdp.h"

#include "console/ConsoleParse.h"
#include "radio/WifiFcs.h"
#include "utils/Log.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <variant>

namespace winject
{

bool WifiUdp::forward_trailer_ok(const uint8_t* frame, size_t len) const
{
    return wifi_forward_trailer_ok(frame, len, fcs_mode());
}

WifiUdp::~WifiUdp()
{
    close();
}

bool WifiUdp::open(IOReactor& reactor, rx on_rx, idle on_idle)
{
    close();
    this->reactor = &reactor;
    this->on_rx = std::move(on_rx);
    this->on_idle = std::move(on_idle);
    sock = bfc::socket(bfc::create_udp4());
    if (sock.fd() < 0)
    {
        LOG_ERR("radio udp socket failed: %s", strerror(errno));
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

void WifiUdp::set_tx_dplane(const sockaddr_in& dplane)
{
    tx_dplane = dplane;
    have_tx_dplane = true;
}

void WifiUdp::post_rx_event(RxEvent ev)
{
    // App::radio outlives reactor.run(); callback captures this safely.
    reactor->wake_up(
        [this, ev = std::move(ev)]()
        {
            handle_rx_event(ev);
        });
}

void WifiUdp::handle_rx_event(const RxEvent& ev)
{
    std::visit(
        [this](const auto& e)
        {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, EventCtrlRecvSocketChange>)
            {
                set_rx_dplane(e.dplane);
                LOG_INF("rx dplane -> %s",
                        console_format_ipv4_port(e.dplane).c_str());
            }
        },
        ev);
}

bool WifiUdp::set_rx_dplane(const sockaddr_in& dplane)
{
    rx_dplane = dplane;
    have_rx_dplane = true;
    return register_forward();
}

bool WifiUdp::register_forward()
{
    if (sock.fd() < 0 || !have_rx_dplane)
    {
        return false;
    }
    static const uint8_t k_reg = 0;
    const bfc::const_buffer_view reg_view(
        reinterpret_cast<const std::byte*>(&k_reg), 1);
    const ssize_t sent = sock.send(
        reg_view, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&rx_dplane),
        sizeof(rx_dplane));
    if (sent < 0)
    {
        LOG_ERR("radio dplane_rx register failed: %s", strerror(errno));
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
    have_tx_dplane = false;
    have_rx_dplane = false;
}

bool WifiUdp::send(const uint8_t* mpdu, size_t len)
{
    if (sock.fd() < 0 || !have_tx_dplane || mpdu == nullptr || len == 0)
    {
        return false;
    }
    if (len < 24 || len > k_tx_mpdu_max)
    {
        return false;
    }
    const bfc::const_buffer_view view(reinterpret_cast<const std::byte*>(mpdu),
                                      len);
    const ssize_t sent = sock.send(
        view, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&tx_dplane),
        sizeof(tx_dplane));
    if (sent != static_cast<ssize_t>(len))
    {
        return false;
    }
    tx_pkt_acc.fetch_add(1, std::memory_order_relaxed);
    tx_byte_acc.fetch_add(len, std::memory_order_relaxed);
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
        if (rx_buf.capacity() < k_recv_capacity)
        {
            rx_buf.reserve(k_recv_capacity);
        }
        rx_buf.resize(k_recv_capacity);
        const ssize_t n =
            sock.recv(rx_buf, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&peer),
                      &peer_len);
        if (n >= 0)
        {
            rx_buf.resize(static_cast<size_t>(n));
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
        rx_pkt_acc.fetch_add(1, std::memory_order_relaxed);
        rx_byte_acc.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        const size_t len = static_cast<size_t>(n);
        if (len < 28 || len > k_rx_mpdu_max + 4)
        {
            continue;
        }
        const uint8_t* frame = reinterpret_cast<const uint8_t*>(rx_buf.data());
        if (fcs_mode() == RadioFcsMode::unknown)
        {
            fcs_unknown_pkt_acc.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!forward_trailer_ok(frame, len))
        {
            fcs_error_pkt_acc.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        rx_buf.resize(len - 4);
        if (on_rx)
        {
            on_rx(std::move(rx_buf));
        }
    }
    if (any && on_idle)
    {
        on_idle();
    }
}

}  // namespace winject
