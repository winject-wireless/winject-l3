#include "endpoint/UDPUpstream.h"

#include "utils/NetUtil.h"

#include <errno.h>
#include <stdexcept>
#include <sys/socket.h>
namespace winject
{
UDPUpstream::UDPUpstream(std::shared_ptr<Reactor> reactor,
                         std::shared_ptr<IOReactor> io,
                         const UpstreamConfig& config,
                         std::shared_ptr<BoundedLossyQueue> radio_tx_queue)
    : Upstream(std::move(reactor), config, std::move(radio_tx_queue)),
      io_(std::move(io)),
      socket_(bfc::create_udp4())
{
    if (!io_ || config.mode == UpstreamMode::queue || !config.reactor.empty() ||
        (config.endpoint.rx.empty() && config.endpoint.tx.empty()) ||
        socket_.fd() < 0)
    {
        throw std::invalid_argument("invalid UDP upstream configuration");
    }
    app_tx_queue_->set_capacity(config.rx_queue_capacity);
    config_.mode = upstream_mode(config.endpoint);
    const int one = 1;
    socket_.set_sock_opt(SOL_SOCKET, SO_REUSEADDR, one);
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
        if (!parse_host_port(config.endpoint.tx, &destination_))
        {
            throw std::invalid_argument("invalid UDP destination");
        }
        have_destination_ = true;
    }
    if (socket_.bind(bind) != 0)
    {
        throw std::runtime_error("UDP upstream bind failed");
    }
}
void UDPUpstream::bind_callbacks()
{
    if (registered_)
    {
        return;
    }
    Upstream::bind_callbacks();
    const auto weak = weak_from_this();
    registered_ = io_->add_read_rdy(
        socket_.fd(),
        [weak]
        {
            if (auto self = weak.lock())
            {
                static_cast<UDPUpstream&>(*self).on_socket_readable();
            }
        });
    if (!registered_)
    {
        throw std::runtime_error("UDP upstream reactor registration failed");
    }
}
UDPUpstream::~UDPUpstream()
{
    unbind_callbacks();
    if (registered_)
    {
        io_->rem_read_rdy(socket_.fd());
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
            std::lock_guard<std::mutex> lock(socket_mu_);
            received =
                socket_.recv(buffer, MSG_DONTWAIT | MSG_TRUNC,
                             reinterpret_cast<sockaddr*>(&from), &from_size);
            if (received > 0 && config_.mode == UpstreamMode::udp_server)
            {
                destination_ = from;
                have_destination_ = true;
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
                app_tx_queue_->push(
                    bfc::shared_sized_buffer::copy_from(buffer.data(), size));
            }
        }
    }
}
void UDPUpstream::deliver_datagram(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> counters(rx_counters_mu_);
    ++rx_counters_.delivery_attempts;
    std::lock_guard<std::mutex> socket(socket_mu_);
    if (!have_destination_)
    {
        ++rx_counters_.no_peer_drops;
        return;
    }
    const bfc::const_buffer_view view(reinterpret_cast<const std::byte*>(data),
                                      len);
    const auto sent = socket_.send(
        view, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&destination_),
        sizeof(destination_));
    if (sent < 0 || static_cast<size_t>(sent) != len)
    {
        ++rx_counters_.send_errors;
        return;
    }
    ++rx_counters_.accepted_packets;
    rx_counters_.accepted_bytes += len;
}
}  // namespace winject
