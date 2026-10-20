#ifndef WINJECT_MANAGER_UDP_ENDPOINT_H_
#define WINJECT_MANAGER_UDP_ENDPOINT_H_

#include "Config.h"
#include "endpoint/HostEndpoint.h"
#include "utils/IOReactor.h"

#include <bfc/sized_buffer.hpp>
#include <bfc/socket.hpp>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

namespace winject
{

class UdpEndpoint : public HostEndpoint
{
public:
    UdpEndpoint() = default;
    ~UdpEndpoint() override;

    bool open(IOReactor& reactor, const UpstreamConfig& cfg,
              std::function<void()> tx_wake = nullptr);
    void close() override;

    const UdpPeerEndpoint& peer_endpoints() const
    {
        return peer_endpoints_;
    }
    const char* type_name() const override
    {
        return "UDP";
    }
    std::string endpoint_rx() const override
    {
        return peer_endpoints_.rx;
    }
    std::string endpoint_tx() const override
    {
        return peer_endpoints_.tx;
    }

protected:
    bool can_deliver_app() const override;
    void deliver_app(const uint8_t* data, size_t len) override;

private:
    void on_app();

    IOReactor* reactor = nullptr;
    bfc::socket sock;
    UpstreamMode mode = UpstreamMode::udp_static;
    UdpPeerEndpoint peer_endpoints_;
    sockaddr_in dest{};
    bool dest_valid = false;
    bfc::sized_buffer rx_buf;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UDP_ENDPOINT_H_
