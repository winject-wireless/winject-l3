#ifndef WINJECT_MANAGER_FRAMES_MPDU_H_
#define WINJECT_MANAGER_FRAMES_MPDU_H_

#include "radio/RadioDefs.h"

#include <stddef.h>
#include <stdint.h>

#include <bfc/buffer.hpp>

#include "frames/Frame.h"

namespace winject
{

class Mpdu
{
public:
    Mpdu() = default;
    Mpdu(uint8_t* data, size_t len);

    bool rescan();

    bool is_valid_winject_frame() const;
    uint16_t get_domain() const;
    void set_domain(uint16_t domain);


    void set_slot_payload(uint8_t slot_index, uint16_t slot_size);
    uint16_t slot_payload_size(uint8_t slot_index) const;

    bfc::buffer_view get_slot_payload(uint8_t slot_index);
    bfc::const_buffer_view get_slot_payload(uint8_t slot_index) const;

    ieee_802_11::Frame& ieee();
    const ieee_802_11::Frame& ieee() const;

private:
    bool validate_data_frame() const;
    bool init_data_frame();
    bool unpack_slots_from_header();
    void pack_slots_to_header();
    bool validate_slot_payload_layout() const;
    size_t slot_payload_bytes() const;
    bool slot_payload_offset(uint8_t slot_index, size_t* offset) const;
    void refresh_slot_cache_from_header() const;
    size_t buffer_size() const;
    size_t frame_body_bytes() const;

    mutable uint16_t slot_cache_[WIFI_PDU_SLOTS] = {};
    bool bound_80211_ = false;
    bool data_frame_ready_ = false;

    ieee_802_11::Frame ieee_{};
};

uint16_t next_tx_sequence();

}  // namespace winject

#endif  // WINJECT_MANAGER_FRAMES_MPDU_H_
