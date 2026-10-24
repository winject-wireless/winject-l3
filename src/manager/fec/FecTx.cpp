#include "fec/FecTx.h"

#include <utility>

namespace winject
{

bool FecTx::start(std::shared_ptr<Reactor> reactor, int k, int n,
                  int timeout_ms, Emit emit)
{
    stop();
    if (!reactor || !emit || !codec.init(k, n, timeout_ms))
    {
        codec.disable();
        return false;
    }
    this->emit = std::move(emit);
    timer.bind(std::move(reactor),
               [this]
               {
                   on_timer();
               });
    return true;
}

void FecTx::stop()
{
    timer.unbind();
    codec.disable();
    emit = nullptr;
    sync();
}

void FecTx::push(const uint8_t* data, size_t len)
{
    if (!codec.enabled())
    {
        return;
    }
    codec.push_app(data, len, &shards);
    emit_shards();
    sync();
}

void FecTx::on_timer()
{
    codec.on_tick(&shards);
    emit_shards();
    sync();
}

void FecTx::emit_shards()
{
    for (const auto& shard : shards)
    {
        emit(shard.data(), shard.size());
    }
    shards.clear();
}

void FecTx::sync()
{
    std::chrono::steady_clock::time_point deadline;
    if (codec.next_deadline(&deadline))
    {
        timer.arm(deadline);
    }
    else
    {
        timer.cancel();
    }
    uint64_t packets = 0, bytes = 0;
    codec.pending_tx_stats(&packets, &bytes);
    pending_packets.store(packets, std::memory_order_relaxed);
    pending_bytes.store(bytes, std::memory_order_relaxed);
}

void FecTx::pending_stats(uint64_t* packets, uint64_t* bytes) const
{
    if (packets)
    {
        *packets = pending_packets.load(std::memory_order_relaxed);
    }
    if (bytes)
    {
        *bytes = pending_bytes.load(std::memory_order_relaxed);
    }
}

}  // namespace winject
