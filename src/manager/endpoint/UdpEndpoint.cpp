#include "endpoint/UdpEndpoint.h"

#include "utils/Log.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <vector>

namespace
{
constexpr size_t k_max_udp_queue = 1024;

bfc::sized_buffer make_pkt(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return {};
    }
    bfc::sized_buffer pkt(len);
    memcpy(pkt.data(), data, len);
    return pkt;
}

bfc::sized_buffer make_pkt(std::vector<uint8_t>&& payload)
{
    if (payload.empty())
    {
        return {};
    }
    return make_pkt(payload.data(), payload.size());
}
}  // namespace

namespace winject
{

UdpEndpoint::~UdpEndpoint()
{
    close();
}

bool UdpEndpoint::open(IOReactor& reactor, const UpstreamConfig& cfg,
                       std::function<void()> tx_wake)
{
    this->reactor = &reactor;
    tx_wake_ = std::move(tx_wake);
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

    if (cfg.fec_type == FecType::RsBlockErasure)
    {
        fec_timeout_ms_ = cfg.fec_timeout_ms;
        if (!fec.init(cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms))
        {
            LOG_ERR("udp fec init k=%d n=%d failed", cfg.fec_k, cfg.fec_n);
            return false;
        }
        LOG_INF("udp fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)",
                cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms, fec.impl_name());
    }

    return reactor.add_read_rdy(sock.fd(),
                                [this]()
                                {
                                    on_app();
                                });
}

void UdpEndpoint::close()
{
    cancel_fec_timer();
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        bfc::socket(std::move(sock));
    }
}

void UdpEndpoint::push_tx(bfc::sized_buffer pkt, bool is_fec)
{
    if (pkt.empty())
    {
        return;
    }
    if (txq.size() >= k_max_udp_queue)
    {
        txq.pop_front();
    }
    TxItem item;
    item.pkt = std::move(pkt);
    item.is_fec = is_fec;
    txq.push_back(std::move(item));
}

void UdpEndpoint::enqueue_air(bfc::sized_buffer pkt)
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    push_tx(std::move(pkt), false);
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
                // Reply dest is a closed local port; ICMP is queued on this
                // socket. Drop it and accept the next sender (nc -w1, or a
                // new interactive nc after the previous one quit).
                dest_valid = false;
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
        app_tx_bytes_.fetch_add(static_cast<uint64_t>(n),
                                std::memory_order_relaxed);
        app_tx_packets_.fetch_add(1, std::memory_order_relaxed);
        if (mode == UpstreamMode::udp_server)
        {
            // Last sender gets replies. Camera/master also seed with "ok\n"
            // at connect; that is just another datagram. A stray nc to the
            // camera bind steals until camera sends again — don't nc :21092
            // while camera is running.
            dest = from;
            dest_valid = true;
        }
        if (fec.enabled())
        {
            bool wake_tx = false;
            {
                std::lock_guard<std::mutex> lock(tx_mu_);
                fec.push_app(reinterpret_cast<const uint8_t*>(rx_buf.data()),
                             rx_buf.size(), nullptr);
                sync_fec_timer_after_push();
                wake_tx = fec.has_tx_shards();
            }
            if (wake_tx && tx_wake_)
            {
                tx_wake_();
            }
            continue;
        }
        if (static_cast<size_t>(n) > k_stream_payload_max)
        {
            const uint64_t drops =
                app_rx_oversize_pkt_.fetch_add(1, std::memory_order_relaxed) +
                1;
            if (drops == 1 || (drops & 1023u) == 0)
            {
                LOG_WRN("drop oversized udp %zd (drops=%llu)", n,
                        static_cast<unsigned long long>(drops));
            }
            continue;
        }
        enqueue_air(make_pkt(reinterpret_cast<const uint8_t*>(rx_buf.data()),
                             rx_buf.size()));
    }
}

void UdpEndpoint::send_app(const uint8_t* data, size_t len)
{
    app_rx_bytes_.fetch_add(static_cast<uint64_t>(len),
                            std::memory_order_relaxed);
    app_rx_packets_.fetch_add(1, std::memory_order_relaxed);
    const bfc::const_buffer_view view(reinterpret_cast<const std::byte*>(data),
                                      len);
    sock.send(view, 0, reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

void UdpEndpoint::on_radio_rx(bfcext::shared_sized_buffer pkt, bool is_fec)
{
    if (pkt.empty())
    {
        return;
    }
    if (sock.fd() < 0 || !dest_valid)
    {
        return;
    }
    const uint8_t* air = reinterpret_cast<const uint8_t*>(pkt.data());
    const size_t air_len = pkt.size();
    // The LC header says whether this is a shard; the SDU is never inspected.
    if (!is_fec)
    {
        send_app(air, air_len);
        return;
    }
    fec_air_rx_bytes_.fetch_add(static_cast<uint64_t>(air_len),
                                std::memory_order_relaxed);
    fec_air_rx_packets_.fetch_add(1, std::memory_order_relaxed);
    if (!fec.enabled())
    {
        fec_air_rx_unexpected_.fetch_add(1, std::memory_order_relaxed);
    }
    // k/n come from the shard header, so shards decode even with FEC disabled.
    std::vector<std::vector<uint8_t>> payloads;
    fec.push_air(air, air_len, &payloads);
    for (const auto& p : payloads)
    {
        send_app(p.data(), p.size());
    }
}

void UdpEndpoint::arm_fec_timer()
{
    if (reactor == nullptr || !fec.enabled() || fec_timer_armed_ ||
        fec_timeout_ms_ <= 0 || !fec.has_pending())
    {
        return;
    }
    fec_timer_id_ = reactor->get_timer().wait_ms(fec_timeout_ms_,
                                                 [this]()
                                                 {
                                                     on_fec_timer();
                                                 });
    fec_timer_armed_ = true;
}

void UdpEndpoint::cancel_fec_timer()
{
    if (!fec_timer_armed_ || reactor == nullptr)
    {
        return;
    }
    reactor->get_timer().cancel(fec_timer_id_);
    fec_timer_armed_ = false;
}

void UdpEndpoint::on_fec_timer()
{
    fec_timer_armed_ = false;
    bool wake_tx = false;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        fec.flush_if_deadline();
        sync_fec_timer_after_push();
        wake_tx = fec.has_tx_shards();
    }
    if (wake_tx && tx_wake_)
    {
        tx_wake_();
    }
}

void UdpEndpoint::sync_fec_timer_after_push()
{
    if (!fec.has_pending())
    {
        cancel_fec_timer();
        return;
    }
    arm_fec_timer();
}

void UdpEndpoint::stage_fec_shard_for_tx(size_t max)
{
    if (!fec.enabled() || !txq.empty())
    {
        return;
    }
    std::vector<uint8_t> shard;
    if (!fec.pop_tx_shard(max, &shard))
    {
        return;
    }
    push_tx(make_pkt(std::move(shard)), true);
}

void UdpEndpoint::announce_down()
{
    if (!fec.enabled())
    {
        return;
    }
    std::vector<std::vector<uint8_t>> encoded;
    std::lock_guard<std::mutex> lock(tx_mu_);
    cancel_fec_timer();
    fec.flush(&encoded);
    fec.drain_tx_shards(&encoded);
    for (auto& pkt : encoded)
    {
        push_tx(make_pkt(std::move(pkt)), true);
    }
}

bool UdpEndpoint::set_fec(FecType type, int k, int n, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };

    announce_down();
    std::lock_guard<std::mutex> lock(tx_mu_);
    if (type == FecType::none)
    {
        fec.disable();
        LOG_INF("udp fec disabled");
        return true;
    }
    if (type != FecType::RsBlockErasure)
    {
        return fail("unsupported fec type");
    }
    const int timeout_ms = fec_timeout_ms_ > 0
                               ? fec_timeout_ms_
                               : RsBlockErasure::k_default_timeout_ms;
    if (!fec.init(k, n, timeout_ms))
    {
        return fail("invalid fec k/n (need 1 <= k < n <= 31)");
    }

    LOG_INF("udp fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)", k, n,
            timeout_ms, fec.impl_name());
    return true;
}

bool UdpEndpoint::set_fec_timeout_ms(int timeout_ms, std::string* error)
{
    if (timeout_ms < 0)
    {
        if (error != nullptr)
        {
            *error = "EINVAL";
        }
        return false;
    }
    fec_timeout_ms_ = timeout_ms;
    return true;
}

uint64_t UdpEndpoint::fec_recovered() const
{
    return fec.recovered();
}

uint64_t UdpEndpoint::fec_decode_fail() const
{
    return fec.decode_fail();
}

void UdpEndpoint::tx_pending_stats(uint64_t* pkt, uint64_t* byt) const
{
    uint64_t p = 0;
    uint64_t b = 0;
    std::lock_guard<std::mutex> lock(tx_mu_);
    for (const auto& q : txq)
    {
        p++;
        b += static_cast<uint64_t>(q.pkt.size());
    }
    if (fec.enabled())
    {
        uint64_t fp = 0;
        uint64_t fb = 0;
        fec.tx_pending_stats(&fp, &fb);
        p += fp;
        b += fb;
    }
    if (pkt != nullptr)
    {
        *pkt = p;
    }
    if (byt != nullptr)
    {
        *byt = b;
    }
}

void UdpEndpoint::get_fec(FecType* type, int* k, int* n) const
{
    if (type != nullptr)
    {
        *type = fec.enabled() ? FecType::RsBlockErasure : FecType::none;
    }
    if (k != nullptr)
    {
        *k = fec.enabled() ? fec.k() : 0;
    }
    if (n != nullptr)
    {
        *n = fec.enabled() ? fec.n() : 0;
    }
}

bool UdpEndpoint::has_tx()
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    if (!txq.empty())
    {
        return true;
    }
    return fec.enabled() && fec.has_tx_shards();
}

size_t UdpEndpoint::get_tx_size()
{
    std::lock_guard<std::mutex> lock(tx_mu_);
    if (!txq.empty())
    {
        return txq.front().pkt.size();
    }
    if (fec.enabled())
    {
        return fec.first_tx_shard_size();
    }
    return 0;
}

bfc::sized_buffer UdpEndpoint::pull_tx(size_t max, bool* is_fec)
{
    if (is_fec != nullptr)
    {
        *is_fec = false;
    }
    std::lock_guard<std::mutex> lock(tx_mu_);
    stage_fec_shard_for_tx(max);
    if (txq.empty() || max == 0)
    {
        return {};
    }
    TxItem& item = txq.front();
    if (item.pkt.size() > max)
    {
        return {};
    }
    bfc::sized_buffer out = std::move(item.pkt);
    const bool shard = item.is_fec;
    txq.pop_front();
    if (shard)
    {
        fec_air_tx_bytes_.fetch_add(static_cast<uint64_t>(out.size()),
                                    std::memory_order_relaxed);
        fec_air_tx_packets_.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_fec != nullptr)
    {
        *is_fec = shard;
    }
    return out;
}

}  // namespace winject
