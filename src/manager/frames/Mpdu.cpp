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
    wlan_frame = ieee_802_11::Frame(data, data + len);
    wlan_frame.set_enable_fcs(false);
}

size_t Mpdu::buffer_size() const
{
    if (wlan_frame.frame_control == nullptr || wlan_frame.last == nullptr)
    {
        return 0;
    }
    return static_cast<size_t>(
        wlan_frame.last - reinterpret_cast<uint8_t*>(wlan_frame.frame_control));
}

size_t Mpdu::frame_body_bytes() const
{
    if (wlan_frame.frame_body == nullptr)
    {
        return 0;
    }
    auto& frame = const_cast<ieee_802_11::Frame&>(wlan_frame);
    return frame.frame_body_size();
}

bool Mpdu::init_data_frame()
{
    if (data_frame_ready)
    {
        return wlan_frame.frame_body != nullptr && wlan_frame.address1 != nullptr &&
               wlan_frame.address2 != nullptr;
    }

    if (buffer_size() < WIFI_HDR_LEN)
    {
        return false;
    }

    using namespace ieee_802_11;
    wlan_frame.frame_control->protocol_type = FrameControl::E_TYPE_DATA;
    wlan_frame.frame_control->flags = 0;
    *wlan_frame.duration = 0;
    wlan_frame.rescan();

    if (wlan_frame.frame_body == nullptr || wlan_frame.address1 == nullptr ||
        wlan_frame.address2 == nullptr)
    {
        return false;
    }

    const size_t hdr_len = static_cast<size_t>(
        wlan_frame.frame_body - reinterpret_cast<uint8_t*>(wlan_frame.frame_control));
    if (buffer_size() < hdr_len)
    {
        return false;
    }
    const size_t payload = buffer_size() - hdr_len;
    if (payload > WIFI_PAYLOAD_MAX)
    {
        return false;
    }
    wlan_frame.set_body_size(static_cast<uint16_t>(payload));
    data_frame_ready = true;
    return true;
}

bool Mpdu::rescan()
{
    bound_80211 = false;
    data_frame_ready = false;

    if (buffer_size() < WIFI_HDR_LEN)
    {
        return false;
    }

    wlan_frame.set_enable_fcs(false);
    wlan_frame.rescan();

    if (!validate_data_frame())
    {
        return false;
    }
    if (wlan_frame.frame_body == nullptr)
    {
        return false;
    }

    const size_t hdr_len = static_cast<size_t>(
        wlan_frame.frame_body - reinterpret_cast<uint8_t*>(wlan_frame.frame_control));
    if (buffer_size() < hdr_len)
    {
        return false;
    }
    const size_t payload = buffer_size() - hdr_len;
    if (payload > WIFI_RX_PAYLOAD_MAX)
    {
        return false;
    }
    wlan_frame.set_body_size(static_cast<uint16_t>(payload));

    if (!unpack_slots_from_header())
    {
        return false;
    }
    if (!validate_slot_payload_layout())
    {
        return false;
    }

    data_frame_ready = true;
    bound_80211 = true;
    return true;
}

bool Mpdu::validate_data_frame() const
{
    using namespace ieee_802_11;

    if (wlan_frame.frame_control == nullptr)
    {
        return false;
    }
    const auto type =
        wlan_frame.frame_control->protocol_type & FrameControl::E_TYPE_MASK;
    const auto mtype = type & FrameControl::E_MTYPE_MASK;
    if (mtype != FrameControl::E_MTYPE_DATA)
    {
        return false;
    }
    if (type != FrameControl::E_TYPE_DATA)
    {
        return false;
    }
    if (wlan_frame.frame_control->flags != 0)
    {
        return false;
    }
    return true;
}

void Mpdu::refresh_slot_cache_from_header() const
{
    if (wlan_frame.address1 == nullptr || wlan_frame.address2 == nullptr)
    {
        return;
    }
    unpack_slot_sizes_from_addrs(wlan_frame.address1->address, wlan_frame.address2->address,
                                 slot_cache_);
}

bool Mpdu::unpack_slots_from_header()
{
    if (wlan_frame.address1 == nullptr || wlan_frame.address2 == nullptr)
    {
        return false;
    }
    refresh_slot_cache_from_header();
    return true;
}

void Mpdu::pack_slots_to_header()
{
    if (wlan_frame.address1 == nullptr || wlan_frame.address2 == nullptr)
    {
        return;
    }
    pack_slot_sizes_to_addrs(wlan_frame.address1->address, wlan_frame.address2->address,
                             slot_cache_);
}

bool Mpdu::validate_slot_payload_layout() const
{
    if (wlan_frame.frame_body == nullptr)
    {
        return false;
    }
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        if (slot_cache_[i] > WIFI_RX_PAYLOAD_MAX)
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
    if (wlan_frame.address1 == nullptr || wlan_frame.address2 == nullptr)
    {
        return 0;
    }
    refresh_slot_cache_from_header();
    return slot_cache_[slot_index];
}

bfc::const_buffer_view Mpdu::get_slot_payload(uint8_t slot_index) const
{
    if (slot_index >= WIFI_PDU_SLOTS || wlan_frame.frame_body == nullptr)
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

    return bfc::const_buffer_view(wlan_frame.frame_body + off, slot_size);
}

bfc::buffer_view Mpdu::get_slot_payload(uint8_t slot_index)
{
    if (slot_index >= WIFI_PDU_SLOTS || wlan_frame.frame_body == nullptr)
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

    return bfc::buffer_view(wlan_frame.frame_body + off, slot_size);
}

bool Mpdu::is_valid_winject_frame() const
{
    if (wlan_frame.address3 == nullptr)
    {
        return false;
    }
    return addr3_bytes_prefix_match(wlan_frame.address3->address);
}

uint16_t Mpdu::get_domain() const
{
    if (wlan_frame.address3 == nullptr)
    {
        return 0;
    }
    return read_addr3_domain(wlan_frame.address3->address);
}

void Mpdu::set_domain(uint16_t domain)
{
    if (!init_data_frame() || wlan_frame.address3 == nullptr)
    {
        return;
    }
    write_addr3_domain(wlan_frame.address3->address, domain);
}

ieee_802_11::Frame& Mpdu::ieee()
{
    return this->wlan_frame;
}

const ieee_802_11::Frame& Mpdu::ieee() const
{
    return this->wlan_frame;
}

uint16_t next_tx_sequence()
{
    return next_tx_seq();
}

}  // namespace winject
