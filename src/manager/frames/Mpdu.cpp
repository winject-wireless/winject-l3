#include "frames/Mpdu.h"

#include <atomic>
#include <string.h>

namespace
{

static const uint8_t kAddr3Prefix[4] = {WIFI_BSSID_PREFIX};
static std::atomic<uint16_t> g_mpdu_tx_seq{0};

void emit_bits(uint8_t* packed, size_t* bit, uint16_t value, int nbits)
{
    for (int i = 0; i < nbits; i++)
    {
        const size_t b = (*bit)++;
        if ((value & 1u) != 0)
        {
            packed[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
        }
        value = static_cast<uint16_t>(value >> 1);
    }
}

uint16_t take_bits(const uint8_t* packed, size_t* bit, int nbits)
{
    uint16_t value = 0;
    for (int i = 0; i < nbits; i++)
    {
        const size_t b = (*bit)++;
        if ((packed[b / 8] & (1u << (b % 8))) != 0)
        {
            value |= static_cast<uint16_t>(1u << i);
        }
    }
    return value;
}

uint16_t next_tx_seq()
{
    return static_cast<uint16_t>(
        g_mpdu_tx_seq.fetch_add(1, std::memory_order_relaxed) & 0x0FFF);
}

bool addr3_bytes_prefix_match(const uint8_t addr3[6])
{
    if (addr3 == nullptr)
    {
        return false;
    }
    return memcmp(addr3, kAddr3Prefix, 4) == 0;
}

void write_addr3_domain(uint8_t addr3[6], uint16_t domain)
{
    if (addr3 == nullptr)
    {
        return;
    }
    memcpy(addr3, kAddr3Prefix, 4);
    addr3[4] = static_cast<uint8_t>(domain >> 8);
    addr3[5] = static_cast<uint8_t>(domain);
}

uint16_t read_addr3_domain(const uint8_t addr3[6])
{
    if (addr3 == nullptr || !addr3_bytes_prefix_match(addr3))
    {
        return 0;
    }
    return static_cast<uint16_t>((addr3[4] << 8) | addr3[5]);
}

void pack_slot_sizes_to_addrs(uint8_t addr1[6], uint8_t addr2[6],
                              const uint16_t slot_sizes[WIFI_PDU_SLOTS])
{
    if (addr1 == nullptr || addr2 == nullptr || slot_sizes == nullptr)
    {
        return;
    }
    uint8_t packed[12] = {};
    size_t bit = 0;
    emit_bits(packed, &bit, 1, 1);
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        const uint16_t size = static_cast<uint16_t>(slot_sizes[i] & 0x7FFu);
        emit_bits(packed, &bit, size, 11);
    }
    memcpy(addr1, packed, 6);
    memcpy(addr2, packed + 6, 6);
}

void unpack_slot_sizes_from_addrs(const uint8_t addr1[6], const uint8_t addr2[6],
                                  uint16_t slot_sizes[WIFI_PDU_SLOTS])
{
    if (addr1 == nullptr || addr2 == nullptr || slot_sizes == nullptr)
    {
        return;
    }
    uint8_t packed[12];
    memcpy(packed, addr1, 6);
    memcpy(packed + 6, addr2, 6);
    size_t bit = 0;
    (void)take_bits(packed, &bit, 1);
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        slot_sizes[i] = take_bits(packed, &bit, 11);
    }
}

}  // namespace

namespace winject
{

Mpdu::Mpdu(uint8_t* data, size_t len)
{
    if (data == nullptr || len < WIFI_HDR_LEN)
    {
        return;
    }
    ieee_ = ieee_802_11::Frame(data, data + len);
    ieee_.set_enable_fcs(false);
}

size_t Mpdu::buffer_size() const
{
    if (ieee_.frame_control == nullptr || ieee_.last == nullptr)
    {
        return 0;
    }
    return static_cast<size_t>(
        ieee_.last - reinterpret_cast<uint8_t*>(ieee_.frame_control));
}

size_t Mpdu::frame_body_bytes() const
{
    if (ieee_.frame_body == nullptr)
    {
        return 0;
    }
    auto& frame = const_cast<ieee_802_11::Frame&>(ieee_);
    return frame.frame_body_size();
}

bool Mpdu::init_data_frame()
{
    if (data_frame_ready_)
    {
        return ieee_.frame_body != nullptr && ieee_.address1 != nullptr &&
               ieee_.address2 != nullptr;
    }

    if (buffer_size() < WIFI_HDR_LEN)
    {
        return false;
    }

    using namespace ieee_802_11;
    ieee_.frame_control->protocol_type = FrameControl::E_TYPE_DATA;
    ieee_.frame_control->flags = 0;
    *ieee_.duration = 0;
    ieee_.rescan();

    if (ieee_.frame_body == nullptr || ieee_.address1 == nullptr ||
        ieee_.address2 == nullptr)
    {
        return false;
    }

    const size_t hdr_len = static_cast<size_t>(
        ieee_.frame_body - reinterpret_cast<uint8_t*>(ieee_.frame_control));
    if (buffer_size() < hdr_len)
    {
        return false;
    }
    const size_t payload = buffer_size() - hdr_len;
    if (payload > WIFI_PAYLOAD_MAX)
    {
        return false;
    }
    ieee_.set_body_size(static_cast<uint16_t>(payload));
    data_frame_ready_ = true;
    return true;
}

bool Mpdu::rescan()
{
    bound_80211_ = false;
    data_frame_ready_ = false;

    if (buffer_size() < WIFI_HDR_LEN)
    {
        return false;
    }

    ieee_.set_enable_fcs(false);
    ieee_.rescan();

    if (!validate_data_frame())
    {
        return false;
    }
    if (ieee_.frame_body == nullptr)
    {
        return false;
    }

    const size_t hdr_len = static_cast<size_t>(
        ieee_.frame_body - reinterpret_cast<uint8_t*>(ieee_.frame_control));
    if (buffer_size() < hdr_len)
    {
        return false;
    }
    const size_t payload = buffer_size() - hdr_len;
    if (payload > WIFI_PAYLOAD_MAX)
    {
        return false;
    }
    ieee_.set_body_size(static_cast<uint16_t>(payload));

    if (!unpack_slots_from_header())
    {
        return false;
    }
    if (!validate_slot_payload_layout())
    {
        return false;
    }

    data_frame_ready_ = true;
    bound_80211_ = true;
    return true;
}

bool Mpdu::validate_data_frame() const
{
    using namespace ieee_802_11;

    if (ieee_.frame_control == nullptr)
    {
        return false;
    }
    const auto type =
        ieee_.frame_control->protocol_type & FrameControl::E_TYPE_MASK;
    const auto mtype = type & FrameControl::E_MTYPE_MASK;
    if (mtype != FrameControl::E_MTYPE_DATA)
    {
        return false;
    }
    if (type != FrameControl::E_TYPE_DATA)
    {
        return false;
    }
    if (ieee_.frame_control->flags != 0)
    {
        return false;
    }
    return true;
}

void Mpdu::refresh_slot_cache_from_header() const
{
    if (ieee_.address1 == nullptr || ieee_.address2 == nullptr)
    {
        return;
    }
    unpack_slot_sizes_from_addrs(ieee_.address1->address, ieee_.address2->address,
                                 slot_cache_);
}

bool Mpdu::unpack_slots_from_header()
{
    if (ieee_.address1 == nullptr || ieee_.address2 == nullptr)
    {
        return false;
    }
    refresh_slot_cache_from_header();
    return true;
}

void Mpdu::pack_slots_to_header()
{
    if (ieee_.address1 == nullptr || ieee_.address2 == nullptr)
    {
        return;
    }
    pack_slot_sizes_to_addrs(ieee_.address1->address, ieee_.address2->address,
                             slot_cache_);
}

bool Mpdu::validate_slot_payload_layout() const
{
    if (ieee_.frame_body == nullptr)
    {
        return false;
    }
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        if (slot_cache_[i] > WIFI_PAYLOAD_MAX)
        {
            return false;
        }
    }
    const size_t want = slot_payload_bytes();
    const size_t got = frame_body_bytes();
    if (want != got)
    {
        return false;
    }
    return true;
}

size_t Mpdu::slot_payload_bytes() const
{
    size_t sum = 0;
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        sum += slot_cache_[i];
    }
    return sum;
}

bool Mpdu::slot_payload_offset(uint8_t slot_index, size_t* offset) const
{
    if (offset == nullptr || slot_index >= WIFI_PDU_SLOTS)
    {
        return false;
    }
    size_t off = 0;
    for (uint8_t i = 0; i < slot_index; i++)
    {
        off += slot_cache_[i];
    }
    *offset = off;
    return true;
}

void Mpdu::set_slot_payload(uint8_t slot_index, uint16_t slot_size)
{
    if (slot_index >= WIFI_PDU_SLOTS)
    {
        return;
    }
    if (!init_data_frame())
    {
        return;
    }
    slot_cache_[slot_index] = static_cast<uint16_t>(slot_size & 0x7FFu);
    pack_slots_to_header();
}

uint16_t Mpdu::slot_payload_size(uint8_t slot_index) const
{
    if (slot_index >= WIFI_PDU_SLOTS)
    {
        return 0;
    }
    if (ieee_.address1 == nullptr || ieee_.address2 == nullptr)
    {
        return 0;
    }
    refresh_slot_cache_from_header();
    return slot_cache_[slot_index];
}

bfc::const_buffer_view Mpdu::get_slot_payload(uint8_t slot_index) const
{
    if (slot_index >= WIFI_PDU_SLOTS || ieee_.frame_body == nullptr)
    {
        return bfc::const_buffer_view();
    }
    refresh_slot_cache_from_header();
    if (slot_cache_[slot_index] == 0)
    {
        return bfc::const_buffer_view();
    }

    size_t off = 0;
    if (!slot_payload_offset(slot_index, &off))
    {
        return bfc::const_buffer_view();
    }

    const size_t slot_size = slot_cache_[slot_index];
    const size_t total_body = frame_body_bytes();
    if (off + slot_size > total_body)
    {
        return bfc::const_buffer_view();
    }

    return bfc::const_buffer_view(ieee_.frame_body + off, slot_size);
}

bfc::buffer_view Mpdu::get_slot_payload(uint8_t slot_index)
{
    if (slot_index >= WIFI_PDU_SLOTS || ieee_.frame_body == nullptr)
    {
        return bfc::buffer_view();
    }
    refresh_slot_cache_from_header();
    if (slot_cache_[slot_index] == 0)
    {
        return bfc::buffer_view();
    }

    size_t off = 0;
    if (!slot_payload_offset(slot_index, &off))
    {
        return bfc::buffer_view();
    }

    const size_t slot_size = slot_cache_[slot_index];
    const size_t total_body = frame_body_bytes();
    if (off + slot_size > total_body)
    {
        return bfc::buffer_view();
    }

    return bfc::buffer_view(ieee_.frame_body + off, slot_size);
}

bool Mpdu::is_valid_winject_frame() const
{
    if (ieee_.address3 == nullptr)
    {
        return false;
    }
    return addr3_bytes_prefix_match(ieee_.address3->address);
}

uint16_t Mpdu::get_domain() const
{
    if (ieee_.address3 == nullptr)
    {
        return 0;
    }
    return read_addr3_domain(ieee_.address3->address);
}

void Mpdu::set_domain(uint16_t domain)
{
    if (!init_data_frame() || ieee_.address3 == nullptr)
    {
        return;
    }
    write_addr3_domain(ieee_.address3->address, domain);
}

ieee_802_11::Frame& Mpdu::ieee()
{
    return ieee_;
}

const ieee_802_11::Frame& Mpdu::ieee() const
{
    return ieee_;
}

uint16_t next_tx_sequence()
{
    return next_tx_seq();
}

}  // namespace winject
