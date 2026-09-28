/*
 * Copyright (C) 2023 Prinz Rainer Buyo <mynameisrainer@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "frames/Frame.h"

#include <cstdio>

namespace winject
{
namespace ieee_802_11
{

uint64_t MacAddress::get()
{
    uint64_t rv = 0;

    rv |= uint64_t(address[0]) << (40);
    rv |= uint64_t(address[1]) << (32);
    rv |= uint64_t(address[2]) << (24);
    rv |= uint64_t(address[3]) << (16);
    rv |= uint64_t(address[4]) << (8);
    rv |= uint64_t(address[5]) << (0);

    return rv;
}

void MacAddress::set(uint64_t value)
{
    address[0] = (value >> 40) & 0xFF;
    address[1] = (value >> 32) & 0xFF;
    address[2] = (value >> 24) & 0xFF;
    address[3] = (value >> 16) & 0xFF;
    address[4] = (value >> 8) & 0xFF;
    address[5] = (value >> 0) & 0xFF;
}

uint8_t SeqControl::get_fragment_num()
{
    return le16toh(frag_sec) & 0xF;
}

void SeqControl::set_fragment_num(uint8_t value)
{
    frag_sec = le16toh(frag_sec) & 0xFFF0;
    frag_sec = le16toh(frag_sec) | value;
}

uint16_t SeqControl::get_seq_num()
{
    return le16toh(frag_sec) >> 4;
}

void SeqControl::set_seq_num(uint16_t value)
{
    frag_sec = le16toh(frag_sec) & 0xF;
    frag_sec = le16toh(frag_sec) | (value << 4);
}

Frame::Frame(uint8_t* buffer, uint8_t* last) : last(last), is_enabled_fcs(true)
{
    if (!buffer)
    {
        return;
    }

    frame_control = (FrameControl*)buffer;
    duration = (LEU16UA*)(buffer + sizeof(FrameControl));
}

void Frame::reset()
{
    address1 = nullptr;
    address2 = nullptr;
    address3 = nullptr;
    seq_ctl = nullptr;
    address4 = nullptr;
    frame_body = nullptr;
    fcs = (LEU32UA*)(last - sizeof(*fcs));
}

void Frame::rescan()
{
    reset();

    auto type = frame_control->protocol_type & FrameControl::E_TYPE_MASK;
    auto mtype = type & FrameControl::E_MTYPE_MASK;
    auto stype = type & FrameControl::E_TYPE_MASK;
    if (mtype == FrameControl::E_MTYPE_DATA)
    {
        address1 = (MacAddress*)((uint8_t*)duration + sizeof(uint16_t));
        address2 = (MacAddress*)((uint8_t*)address1 + sizeof(MacAddress));
        address3 = (MacAddress*)((uint8_t*)address2 + sizeof(MacAddress));
        seq_ctl = (SeqControl*)((uint8_t*)address3 + sizeof(MacAddress));

        if ((frame_control->flags & FrameControl::E_FLAGS_TO_DS) &&
            (frame_control->flags & FrameControl::E_FLAGS_FROM_DS))
        {
            address4 = (MacAddress*)((uint8_t*)seq_ctl + sizeof(SeqControl));
            frame_body = (uint8_t*)address4 + sizeof(MacAddress);
        }
        else
        {
            address4 = nullptr;
            frame_body = (uint8_t*)seq_ctl + sizeof(SeqControl);
        }
    }
    else if (stype == FrameControl::E_TYPE_RTS)
    {
        address1 = (MacAddress*)((uint8_t*)duration + sizeof(*duration));
        address2 = (MacAddress*)((uint8_t*)address1 + sizeof(*address1));
        fcs = (LEU32UA*)((uint8_t*)address2 + sizeof(*address2));
    }
}

uint8_t* Frame::end()
{
    if (is_enabled_fcs)
    {
        return (uint8_t*)fcs + sizeof(*fcs);
    }
    else
    {
        return (uint8_t*)fcs;
    }
}

size_t Frame::size()
{
    return end() - (uint8_t*)frame_control;
}

size_t Frame::frame_body_size()
{
    return end() - (uint8_t*)frame_body;
}

void Frame::set_enable_fcs(bool val)
{
    is_enabled_fcs = val;
}

void Frame::set_body_size(uint16_t size)
{
    fcs = (LEU32UA*)(frame_body + size);
}

std::string to_string(Frame& frame80211)
{
    char buffer[1024];
    char* current = buffer;
    auto rem = [buffer, &current]()
    {
        return uintptr_t(buffer + sizeof(buffer) - current);
    };

    auto check = [&current](int result) mutable
    {
        if (result < 0)
        {
            return;
        }
        current += result;
    };

    check(snprintf(current, rem(), "802.11:\n"));
    check(snprintf(current, rem(), "  frame_control:\n"));
    check(snprintf(current, rem(), "    protocol_type: %p\n",
                   (void*)(uintptr_t)frame80211.frame_control->protocol_type));
    check(snprintf(current, rem(), "    flags: %p\n",
                   (void*)(uintptr_t)frame80211.frame_control->flags));
    check(snprintf(current, rem(), "  duration: %p\n",
                   (void*)(uintptr_t)*frame80211.duration));
    if (frame80211.address1)
    {
        check(snprintf(current, rem(), "  address1: %p\n",
                       (void*)frame80211.address1->get()));
    }
    if (frame80211.address2)
    {
        check(snprintf(current, rem(), "  address2: %p\n",
                       (void*)frame80211.address2->get()));
    }
    if (frame80211.address3)
    {
        check(snprintf(current, rem(), "  address3: %p\n",
                       (void*)frame80211.address3->get()));
    }
    if (frame80211.address3)
    {
        check(snprintf(current, rem(), "  seq_ctl:\n"));
        check(snprintf(current, rem(), "    seq: %d\n",
                       frame80211.seq_ctl->get_seq_num()));
        check(snprintf(current, rem(), "    frag: %d\n",
                       frame80211.seq_ctl->get_fragment_num()));
    }
    return buffer;
}

}  // namespace ieee_802_11
}  // namespace winject
