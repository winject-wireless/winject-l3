#ifndef WINJECT_MANAGER_FRAMES_LC_HEADER_H_
#define WINJECT_MANAGER_FRAMES_LC_HEADER_H_

#include "utils/SafeInt.h"

#include <cstddef>
#include <cstdint>
#include <winject_fec/wire.hpp>

namespace winject::core
{

// The LC (logical channel) prefix of every slot payload:
//
//   u8 bus | u8 lc_type | u16 sn (big endian) | body
//
// lc_type 0 (plain): the body is the app datagram. lc_type 1 (fec): the body
// is a winject-fec shard (data or parity). Other types are dropped. sn counts
// every slot an upstream sends on its bus, parity included, so the receiver
// sees air gaps and duplicates whatever the type. No version field: both ends
// run matching managers.
using LcType = winject::fec::lc_type;

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

// UpstreamLink-owned air metadata before the body (bytes 1..3 of the slot
// prefix): the LC type and the sequence number.
struct UpstreamAirHeader
{
    static constexpr size_t k_len = 3;
    static_assert(sizeof(BEU16UA) == 2, "UpstreamAirHeader layout");
    // Forward distances below this are gaps; at or above, the slot is old.
    static constexpr uint16_t k_seq_half = 0x8000;

    static void write(uint8_t* out, LcType type, uint16_t seq)
    {
        out[0] = static_cast<uint8_t>(type);
        *reinterpret_cast<BEU16UA*>(out + 1) = seq;
    }

    static uint8_t read_type(const uint8_t* slot_payload)
    {
        return slot_payload[RouteHeader::k_len];
    }

    static uint16_t read_seq(const uint8_t* slot_payload)
    {
        return static_cast<uint16_t>(*reinterpret_cast<const BEU16UA*>(
            slot_payload + RouteHeader::k_len + 1));
    }

    static bool known_type(uint8_t type)
    {
        return type == static_cast<uint8_t>(LcType::plain) ||
               type == static_cast<uint8_t>(LcType::fec);
    }
};

inline constexpr size_t k_slot_prefix_len =
    RouteHeader::k_len + UpstreamAirHeader::k_len;

// Full on-wire slot prefix: route byte + upstream air header.
struct LCHeader
{
    static constexpr size_t k_len = k_slot_prefix_len;
    static constexpr uint16_t k_seq_half = UpstreamAirHeader::k_seq_half;

    static void write(uint8_t* out, uint8_t bus, LcType type, uint16_t seq)
    {
        RouteHeader::write(out, bus);
        UpstreamAirHeader::write(out + RouteHeader::k_len, type, seq);
    }

    static uint8_t read_bus(const uint8_t* data)
    {
        return RouteHeader::read_bus(data);
    }

    static uint8_t read_type(const uint8_t* data)
    {
        return UpstreamAirHeader::read_type(data);
    }

    static uint16_t read_seq(const uint8_t* data)
    {
        return UpstreamAirHeader::read_seq(data);
    }
};

}  // namespace winject::core

#endif  // WINJECT_MANAGER_FRAMES_LC_HEADER_H_
