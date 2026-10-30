#include "fec/FecRx.h"

#include <utility>

namespace winject
{

bool FecRx::start(std::shared_ptr<Reactor> reactor, Deliver deliver)
{
    stop();
    if (!reactor || !deliver)
    {
        return false;
    }
    codec = RsBlockErasure{};
    this->deliver = std::move(deliver);
    timer.bind(std::move(reactor),
               [this]
               {
                   on_timer();
               });
    sync();
    return true;
}

void FecRx::stop()
{
    timer.unbind();
    deliver = nullptr;
}

void FecRx::push(bfc::shared_sized_buffer shard)
{
    if (!deliver)
    {
        return;
    }
    fec_vendor::fec_rx_payload_list payloads;
    codec.push_air(std::move(shard), &payloads);
    sync();
    deliver_all(payloads);
}

void FecRx::on_timer()
{
    fec_vendor::fec_rx_payload_list payloads;
    codec.poll_rx(&payloads);
    sync();
    deliver_all(payloads);
}

void FecRx::deliver_all(const fec_vendor::fec_rx_payload_list& payloads)
{
    for (const auto& payload : payloads)
    {
        if (!deliver)
        {
            return;
        }
        deliver(payload.u8(), payload.size());
    }
}

void FecRx::sync()
{
    std::chrono::steady_clock::time_point deadline;
    if (codec.next_rx_deadline(&deadline))
    {
        timer.arm(deadline);
    }
    else
    {
        timer.cancel();
    }
    recovered_total.store(codec.recovered(), std::memory_order_relaxed);
    decode_fail_total.store(
        codec.decode_fail() + codec.hdr_errors() + codec.kn_mismatch(),
        std::memory_order_relaxed);
}

}  // namespace winject
