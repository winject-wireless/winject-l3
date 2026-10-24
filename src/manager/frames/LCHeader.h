#ifndef WINJECT_MANAGER_FRAMES_LC_HEADER_H_
#define WINJECT_MANAGER_FRAMES_LC_HEADER_H_

#include "utils/SafeInt.h"

#include <cstddef>
#include <cstdint>

namespace winject
{

// Demux/radio routing: first byte of each slot payload.
struct RouteHeader
{
    static constexpr size_t k_len = 1;

    static void write(uint8_t* out, uint8_t bus)
    {
        out[0] = bus;
    }

    static uint8_t read_bus(const uint8_t* slot_payload)
    {
        return slot_payload[0];
    }
};

// Upstream-owned air metadata before the SDU (bytes 1..2 of the slot prefix).
struct UpstreamAirHeader
{
    static constexpr size_t k_len = 2;
    static_assert(k_len == sizeof(BEU16UA), "UpstreamAirHeader layout");
    static constexpr uint16_t k_fec_flag = 0x8000;
    static constexpr uint16_t k_seq_mask = 0x7FFF;
    // Forward distances below this are gaps; at or above, the slot is old.
    static constexpr uint16_t k_seq_half = 0x4000;

    static void write(uint8_t* out, uint16_t seq, bool is_fec)
    {
        BEU16UA* word = reinterpret_cast<BEU16UA*>(out);
        *word = static_cast<uint16_t>((seq & k_seq_mask) |
                                      (is_fec ? k_fec_flag : 0));
    }

    static uint16_t read_seq(const uint8_t* slot_payload)
    {
        return static_cast<uint16_t>(read_word(slot_payload) & k_seq_mask);
    }

    static bool read_is_fec(const uint8_t* slot_payload)
    {
        return (read_word(slot_payload) & k_fec_flag) != 0;
    }

private:
    static uint16_t read_word(const uint8_t* slot_payload)
    {
        const BEU16UA* word =
            reinterpret_cast<const BEU16UA*>(slot_payload + RouteHeader::k_len);
        return static_cast<uint16_t>(*word);
    }
};

inline constexpr size_t k_slot_prefix_len =
    RouteHeader::k_len + UpstreamAirHeader::k_len;

// Full on-wire slot prefix: route byte + upstream air header.
struct LCHeader
{
    static constexpr size_t k_len = k_slot_prefix_len;
    static constexpr uint16_t k_fec_flag = UpstreamAirHeader::k_fec_flag;
    static constexpr uint16_t k_seq_mask = UpstreamAirHeader::k_seq_mask;
    static constexpr uint16_t k_seq_half = UpstreamAirHeader::k_seq_half;

    static void write(uint8_t* out, uint8_t bus, uint16_t seq, bool is_fec)
    {
        RouteHeader::write(out, bus);
        UpstreamAirHeader::write(out + RouteHeader::k_len, seq, is_fec);
    }

    static uint8_t read_bus(const uint8_t* data)
    {
        return RouteHeader::read_bus(data);
    }

    static uint16_t read_seq(const uint8_t* data)
    {
        return UpstreamAirHeader::read_seq(data);
    }

    static bool read_is_fec(const uint8_t* data)
    {
        return UpstreamAirHeader::read_is_fec(data);
    }
};

}  // namespace winject

#endif  // WINJECT_MANAGER_FRAMES_LC_HEADER_H_
