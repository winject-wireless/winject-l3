#ifndef WINJECT_MANAGER_RS_BLOCK_ERASURE_H_
#define WINJECT_MANAGER_RS_BLOCK_ERASURE_H_

#include "utils/NetUtil.h"

#include "fec/vendor/rs_block_erasure.hpp"

namespace winject
{
// Winject's existing MTU/config profile over the vendored vstreamer codec.
// The codec supports n == k; winject config continues to require parity.
class RsBlockErasure : public fec_vendor::rs_block_erasure
{
public:
    static constexpr size_t k_max_n = 31;
    bool init(int k, int n, int timeout_ms)
    {
        return k >= 1 && n > k && n <= static_cast<int>(k_max_n) &&
               fec_vendor::rs_block_erasure::init(k, n, timeout_ms,
                                                  k_stream_payload_max);
    }
    static constexpr size_t max_original()
    {
        return k_stream_payload_max - k_header_len - k_len_prefix;
    }
};
}  // namespace winject
#endif
