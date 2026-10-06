#ifndef WINJECT_MANAGER_FRAMES_LC_HEADER_H_
#define WINJECT_MANAGER_FRAMES_LC_HEADER_H_

#include "utils/SafeInt.h"

#include <cstddef>
#include <cstdint>

namespace winject
{

// Air logical-channel header before the SDU body:
//   [0]    bus (u8)
//   [1..2] BE u16: is_fec (1, MSB) | seq (15)
// is_fec marks the SDU as an RsBlockErasure shard. The receiver never looks
// inside the SDU to decide that.
struct LCHeader
{
    static constexpr size_t k_len = 3;
    static_assert(k_len == 1 + sizeof(BEU16UA), "LCHeader layout");
    static constexpr uint16_t k_fec_flag = 0x8000;
    static constexpr uint16_t k_seq_mask = 0x7FFF;
    // Forward distances below this are gaps; at or above, the slot is old.
    static constexpr uint16_t k_seq_half = 0x4000;

    static void write(uint8_t* out, uint8_t bus, uint16_t seq, bool is_fec)
    {
        out[0] = bus;
        BEU16UA* word = reinterpret_cast<BEU16UA*>(out + 1);
        *word = static_cast<uint16_t>((seq & k_seq_mask) |
                                      (is_fec ? k_fec_flag : 0));
    }

    static uint8_t read_bus(const uint8_t* data)
    {
        return data[0];
    }

    static uint16_t read_seq(const uint8_t* data)
    {
        return static_cast<uint16_t>(read_word(data) & k_seq_mask);
    }

    static bool read_is_fec(const uint8_t* data)
    {
        return (read_word(data) & k_fec_flag) != 0;
    }

private:
    static uint16_t read_word(const uint8_t* data)
    {
        const BEU16UA* word = reinterpret_cast<const BEU16UA*>(data + 1);
        return static_cast<uint16_t>(*word);
    }
};

}  // namespace winject

#endif  // WINJECT_MANAGER_FRAMES_LC_HEADER_H_
