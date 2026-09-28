#include "radio/RadioUpstreamEntry.h"
#include "radio/RxDemux.h"

#include "endpoint/Upstream.h"
#include "frames/Mpdu.h"
#include "radio/RadioDefs.h"

#include <string.h>

namespace winject
{

RxDemux::RxDemux(RadioUpstreamTable& table) : table_(table) {}

void RxDemux::on_mpdu(bfcext::shared_sized_buffer mpdu_owned)
{
    std::lock_guard<std::mutex> lock(table_.mutex());
    const size_t len = mpdu_owned.size();
    if (mpdu_owned.empty() || len > WIFI_RADIO_INJECT_MAX)
    {
        return;
    }
    uint8_t* mpdu_bytes = reinterpret_cast<uint8_t*>(mpdu_owned.data());
    Mpdu view(mpdu_bytes, len);
    if (!view.rescan())
    {
        return;
    }
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        const uint16_t pdu_len = view.slot_payload_size(
            static_cast<uint8_t>(i));
        if (pdu_len == 0)
        {
            continue;
        }
        const bfc::const_buffer_view pdu = view.get_slot_payload(
            static_cast<uint8_t>(i));
        if (pdu.empty() || pdu.size() != pdu_len)
        {
            return;
        }

        if (static_cast<size_t>(i) >= table_.entries().size())
        {
            continue;
        }
        RadioUpstreamEntry& s = table_.entries()[static_cast<size_t>(i)];
        if (s.up == nullptr || s.bus_rx == 0)
        {
            continue;
        }
        const uint8_t* payload = nullptr;
        size_t plen = 0;
        if (!s.up->accept_air(reinterpret_cast<const uint8_t*>(pdu.data()),
                              pdu.size(), &payload, &plen))
        {
            continue;
        }
        const size_t payload_off = static_cast<size_t>(payload - mpdu_bytes);
        s.up->on_radio_rx(mpdu_owned.subview(payload_off, plen));
    }
}

}  // namespace winject
