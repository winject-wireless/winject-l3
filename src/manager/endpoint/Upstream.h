#ifndef WINJECT_MANAGER_UPSTREAM_H_
#define WINJECT_MANAGER_UPSTREAM_H_

#include "endpoint/UpstreamStats.h"

#include <bfc/sized_buffer.hpp>
#include <bfcext/shared_sized_buffer.hpp>
#include <stddef.h>
#include <stdint.h>

namespace winject
{

class Upstream
{
public:
    virtual ~Upstream() = default;
    // is_fec comes from the slot's LC header: the SDU is an FEC shard.
    virtual void on_radio_rx(bfcext::shared_sized_buffer pkt, bool is_fec) = 0;
    virtual bool has_tx() = 0;

    virtual size_t get_tx_size() = 0;
    // Pops the next SDU of at most max bytes; *is_fec tells TxMux to set the
    // LC header FEC flag.
    virtual bfc::sized_buffer pull_tx(size_t max, bool* is_fec) = 0;

    virtual void announce_down() {}

    const UpstreamStats& stats() const
    {
        return stats_;
    }

    bool accept_air(const uint8_t* data, size_t len, const uint8_t** payload,
                    size_t* plen, bool* is_fec);
    uint64_t collect_air_seq_loss_delta();

protected:
    UpstreamStats stats_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UPSTREAM_H_
