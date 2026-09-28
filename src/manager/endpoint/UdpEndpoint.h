#ifndef WINJECT_MANAGER_UDP_ENDPOINT_H_
#define WINJECT_MANAGER_UDP_ENDPOINT_H_

#include "Config.h"
#include "endpoint/Upstream.h"
#include "fec/RsBlockErasure.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <bfc/sized_buffer.hpp>
#include <bfc/timer.hpp>
#include <deque>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <string>

namespace winject
{

class UdpEndpoint : public Upstream
{
public:
    UdpEndpoint() = default;
    ~UdpEndpoint() override;
    bool open(IOReactor& reactor, const UpstreamConfig& cfg, std::function<void()> tx_wake = nullptr);
    void close();

    void on_radio_rx(bfcext::shared_sized_buffer pkt) override;
    bool has_tx() override;
    size_t get_tx_size() override;

    bfc::sized_buffer pull_tx(size_t max) override;
    void announce_down() override;

    bool set_fec(FecType type, int k, int n, std::string* error);
    void get_fec(FecType* type, int* k, int* n) const;

private:
    void on_app();
    void push_tx(bfc::sized_buffer pkt);
    void enqueue_air(bfc::sized_buffer pkt);
    void stage_fec_shard_for_tx(size_t max);
    void arm_fec_timer();
    void cancel_fec_timer();
    void on_fec_timer();
    void sync_fec_timer_after_push();

    using FecTimerId = bfc::timer<std::function<void()>>::timer_id_t;

    IOReactor* reactor = nullptr;
    std::function<void()> tx_wake_;
    bfc::socket sock;
    UpstreamMode mode = UpstreamMode::udp_static;
    sockaddr_in dest{};
    bool dest_valid = false;
    mutable std::mutex tx_mu_;
    std::deque<bfc::sized_buffer> txq;
    bfc::sized_buffer rx_buf;
    RsBlockErasure fec;
    int fec_timeout_ms = RsBlockErasure::k_default_timeout_ms;
    bool fec_timer_armed_ = false;
    FecTimerId fec_timer_id_{};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UDP_ENDPOINT_H_
