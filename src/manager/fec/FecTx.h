#ifndef WINJECT_MANAGER_FEC_FEC_TX_H_
#define WINJECT_MANAGER_FEC_FEC_TX_H_

#include "fec/RsBlockErasure.h"
#include "utils/DeadlineTimer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace winject
{

// TX half of the RS FEC: app datagrams in, air shards out through emit.
// A partial block is flushed by a CV reactor timer at its timeout.
// Reactor thread only, except pending_stats.
class FecTx
{
public:
    using Reactor = DeadlineTimer::Reactor;
    using Emit = std::function<void(const uint8_t* shard, size_t len)>;

    // Drops any partial block. The SDU sequence keeps running across restarts.
    bool start(std::shared_ptr<Reactor> reactor, int k, int n, int timeout_ms,
               Emit emit);
    void stop();
    void set_lifetime(std::weak_ptr<void> owner)
    {
        timer.set_lifetime(std::move(owner));
    }
    bool enabled() const
    {
        return codec.enabled();
    }
    static constexpr size_t max_original()
    {
        return RsBlockErasure::max_original();
    }

    void push(const uint8_t* data, size_t len);

    // Any thread.
    void pending_stats(uint64_t* packets, uint64_t* bytes) const;

private:
    void on_timer();
    void emit_shards();
    void sync();

    RsBlockErasure codec;
    Emit emit;
    DeadlineTimer timer;
    std::vector<std::vector<uint8_t>> shards;
    std::atomic<uint64_t> pending_packets{0};
    std::atomic<uint64_t> pending_bytes{0};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_FEC_FEC_TX_H_
