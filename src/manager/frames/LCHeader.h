#ifndef WINJECT_MANAGER_FRAMES_LC_HEADER_H_
#define WINJECT_MANAGER_FRAMES_LC_HEADER_H_

#include "utils/SafeInt.h"

#include <cstddef>
#include <cstdint>

namespace winject
{

// Air logical-channel header: [u8 bus][BE u16 seq] before SDU body.
struct LCHeader
{
    static constexpr size_t k_len = 3;
    static_assert(k_len == 1 + sizeof(BEU16UA), "LCHeader layout");

    static void write(uint8_t* out, uint8_t bus, uint16_t seq)
    {
        out[0] = bus;
        BEU16UA* word = reinterpret_cast<BEU16UA*>(out + 1);
        *word = seq;
    }

    static uint8_t read_bus(const uint8_t* data)
    {
        return data[0];
    }

    static uint16_t read_seq(const uint8_t* data)
    {
        const BEU16UA* word = reinterpret_cast<const BEU16UA*>(data + 1);
        return static_cast<uint16_t>(*word);
    }
};

}  // namespace winject

#endif  // WINJECT_MANAGER_FRAMES_LC_HEADER_H_
