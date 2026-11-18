#include "endpoint/DatagramAir.h"

#include "utils/NetUtil.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace winject::core
{

using winject::fec::fec_read_stream;
using winject::fec::fec_write_stream;
using winject::fec::out_shard;
using winject::fec::shard_type;

DatagramAir::~DatagramAir()
{
    stop();
}

bool DatagramAir::fec_write_config(const UpstreamConfig& cfg,
                                   fec_write_stream::config* out)
{
    fec_write_stream::config w;
    // Checked with FEC off too: a later FEC change would pick it up.
    if (cfg.fec_spread_ms < 0 || cfg.fec_spread_ms > winject::fec::k_spread_cap_ms)
    {
        return false;
    }
    if (cfg.fec_type == FecType::RsBlockErasure)
    {
        // l3 keeps its rules: at least one parity shard, n within one header
        // field (k_header_k_n_max, 31).
        if (cfg.fec_k < 1 || cfg.fec_n <= cfg.fec_k ||
            cfg.fec_n > winject::fec::k_header_k_n_max)
        {
            return false;
        }
        w.k = cfg.fec_k;
        w.n = cfg.fec_n;
        w.max_datagram = k_stream_payload_max;
        w.spread_ms = cfg.fec_spread_ms;
        w.flush_ms = cfg.fec_timeout_ms;
    }
    else if (cfg.fec_type != FecType::none)
    {
        return false;
    }
    if (!fec_write_stream::valid(w))
    {
        return false;
    }
    if (out != nullptr)
    {
        *out = w;
    }
    return true;
}

bool DatagramAir::start(std::shared_ptr<BoundedLossyQueue> tx_queue,
                        DeliverApp deliver_app,
                        std::shared_ptr<Reactor> reactor,
                        const UpstreamConfig& cfg, uint8_t route_bus,
                        UpstreamStats* tx_stats, std::mutex* tx_stats_mu,
                        DeliverOwnedApp deliver_owned_app)
{
    std::lock_guard<std::mutex> lock(mu);
    stop_locked();
    fec_write_stream::config wcfg;
    if (!reactor || !tx_queue || tx_queue->capacity() == 0 || route_bus == 0 ||
        tx_stats == nullptr || tx_stats_mu == nullptr ||
        !fec_write_config(cfg, &wcfg))
    {
        return false;
    }
    auto ws = std::make_unique<fec_write_stream>();
    // The LC prefix goes into the room the stream leaves in front of every
    // shard.
    ws->set_tx_buffers(k_slot_prefix_len, nullptr);
    ws->set_input_capacity(tx_queue->capacity());
    ws->set_output_capacity(tx_queue->capacity());
    if (!ws->configure(wcfg))
    {
        return false;
    }
    ws->attach(reactor);
    auto rs = std::make_unique<fec_read_stream>();
    const size_t rx_capacity =
        cfg.rx_queue_capacity ? cfg.rx_queue_capacity : tx_queue->capacity();
    rs->set_input_capacity(rx_capacity);
    // A rebuilt or given-up hole releases everything held behind it at once:
    // up to the read stream's whole window, before drain_rx runs.
    rs->set_output_capacity(std::max(rx_capacity, fec_read_stream::k_rx_ring));
    rs->attach(reactor);

    fec_tx_on = wcfg.k > 0;
    max_app_datagram = fec_tx_on ? fec_write_stream::max_original(wcfg)
                                 : k_stream_payload_max;
    tx_stream = std::move(ws);
    rx_stream = std::move(rs);
    this->reactor = std::move(reactor);
    this->tx_queue = std::move(tx_queue);
    this->route_bus = route_bus;
    this->tx_stats = tx_stats;
    this->tx_stats_mu = tx_stats_mu;
    this->deliver_app = std::move(deliver_app);
    this->deliver_owned_app = std::move(deliver_owned_app);
    return true;
}

bool DatagramAir::reconfigure_fec(const UpstreamConfig& cfg)
{
    std::lock_guard<std::mutex> lock(mu);
    fec_write_stream::config wcfg;
    if (!tx_stream || !fec_write_config(cfg, &wcfg) ||
        !tx_stream->configure(wcfg))
    {
        return false;
    }
    // A block the old settings opened finishes under them; the write stream
    // drops a datagram too long for it, counted in app_rx_oversize_pkt.
    fec_tx_on = wcfg.k > 0;
    max_app_datagram = fec_tx_on ? fec_write_stream::max_original(wcfg)
                                 : k_stream_payload_max;
    return true;
}

void DatagramAir::stop()
{
    std::lock_guard<std::mutex> lock(mu);
    stop_locked();
}

void DatagramAir::stop_locked()
{
    if (outputs_bound && reactor)
    {
        auto tx_out = tx_stream->output();
        auto rx_out = rx_stream->output();
        reactor->remove_read_rdy(*tx_out);
        reactor->remove_read_rdy(*rx_out);
        tx_out->set_reactor(nullptr);
        rx_out->set_reactor(nullptr);
        // The reactor walks its contexts unlocked: keep both rings alive
        // past a walk in flight.
        reactor->wake_up([tx_out, rx_out] {});
    }
    outputs_bound = false;
    if (tx_stream)
    {
        sync_fec_counters();
    }
    // Pending parity and held RX data go with the streams.
    tx_stream.reset();
    rx_stream.reset();
    reactor.reset();
    tx_queue.reset();
    route_bus = 0;
    tx_stats = nullptr;
    tx_stats_mu = nullptr;
    deliver_app = nullptr;
    deliver_owned_app = nullptr;
    fec_tx_on = false;
    max_app_datagram = 0;
}

void DatagramAir::set_lifetime(std::weak_ptr<void> owner)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!tx_stream || outputs_bound)
    {
        return;
    }
    auto tx_out = tx_stream->output();
    auto rx_out = rx_stream->output();
    tx_out->set_reactor(reactor);
    rx_out->set_reactor(reactor);
    reactor->add_read_rdy(*tx_out,
                          [owner, this]
                          {
                              if (auto alive = owner.lock())
                              {
                                  drain_tx();
                              }
                          });
    reactor->add_read_rdy(*rx_out,
                          [owner, this]
                          {
                              if (auto alive = owner.lock())
                              {
                                  drain_rx();
                              }
                          });
    outputs_bound = true;
}

void DatagramAir::accept_app_datagram(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lock(mu);
    if (data == nullptr || len == 0 || !tx_stream)
    {
        return;
    }
    app_tx_bytes_acc.fetch_add(len, std::memory_order_relaxed);
    app_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
    if (len > max_app_datagram)
    {
        app_rx_oversize_pkt_acc.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The one copy on the way out: the stream and the LC stamp then write
    // their headers into the room in front.
    const size_t offset = tx_stream->push_offset();
    auto buf = bfc::shared_sized_buffer::allocate(offset + len);
    buf.resize(offset + len);
    memcpy(buf.u8() + offset, data, len);
    tx_stream->push(std::move(buf));
    drain_tx_locked();
    fec_tx_size_drops_acc.store(tx_stream->get_counters().drop_size,
                                std::memory_order_relaxed);
}

void DatagramAir::drain_tx()
{
    std::lock_guard<std::mutex> lock(mu);
    if (tx_stream)
    {
        drain_tx_locked();
    }
}

void DatagramAir::drain_tx_locked()
{
    const auto capacity = tx_queue->capacity();
    for (out_shard s = tx_stream->pop(); s.type != shard_type::none;
         s = tx_stream->pop())
    {
        if (s.buf.size() <= k_slot_prefix_len)
        {
            continue;
        }
        const bool fec = s.type != shard_type::plain;
        {
            std::lock_guard<std::mutex> lock(*tx_stats_mu);
            if (!stamp_slot_prefix(tx_stats, route_bus,
                                   fec ? LcType::fec : LcType::plain,
                                   s.buf.u8()))
            {
                continue;
            }
        }
        const size_t body = s.buf.size() - k_slot_prefix_len;
        const auto before = tx_queue->push(std::move(s.buf));
        if (capacity == 0 || before >= capacity)
        {
            tx_queue_drops_acc.fetch_add(1, std::memory_order_relaxed);
        }
        if (fec)
        {
            fec_air_tx_bytes_acc.fetch_add(body, std::memory_order_relaxed);
            fec_air_tx_packets_acc.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void DatagramAir::on_radio_rx(bfc::shared_sized_buffer body, LcType type)
{
    std::lock_guard<std::mutex> lock(mu);
    if (body.empty() || body.size() > k_stream_payload_max || !rx_stream)
    {
        return;
    }
    air_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
    air_rx_bytes_acc.fetch_add(body.size(), std::memory_order_relaxed);
    if (type == LcType::fec)
    {
        fec_air_rx_bytes_acc.fetch_add(body.size(), std::memory_order_relaxed);
        fec_air_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
        if (!fec_tx_on)
        {
            fec_air_rx_unexpected_acc.fetch_add(1, std::memory_order_relaxed);
        }
    }
    rx_stream->push(std::move(body), type);
    drain_rx_locked();
}

void DatagramAir::drain_rx()
{
    std::lock_guard<std::mutex> lock(mu);
    if (rx_stream)
    {
        drain_rx_locked();
    }
}

void DatagramAir::drain_rx_locked()
{
    for (auto b = rx_stream->pop(); !b.empty(); b = rx_stream->pop())
    {
        deliver(std::move(b));
    }
    sync_fec_counters();
    sync_fec_estimates();
}

void DatagramAir::deliver(bfc::shared_sized_buffer datagram)
{
    if (datagram.empty())
    {
        return;
    }
    app_rx_bytes_acc.fetch_add(datagram.size(), std::memory_order_relaxed);
    app_rx_packets_acc.fetch_add(1, std::memory_order_relaxed);
    if (deliver_owned_app)
    {
        // The radio buffer goes on without a copy.
        deliver_owned_app(std::move(datagram));
    }
    else if (deliver_app)
    {
        deliver_app(datagram.u8(), datagram.size());
    }
}

void DatagramAir::sync_fec_counters()
{
    if (!rx_stream)
    {
        return;
    }
    const auto c = rx_stream->get_counters();
    fec_recovered_acc.store(c.recovered, std::memory_order_relaxed);
    fec_decode_fail_acc.store(c.decode_fail + c.hdr_errors + c.kn_mismatch,
                              std::memory_order_relaxed);
    fec_rx_drops_acc.store(c.drop_output + c.drop_input,
                           std::memory_order_relaxed);
    fec_missing_data_acc.store(c.missing_data, std::memory_order_relaxed);
    fec_given_up_acc.store(c.given_up_blocks, std::memory_order_relaxed);
    fec_late_acc.store(c.late, std::memory_order_relaxed);
}

void DatagramAir::sync_fec_estimates()
{
    if (!rx_stream)
    {
        return;
    }
    fec_spread_est_ms.store(rx_stream->spread_estimate_ms(),
                            std::memory_order_relaxed);
    fec_jitter_est_ms.store(rx_stream->jitter_estimate_ms(),
                            std::memory_order_relaxed);
}

DatagramAir::FecRxStats DatagramAir::fec_rx_stats() const
{
    refresh_fec_counters();
    FecRxStats s;
    s.recovered = fec_recovered_acc.load(std::memory_order_relaxed);
    s.lost = fec_decode_fail_acc.load(std::memory_order_relaxed);
    s.missing_data = fec_missing_data_acc.load(std::memory_order_relaxed);
    s.given_up_blocks = fec_given_up_acc.load(std::memory_order_relaxed);
    s.late = fec_late_acc.load(std::memory_order_relaxed);
    s.drops = fec_rx_drops_acc.load(std::memory_order_relaxed);
    s.spread_est_ms = fec_spread_est_ms.load(std::memory_order_relaxed);
    s.jitter_est_ms = fec_jitter_est_ms.load(std::memory_order_relaxed);
    return s;
}

void DatagramAir::refresh_fec_counters() const
{
    // The read stream's timer can give up a hole without releasing anything,
    // so its counters are read when asked, not only on delivery.
    std::lock_guard<std::mutex> lock(mu);
    const_cast<DatagramAir*>(this)->sync_fec_counters();
}

}  // namespace winject::core
