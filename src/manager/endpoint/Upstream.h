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
    virtual void on_radio_rx(bfcext::shared_sized_buffer pkt) = 0;
    virtual bool has_tx() = 0;

    virtual size_t get_tx_size() = 0;
    virtual bfc::sized_buffer pull_tx(size_t max) = 0;

    virtual void announce_down() {}

    const UpstreamStats& stats() const
    {
        return stats_;
    }

    bool stamp_air(uint8_t bus, uint16_t* tx_seq, uint8_t* out, size_t max,
                   const uint8_t* data, size_t len, size_t* out_len);
    bool accept_air(const uint8_t* data, size_t len, const uint8_t** payload,
                    size_t* plen);
    uint64_t collect_air_seq_loss_delta();

protected:
    UpstreamStats stats_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UPSTREAM_H_
