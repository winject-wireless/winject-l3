#include "endpoint/HostEndpoint.h"

#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/socket.hpp>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unordered_map>
#include <utility>

namespace winject
{

// Each callback uses a weak session reference, not a pointer to HostEndpoint.
// A callback already copied by a reactor can safely outlive close or reopen.
struct HostEndpoint::Session : std::enable_shared_from_this<Session>
{
    using Queue = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
    using Registry =
        bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;
    enum class Delivery
    {
        accepted,
        no_peer,
        closed,
        error
    };

    DatagramAir air;
    UpstreamConfig config;
    IOReactor* io = nullptr;
    // close fences ingress before stopping air. Delivery takes only
    // transport_mu, so a timer callback never waits on ingress while close
    // waits for that timer.
    std::recursive_mutex ingress_mu;
    mutable std::mutex transport_mu;
    bool opened = false;
    bool accepting_input = false;
    bool registered = false;
    bool claimed = false;
    static std::mutex claims_mu;
    static std::unordered_map<Queue*, std::weak_ptr<Session>> claims;
    bfc::socket sock;
    sockaddr_in dest{};
    bool dest_valid = false;
    bfc::sized_buffer rx_buf;
    std::shared_ptr<Queue> queue_in, queue_out;
    std::shared_ptr<bfc::cv_reactor<>> cv;
    TransportCounters counters;

    bool queue_mode() const
    {
        return config.mode == UpstreamMode::queue;
    }

    bool is_open() const
    {
        std::lock_guard<std::mutex> lock(transport_mu);
        return opened;
    }

    bool prepare_udp()
    {
        config.mode = upstream_mode(config.endpoint);
        if (config.endpoint.rx.empty() && config.endpoint.tx.empty())
        {
            return false;
        }
        sock = bfc::socket(bfc::create_udp4());
        if (sock.fd() < 0)
        {
            return false;
        }
        const int one = 1;
        sock.set_sock_opt(SOL_SOCKET, SO_REUSEADDR, one);
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (!config.endpoint.rx.empty() &&
            !parse_host_port(config.endpoint.rx, &bind_addr))
        {
            return false;
        }
        // New session: a UDP server starts without a learned destination.
        dest = {};
        dest_valid = false;
        if (!config.endpoint.tx.empty())
        {
            if (!parse_host_port(config.endpoint.tx, &dest))
            {
                return false;
            }
            dest_valid = true;
        }
        return sock.bind(bind_addr) == 0;
    }

    bool prepare_queue()
    {
        if (config.endpoint.rx.empty() || config.endpoint.tx.empty() ||
            config.endpoint.rx == config.endpoint.tx || config.reactor.empty())
        {
            return false;
        }
        queue_in = Registry::get_instance().get(config.endpoint.rx);
        queue_out = Registry::get_instance().get(config.endpoint.tx);
        cv = bfc::cv_reactor_registry::get_instance().get(config.reactor);
        if (!queue_in || !queue_out || queue_in == queue_out || !cv ||
            queue_in->capacity() == 0 || queue_out->capacity() == 0)
        {
            return false;
        }
        return true;
    }

    bool open(IOReactor& reactor, const UpstreamConfig& cfg,
              std::function<void()> tx_wake)
    {
        config = cfg;
        io = &reactor;
        if (!queue_mode() && !config.reactor.empty())
        {
            return false;
        }
        if (queue_mode() ? !prepare_queue() : !prepare_udp())
        {
            LOG_ERR("upstream %s transport open failed",
                    queue_mode() ? "queue" : "udp");
            return false;
        }
        const std::weak_ptr<Session> weak = shared_from_this();
        if (queue_mode())
        {
            std::lock_guard<std::mutex> lock(claims_mu);
            auto& claim = claims[queue_in.get()];
            if (!claim.expired())
            {
                LOG_ERR("upstream queue already has a consumer");
                return false;
            }
            claim = weak;
            claimed = true;
        }
        if (!air.start(
                std::move(tx_wake),
                [weak](const uint8_t* data, size_t len)
                {
                    if (auto self = weak.lock())
                    {
                        self->deliver(data, len);
                    }
                },
                &reactor.get_timer(),
                // Queue ingress can arm a timer while the IO reactor is
                // waiting.
                [&reactor]()
                {
                    reactor.wake_up([]() {});
                },
                cfg, queue_mode() ? "queue" : "udp"))
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(transport_mu);
            opened = true;
            accepting_input = true;
        }
        if (queue_mode())
        {
            registered = cv->add_read_rdy(*queue_in,
                                          [weak]()
                                          {
                                              if (auto self = weak.lock())
                                              {
                                                  self->on_queue();
                                              }
                                          });
            if (registered)
            {
                cv->wake_up();
            }
        }
        else
        {
            registered = reactor.add_read_rdy(sock.fd(),
                                              [weak]()
                                              {
                                                  if (auto self = weak.lock())
                                                  {
                                                      self->on_udp();
                                                  }
                                              });
        }
        return registered;
    }

    void stop_input()
    {
        std::lock_guard<std::recursive_mutex> ingress(ingress_mu);
        {
            std::lock_guard<std::mutex> lock(transport_mu);
            accepting_input = false;
        }
        if (registered)
        {
            registered = false;
            if (queue_mode())
            {
                cv->remove_read_rdy(*queue_in);
                // BFC snapshots raw context pointers for a scan. Keep the queue
                // alive until that scan finishes, even if its registry entry is
                // removed immediately after close. No acknowledgement wait is
                // needed: weak callbacks and the ingress fence protect Session.
                cv->wake_up([lease = queue_in]() {});
            }
            else
            {
                io->rem_read_rdy(sock.fd());
            }
        }
    }

    void close()
    {
        std::lock_guard<std::recursive_mutex> ingress(ingress_mu);
        stop_input();
        {
            std::lock_guard<std::mutex> lock(transport_mu);
            opened = false;
        }
        if (claimed)
        {
            std::lock_guard<std::mutex> lock(claims_mu);
            claims.erase(queue_in.get());
            claimed = false;
        }
        air.stop();
        std::lock_guard<std::mutex> lock(transport_mu);
        bfc::socket(std::move(sock));
        dest = {};
        dest_valid = false;
        queue_in.reset();
        queue_out.reset();
        cv.reset();
    }

    Delivery deliver(const uint8_t* data, size_t len)
    {
        std::lock_guard<std::mutex> lock(transport_mu);
        ++counters.delivery_attempts;
        if (!opened)
        {
            ++counters.closed_drops;
            return Delivery::closed;
        }
        if (queue_mode())
        {
            const size_t capacity = queue_out->capacity();
            if (capacity == 0)
            {
                ++counters.send_errors;
                return Delivery::error;
            }
            const auto before =
                queue_out->push(bfc::shared_sized_buffer::copy_from(data, len));
            if (before >= capacity)
            {
                ++counters.queue_evictions;
            }
            cv->wake_up();
        }
        else
        {
            if (!dest_valid)
            {
                ++counters.no_peer_drops;
                return Delivery::no_peer;
            }
            const bfc::const_buffer_view view(
                reinterpret_cast<const std::byte*>(data), len);
            const auto sent = sock.send(
                view, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&dest),
                sizeof(dest));
            if (sent < 0 || static_cast<size_t>(sent) != len)
            {
                ++counters.send_errors;
                return Delivery::error;
            }
        }
        ++counters.accepted_packets;
        counters.accepted_bytes += len;
        return Delivery::accepted;
    }

    void on_air(bfc::shared_sized_buffer pkt, bool fec)
    {
        std::lock_guard<std::recursive_mutex> ingress(ingress_mu);
        if (!is_open())
        {
            return;
        }
        // Decode independently of peer readiness. A missing UDP server peer is
        // counted at delivery instead of silently bypassing FEC and its
        // metrics.
        air.on_radio_rx(std::move(pkt), fec);
    }

    void on_queue()
    {
        std::lock_guard<std::recursive_mutex> ingress(ingress_mu);
        for (int n = 0; n < 32; ++n)
        {
            std::shared_ptr<Queue> in;
            {
                std::lock_guard<std::mutex> lock(transport_mu);
                if (!opened || !accepting_input)
                {
                    return;
                }
                in = queue_in;
            }
            auto pkt = in->pop();
            if (!pkt)
            {
                return;
            }
            if (!pkt->empty())
            {
                air.accept_app_datagram(pkt->u8(), pkt->size());
            }
        }
        std::lock_guard<std::mutex> lock(transport_mu);
        if (opened && accepting_input && queue_in->has_data())
        {
            cv->wake_up();
        }
    }

    void on_udp()
    {
        std::lock_guard<std::recursive_mutex> ingress(ingress_mu);
        for (int n = 0; n < 8; ++n)
        {
            ssize_t received;
            {
                std::lock_guard<std::mutex> lock(transport_mu);
                if (!opened || !accepting_input)
                {
                    return;
                }
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                rx_buf.resize(2048);
                received =
                    sock.recv(rx_buf, MSG_DONTWAIT,
                              reinterpret_cast<sockaddr*>(&from), &from_len);
                if (received >= 0 && config.mode == UpstreamMode::udp_server &&
                    received > 0)
                {
                    dest = from;
                    dest_valid = true;
                }
            }
            if (received < 0)
            {
                if (errno == EINTR || errno == ECONNREFUSED)
                {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    LOG_ERR("upstream UDP recv: %s", strerror(errno));
                }
                return;
            }
            if (received > 0)
            {
                air.accept_app_datagram(
                    reinterpret_cast<const uint8_t*>(rx_buf.data()),
                    static_cast<size_t>(received));
            }
        }
    }
};

std::mutex HostEndpoint::Session::claims_mu;
std::unordered_map<HostEndpoint::Session::Queue*,
                   std::weak_ptr<HostEndpoint::Session>>
    HostEndpoint::Session::claims;

HostEndpoint::~HostEndpoint()
{
    close();
}

std::shared_ptr<HostEndpoint::Session> HostEndpoint::session() const
{
    std::lock_guard<std::mutex> lock(session_mu_);
    return session_;
}

bool HostEndpoint::open(IOReactor& reactor, const UpstreamConfig& cfg,
                        std::function<void()> tx_wake)
{
    if (is_open())
    {
        return false;
    }
    auto next = std::make_shared<Session>();
    if (!next->open(reactor, cfg, std::move(tx_wake)))
    {
        next->close();
        return false;
    }
    std::lock_guard<std::mutex> lock(session_mu_);
    session_ = std::move(next);
    stats_ = {};
    return true;
}

bool HostEndpoint::is_open() const
{
    auto s = session();
    return s && s->is_open();
}

void HostEndpoint::stop_input()
{
    if (auto s = session())
    {
        s->stop_input();
    }
}

void HostEndpoint::close()
{
    if (auto s = session())
    {
        s->close();
    }
}

const char* HostEndpoint::type_name() const
{
    auto s = session();
    return s && s->queue_mode() ? "QUEUE" : "UDP";
}

std::string HostEndpoint::endpoint_rx() const
{
    auto s = session();
    return s ? s->config.endpoint.rx : std::string{};
}

std::string HostEndpoint::endpoint_tx() const
{
    auto s = session();
    return s ? s->config.endpoint.tx : std::string{};
}

HostEndpoint::TransportCounters HostEndpoint::transport_counters() const
{
    auto s = session();
    if (!s)
    {
        return {};
    }
    std::lock_guard<std::mutex> lock(s->transport_mu);
    return s->counters;
}

void HostEndpoint::on_radio_rx(bfc::shared_sized_buffer pkt, bool is_fec)
{
    if (auto s = session())
    {
        s->on_air(std::move(pkt), is_fec);
    }
}

bool HostEndpoint::has_tx()
{
    auto s = session();
    return s && s->air.has_tx();
}

size_t HostEndpoint::get_tx_size()
{
    auto s = session();
    return s ? s->air.get_tx_size() : 0;
}

bfc::sized_buffer HostEndpoint::pull_tx(size_t max, bool* is_fec)
{
    auto s = session();
    if (s)
    {
        return s->air.pull_tx(max, is_fec);
    }
    if (is_fec)
    {
        *is_fec = false;
    }
    return {};
}

void HostEndpoint::announce_down()
{
    if (auto s = session())
    {
        s->air.announce_down();
    }
}

bool HostEndpoint::set_fec(FecType type, int k, int n, std::string* error)
{
    auto s = session();
    if (s && s->is_open())
    {
        return s->air.set_fec(type, k, n, error);
    }
    if (error)
    {
        *error = "endpoint closed";
    }
    return false;
}

void HostEndpoint::get_fec(FecType* type, int* k, int* n) const
{
    if (auto s = session())
    {
        s->air.get_fec(type, k, n);
        return;
    }
    if (type)
    {
        *type = FecType::none;
    }
    if (k)
    {
        *k = 0;
    }
    if (n)
    {
        *n = 0;
    }
}

bool HostEndpoint::set_fec_timeout_ms(int timeout_ms, std::string* error)
{
    auto s = session();
    if (s && s->is_open())
    {
        return s->air.set_fec_timeout_ms(timeout_ms, error);
    }
    if (error)
    {
        *error = "endpoint closed";
    }
    return false;
}

int HostEndpoint::fec_timeout_ms() const
{
    auto s = session();
    return s ? s->air.fec_timeout_ms() : RsBlockErasure::k_default_timeout_ms;
}

void HostEndpoint::tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
{
    if (auto s = session())
    {
        s->air.tx_pending_stats(pkt, byt);
        return;
    }
    if (pkt)
    {
        *pkt = 0;
    }
    if (byt)
    {
        *byt = 0;
    }
}

uint64_t HostEndpoint::app_rx_bytes() const
{
    auto s = session();
    return s ? s->air.app_rx_bytes() : 0;
}

uint64_t HostEndpoint::app_rx_packets() const
{
    auto s = session();
    return s ? s->air.app_rx_packets() : 0;
}

uint64_t HostEndpoint::app_rx_oversize_pkt() const
{
    auto s = session();
    return s ? s->air.app_rx_oversize_pkt() : 0;
}

uint64_t HostEndpoint::app_tx_bytes() const
{
    auto s = session();
    return s ? s->air.app_tx_bytes() : 0;
}

uint64_t HostEndpoint::app_tx_packets() const
{
    auto s = session();
    return s ? s->air.app_tx_packets() : 0;
}

uint64_t HostEndpoint::fec_air_rx_bytes() const
{
    auto s = session();
    return s ? s->air.fec_air_rx_bytes() : 0;
}

uint64_t HostEndpoint::fec_air_rx_packets() const
{
    auto s = session();
    return s ? s->air.fec_air_rx_packets() : 0;
}

uint64_t HostEndpoint::fec_air_rx_unexpected() const
{
    auto s = session();
    return s ? s->air.fec_air_rx_unexpected() : 0;
}

uint64_t HostEndpoint::fec_air_tx_bytes() const
{
    auto s = session();
    return s ? s->air.fec_air_tx_bytes() : 0;
}

uint64_t HostEndpoint::fec_air_tx_packets() const
{
    auto s = session();
    return s ? s->air.fec_air_tx_packets() : 0;
}

uint64_t HostEndpoint::fec_recovered() const
{
    auto s = session();
    return s ? s->air.fec_recovered() : 0;
}

uint64_t HostEndpoint::fec_decode_fail() const
{
    auto s = session();
    return s ? s->air.fec_decode_fail() : 0;
}

uint64_t HostEndpoint::tx_queue_drops() const
{
    auto s = session();
    return s ? s->air.tx_queue_drops() : 0;
}

}  // namespace winject
