#include "radio/WifiFcs.h"

namespace winject
{

namespace
{

constexpr uint32_t k_polynomial = 0xEDB88320u;

const uint32_t* crc32_table()
{
    static uint32_t table[256] = {};
    static bool initialized = false;
    if (!initialized)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1) ^ (k_polynomial & (0u - (crc & 1u)));
            }
            table[i] = crc;
        }
        initialized = true;
    }
    return table;
}

}  // namespace

uint32_t wifi_fcs_compute(const uint8_t* mpdu, size_t len)
{
    if (mpdu == nullptr)
    {
        return 0;
    }
    const uint32_t* table = crc32_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
    {
        crc = (crc >> 8) ^ table[(crc ^ mpdu[i]) & 0xFFu];
    }
    return ~crc;
}

bool wifi_fcs_matches(const uint8_t* frame, size_t len)
{
    if (frame == nullptr || len < 4)
    {
        return false;
    }
    const uint8_t* fcs = frame + len - 4;
    const uint32_t got = static_cast<uint32_t>(fcs[0]) |
                         (static_cast<uint32_t>(fcs[1]) << 8) |
                         (static_cast<uint32_t>(fcs[2]) << 16) |
                         (static_cast<uint32_t>(fcs[3]) << 24);
    return wifi_fcs_compute(frame, len - 4) == got;
}

void wifi_fcs_store(const uint8_t* mpdu, size_t len, uint8_t out[4])
{
    if (out == nullptr)
    {
        return;
    }
    const uint32_t crc = wifi_fcs_compute(mpdu, len);
    out[0] = static_cast<uint8_t>(crc);
    out[1] = static_cast<uint8_t>(crc >> 8);
    out[2] = static_cast<uint8_t>(crc >> 16);
    out[3] = static_cast<uint8_t>(crc >> 24);
}

}  // namespace winject
