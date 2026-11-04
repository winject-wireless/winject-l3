#ifndef WINJECT_L3_PUBLIC_UPSTREAM_H_
#define WINJECT_L3_PUBLIC_UPSTREAM_H_

#include "winject_l3/Types.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace winject::core
{
class UpstreamQueue;
}

namespace winject
{

// The queue pair an upstream with an l3-owned queue hands to on_queue.
using UpstreamQueue = core::UpstreamQueue;

enum class FecType
{
    none,
    rs_block,
};

struct Fec
{
    FecType type = FecType::none;
    int k = 0;
    int n = 0;
    std::chrono::milliseconds timeout{20};
};

enum class Transport
{
    udp,
    queue,
};

// One upstream: an application stream on l3's radio. Its air address
// is (radio domain, bus_rx).
struct UpstreamSpec
{
    uint8_t id = 0;
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    // udp: rx and tx are host:port; both = static, rx only = server bind,
    // tx only = client connect. queue: registry names of the app queues,
    // rx consumed (app toward air) and tx produced (air toward app), and the
    // host's cv_reactor in reactor.
    Transport transport = Transport::udp;
    std::string rx;
    std::string tx;
    std::string reactor;
    Fec fec;
    size_t quanta = 256;  // scheduler budget, bytes per turn
    size_t rx_queue_capacity = 1024;
    // If set, a queue upstream hands the app its end here, at add and again
    // on every hot-swap, on l3's thread: store the queue only, l3 calls
    // from here return EDEADLK. With rx, tx and reactor all empty, l3 owns
    // the queues and this is required (EINVAL).
    std::function<void(std::shared_ptr<UpstreamQueue>)> on_queue;
};

struct UpstreamRxStats
{
    Fec fec;
    uint64_t bytes = 0;
    uint64_t packets = 0;
    uint64_t oversize = 0;
    uint64_t air_gap = 0;
    uint64_t fec_recovered = 0;
    uint64_t fec_lost = 0;
    uint64_t fec_bytes = 0;
    uint64_t fec_packets = 0;
    uint64_t fec_gap = 0;
    // Queue transport: deliveries that evicted the oldest datagram from the
    // app's RX queue. 0 for UDP.
    uint64_t push_drops = 0;
};

struct UpstreamTxStats
{
    Fec fec;
    uint64_t bytes = 0;
    uint64_t packets = 0;
    uint64_t fec_bytes = 0;
    uint64_t fec_packets = 0;
    uint64_t pending_bytes = 0;
    uint64_t pending_packets = 0;
};

// Several fields, one hot-swap.
struct UpstreamPatch
{
    std::optional<Fec> fec;
    std::optional<size_t> quanta;
};

// A handle to one upstream, by id. Cheap to copy, safe from any thread
// (see winject::L3::run). An FEC change hot-swaps the data path underneath and
// the handle keeps working. After remove its calls return ENOENT; if the same
// id is added again, the handle reaches the new upstream. A default handle is
// attached to nothing: calls return ESRCH.
class Upstream
{
public:
    Upstream() = default;

    uint8_t id() const
    {
        return upstream_id;
    }
    bool valid() const;  // still added?

    Result<UpstreamSpec> spec() const;  // full snapshot (on_queue left empty)
    Result<Fec> fec() const;
    Status set_fec(const Fec& fec);  // hot-swap, keeps the air sequence
    Result<size_t> quanta() const;
    Status set_quanta(size_t quanta);
    // fec set, alone or with quanta: one hot-swap. Only quanta: no hot-swap,
    // as set_quanta. Neither: EINVAL.
    Result<UpstreamSpec> update(const UpstreamPatch& patch);

    Result<UpstreamRxStats> rx_stats() const;
    Result<UpstreamTxStats> tx_stats() const;

private:
    friend struct l3::detail::HandleAccess;
    std::shared_ptr<l3::detail::L3Core> core;
    uint8_t upstream_id = 0;
};

class UpstreamManager
{
public:
    UpstreamManager() = default;

    // EEXIST (id taken), EINVAL (spec), ENODEV (no radio created), EIO.
    Result<Upstream> add(const UpstreamSpec& spec);
    Result<Upstream> get(uint8_t id) const;  // ENOENT
    Status remove(uint8_t id);               // ENOENT
    Result<std::vector<Upstream>> list() const;

private:
    friend struct l3::detail::HandleAccess;
    std::shared_ptr<l3::detail::L3Core> core;
};

}  // namespace winject

#endif  // WINJECT_L3_PUBLIC_UPSTREAM_H_
