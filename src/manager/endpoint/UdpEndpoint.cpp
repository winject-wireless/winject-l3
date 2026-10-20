#include "endpoint/UdpEndpoint.h"

#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

namespace winject
{

UdpEndpoint::~UdpEndpoint()
{
    close();
}

bool UdpEndpoint::open(IOReactor& io_reactor, const UpstreamConfig& cfg,
                       std::function<void()> tx_wake)
{
    this->reactor = &io_reactor;
    mode = upstream_mode(cfg.endpoint);
    sock = bfc::socket(bfc::create_udp4());
    if (sock.fd() < 0)
    {
        LOG_ERR("udp socket: %s", strerror(errno));
        return false;
    }
    const int one = 1;
    sock.set_sock_opt(SOL_SOCKET, SO_REUSEADDR, one);

    const UdpPeerEndpoint& ep = cfg.endpoint;
    peer_endpoints_ = ep;
    bool opened = false;
    if (mode == UpstreamMode::udp_static)
    {
        sockaddr_in bind_addr = {};
        if (!parse_host_port(ep.rx, &bind_addr) || sock.bind(bind_addr) < 0)
        {
            LOG_ERR("udp bind %s: %s", ep.rx.c_str(), strerror(errno));
            opened = false;
        }
        else
        {
            dest_valid = parse_host_port(ep.tx, &dest);
            if (!dest_valid)
            {
                LOG_ERR("udp tx %s invalid", ep.tx.c_str());
                opened = false;
            }
            else
            {
                opened = true;
            }
        }
    }
    else if (mode == UpstreamMode::udp_server)
    {
        sockaddr_in bind_addr = {};
        if (!parse_host_port(ep.rx, &bind_addr) || sock.bind(bind_addr) < 0)
        {
            LOG_ERR("udp bind %s: %s", ep.rx.c_str(), strerror(errno));
            opened = false;
        }
        else
        {
            opened = true;
        }
    }
    else
    {
        if (!parse_host_port(ep.tx, &dest))
        {
            LOG_ERR("udp tx %s invalid", ep.tx.c_str());
            opened = false;
        }
        else
        {
            dest_valid = true;
            sockaddr_in bind_addr = {};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            bind_addr.sin_port = 0;
            if (sock.bind(bind_addr) < 0)
            {
                LOG_ERR("udp client bind: %s", strerror(errno));
                opened = false;
            }
            else
            {
                opened = true;
            }
        }
    }

    if (!opened)
    {
        return false;
    }
    if (!start_air(std::move(tx_wake), &io_reactor.get_timer(), nullptr, cfg,
                   "udp"))
    {
        return false;
    }
    return io_reactor.add_read_rdy(sock.fd(),
                                   [this]()
                                   {
                                       on_app();
                                   });
}

void UdpEndpoint::close()
{
    stop_air();
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        bfc::socket(std::move(sock));
    }
}

bool UdpEndpoint::can_deliver_app() const
{
    return sock.fd() >= 0 && dest_valid;
}

void UdpEndpoint::deliver_app(const uint8_t* data, size_t len)
{
    const bfc::const_buffer_view view(reinterpret_cast<const std::byte*>(data),
                                      len);
    sock.send(view, 0, reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

void UdpEndpoint::on_app()
{
    constexpr int k_max_rx_per_wakeup = 8;
    for (int drained = 0; drained < k_max_rx_per_wakeup; ++drained)
    {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        if (rx_buf.capacity() < 2048)
        {
            rx_buf.reserve(2048);
        }
        rx_buf.resize(2048);
        const ssize_t n =
            sock.recv(rx_buf, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from),
                      &from_len);
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
            if (errno == ECONNREFUSED)
            {
                // Async ICMP for a prior sendto() (typically only with
                // IP_RECVERR, which we do not enable). Likely dead on Linux
                // today; kept as a guard — do not clear dest_valid (fixed
                // dest in udp_client/static).
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            LOG_ERR("udp app recv: %s", strerror(errno));
            return;
        }
        if (n == 0)
        {
            continue;
        }
        if (mode == UpstreamMode::udp_server)
        {
            // Last sender gets replies. Camera/master also seed with "ok\n"
            // at connect; that is just another datagram. A stray nc to the
            // camera bind steals until camera sends again — don't nc :21092
            // while camera is running.
            dest = from;
            dest_valid = true;
        }
        accept_app_datagram(reinterpret_cast<const uint8_t*>(rx_buf.data()),
                            static_cast<size_t>(n));
    }
}

}  // namespace winject
