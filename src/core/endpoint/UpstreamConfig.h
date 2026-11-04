#ifndef WINJECT_CORE_ENDPOINT_UPSTREAM_CONFIG_H_
#define WINJECT_CORE_ENDPOINT_UPSTREAM_CONFIG_H_

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace winject::core
{

enum class UpstreamMode
{
    udp_static,
    udp_client,
    udp_server,
    // In-process datagrams via named lossy queues. See QueueUpstream.
    queue,
};

// rx/tx are a UDP host:port, or QUEUE_FORWARDING registry names.
// UDP: both set = static; rx only = server bind; tx only = client connect.
// Queue: both required and distinct. rx is consumed (app toward air);
// tx is produced (air toward app).
struct EndpointAddress
{
    std::string rx;
    std::string tx;
};

inline UpstreamMode upstream_mode(const EndpointAddress& endpoint)
{
    const bool has_rx = !endpoint.rx.empty();
    const bool has_tx = !endpoint.tx.empty();
    if (has_rx && has_tx)
    {
        return UpstreamMode::udp_static;
    }
    if (has_rx)
    {
        return UpstreamMode::udp_server;
    }
    if (has_tx)
    {
        return UpstreamMode::udp_client;
    }
    return UpstreamMode::udp_static;
}

enum class FecType
{
    none,
    RsBlockErasure,
};

struct UpstreamConfig
{
    size_t index = 0;
    UpstreamMode mode = UpstreamMode::udp_static;
    // UDP host:port, or queue registry names when mode is queue.
    EndpointAddress endpoint;
    // QUEUE_FORWARDING: the host's running cv_reactor. Empty for UDP.
    std::string reactor;
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    size_t scheduler_budget = 256;
    size_t rx_queue_capacity = 1024;
    FecType fec_type = FecType::none;
    int fec_k = 0;
    int fec_n = 0;
    int fec_timeout_ms = 20;
};

}  // namespace winject::core

#endif  // WINJECT_CORE_ENDPOINT_UPSTREAM_CONFIG_H_
