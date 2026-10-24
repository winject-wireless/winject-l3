#ifndef WINJECT_MANAGER_UDP_UPSTREAM_H_
#define WINJECT_MANAGER_UDP_UPSTREAM_H_
#include "endpoint/Upstream.h"
#include "utils/IOReactor.h"

#include <bfc/socket.hpp>
namespace winject
{
class UDPUpstream final : public Upstream
{
public:
    UDPUpstream(std::shared_ptr<Reactor> reactor, std::shared_ptr<IOReactor> io,
                const UpstreamConfig& config,
                std::shared_ptr<BoundedLossyQueue> radio_tx_queue);
    ~UDPUpstream() override;
    void bind_callbacks() override;

private:
    void on_socket_readable();
    void deliver_datagram(const uint8_t* data, size_t len) override;
    std::shared_ptr<IOReactor> io_;
    bfc::socket socket_;
    std::mutex socket_mu_;
    sockaddr_in destination_{};
    bool have_destination_ = false;
    bool registered_ = false;
};
}  // namespace winject
#endif
