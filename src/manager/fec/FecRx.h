#ifndef WINJECT_MANAGER_FEC_FEC_RX_H_
#define WINJECT_MANAGER_FEC_FEC_RX_H_

#include "fec/RsBlockErasure.h"
#include "utils/DeadlineTimer.h"

#include <atomic>
#include <bfc/shared_sized_buffer.hpp>
#include <functional>
#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace winject
{

// RX half of the RS FEC: air shards in, app datagrams out in sequence through
// deliver. k/n come from each shard header, so RX needs no configuration.
// A CV reactor timer at the sequencer's next deadline releases what waits on
// time. Reactor thread only, except the counters.
class FecRx
{
public:
    using Reactor = DeadlineTimer::Reactor;
    using Deliver = std::function<void(const uint8_t* data, size_t len)>;

    // Starts a fresh sequencer.
    bool start(std::shared_ptr<Reactor> reactor, Deliver deliver);
    void stop();
    void set_lifetime(std::weak_ptr<void> owner)
    {
        timer.set_lifetime(std::move(owner));
    }

    void push(bfc::shared_sized_buffer shard);

    // Any thread.
    uint64_t recovered() const
    {
        return recovered_total.load(std::memory_order_relaxed);
    }
    // Includes malformed and k/n-mismatched shards.
    uint64_t decode_fail() const
    {
        return decode_fail_total.load(std::memory_order_relaxed);
    }

private:
    void on_timer();
    void deliver_all(const fec_vendor::fec_rx_payload_list& payloads);
    void sync();

    RsBlockErasure codec;
    Deliver deliver;
    DeadlineTimer timer;
    std::atomic<uint64_t> recovered_total{0};
    std::atomic<uint64_t> decode_fail_total{0};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_FEC_FEC_RX_H_
