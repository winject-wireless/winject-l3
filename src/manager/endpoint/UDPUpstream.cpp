#include "endpoint/UDPUpstream.h"

#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <errno.h>
#include <future>
#include <stdexcept>
#include <sys/socket.h>
namespace winject
{
UDPUpstream::UDPUpstream(std::shared_ptr<Reactor> reactor,
                         std::shared_ptr<IOReactor> io,
                         const UpstreamConfig& config,
                         std::shared_ptr<BoundedLossyQueue> radio_tx_queue)
    : Upstream(std::move(reactor), config, std::move(radio_tx_queue)),
      io(std::move(io)),
      socket(bfc::create_udp4())
{
    if (!this->io || config.mode == UpstreamMode::queue || !config.reactor.empty() ||
        (config.endpoint.rx.empty() && config.endpoint.tx.empty()) ||
        socket.fd() < 0)
    {
        throw std::invalid_argument("invalid UDP upstream configuration");
    }
    app_tx_queue->set_capacity(config.rx_queue_capacity);
    upstream_config.mode = upstream_mode(config.endpoint);
    const int one = 1;
    socket.set_sock_opt(SOL_SOCKET, SO_REUSEADDR, one);
    sockaddr_in bind{};
    bind.sin_family = AF_INET;
    bind.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!config.endpoint.rx.empty() &&
        !parse_host_port(config.endpoint.rx, &bind))
    {
        throw std::invalid_argument("invalid UDP bind address");
    }
    if (!config.endpoint.tx.empty())
    {
        if (!parse_host_port(config.endpoint.tx, &destination))
        {
            throw std::invalid_argument("invalid UDP destination");
        }
        have_destination = true;
    }
    if (socket.bind(bind) != 0)
    {
        throw std::runtime_error("UDP upstream bind failed");
    }
}
void UDPUpstream::bind_callbacks()
{
    if (registered)
    {
        return;
    }
    Upstream::bind_callbacks();
    const auto weak = weak_from_this();
    registered = io->add_read_rdy(
        socket.fd(),
        [weak]
        {
            if (auto self = weak.lock())
            {
                static_cast<UDPUpstream&>(*self).on_socket_readable();
            }
        });
    if (!registered)
    {
        throw std::runtime_error("UDP upstream reactor registration failed");
    }
}
void UDPUpstream::teardown()
{
    // Fail before touching the socket so misuse leaves nothing half removed.
    if (reactor->is_reactor_thread())
    {
        throw std::logic_error("upstream teardown on cv reactor thread");
    }
    if (registered)
    {
        const int fd = socket.fd();
        if (io->is_reactor_thread())
        {
            io->rem_read_rdy(fd);
        }
        else
        {
            // Shared: a stopped IO reactor runs done_cb late or never.
            auto io_done = std::make_shared<std::promise<void>>();
            auto removed = io_done->get_future();
            io->rem_read_rdy(fd,
                             [io_done]()
                             {
                                 io_done->set_value();
                             });
            if (removed.wait_for(k_fence_timeout) != std::future_status::ready)
            {
                LOG_WRN("udp upstream teardown: io reactor fence timed out");
            }
        }
        registered = false;
    }
    Upstream::teardown();
}

UDPUpstream::~UDPUpstream()
{
    if (registered)
    {
        io->rem_read_rdy(socket.fd());
        registered = false;
    }
}
void UDPUpstream::on_socket_readable()
{
    for (int n = 0; n < 8; ++n)
    {
        bfc::sized_buffer buffer(2048);
        sockaddr_in from{};
        socklen_t from_size = sizeof(from);
        ssize_t received;
        {
            std::lock_guard<std::mutex> lock(socket_mu);
            received =
                socket.recv(buffer, MSG_DONTWAIT | MSG_TRUNC,
                             reinterpret_cast<sockaddr*>(&from), &from_size);
            if (received > 0 && upstream_config.mode == UpstreamMode::udp_server)
            {
                destination = from;
                have_destination = true;
            }
        }
        if (received < 0)
        {
            if (errno == EINTR || errno == ECONNREFUSED)
            {
                continue;
            }
            return;
        }
        // Preserve oversize rejection; never encode a silently truncated
        // datagram.
        if (received > 0)
        {
            const size_t size = static_cast<size_t>(received);
            if (size > buffer.size())
            {
                reject_oversize_datagram(size);
            }
            else
            {
                app_tx_queue->push(
                    bfc::shared_sized_buffer::copy_from(buffer.data(), size));
            }
        }
    }
}
void UDPUpstream::deliver_datagram(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> counters(rx_counters_mu);
    ++stored_rx_counters.delivery_attempts;
    std::lock_guard<std::mutex> socket_lock(socket_mu);
    if (!have_destination)
    {
        ++stored_rx_counters.no_peer_drops;
        return;
    }
    const bfc::const_buffer_view view(reinterpret_cast<const std::byte*>(data),
                                      len);
    const auto sent = this->socket.send(
        view, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    if (sent < 0 || static_cast<size_t>(sent) != len)
    {
        ++stored_rx_counters.send_errors;
        return;
    }
    ++stored_rx_counters.accepted_packets;
    stored_rx_counters.accepted_bytes += len;
}
}  // namespace winject
