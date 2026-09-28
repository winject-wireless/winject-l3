#ifndef WINJECT_MANAGER_FRAMES_LC_SEQUENCE_H_
#define WINJECT_MANAGER_FRAMES_LC_SEQUENCE_H_

#include "utils/SafeInt.h"

#include <cstddef>
#include <cstdint>

namespace winject
{

// Big-endian uint16 prefix on each logical-channel (LCP) body before MPDU pack.
struct LCSequence
{
    static constexpr size_t k_len = sizeof(BEU16UA);
    static_assert(k_len == 2, "LCSequence is one BEU16UA");

    static void write(uint8_t* out, uint16_t seq)
    {
        BEU16UA* word = reinterpret_cast<BEU16UA*>(out);
        *word = seq;
    }

    static uint16_t read(const uint8_t* data)
    {
        const BEU16UA* word = reinterpret_cast<const BEU16UA*>(data);
        return static_cast<uint16_t>(*word);
    }
};

}  // namespace winject

#endif  // WINJECT_MANAGER_FRAMES_LC_SEQUENCE_H_
