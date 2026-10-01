#include "radio/RxDemux.h"

#include "endpoint/Upstream.h"
#include "frames/LCHeader.h"
#include "frames/Mpdu.h"
#include "radio/RadioDefs.h"
#include "radio/RadioUpstreamEntry.h"

#include <array>

namespace winject
{

RxDemux::RxDemux(RadioUpstreamTable& table) : table_(table) {}

void RxDemux::set_domain(uint16_t domain)
{
    domain_ = domain;
}

uint64_t RxDemux::rx_drop_domain() const
{
    return rx_drop_domain_.load(std::memory_order_relaxed);
}

uint64_t RxDemux::rx_drop_bus() const
{
    return rx_drop_bus_.load(std::memory_order_relaxed);
}

void RxDemux::on_mpdu(bfcext::shared_sized_buffer mpdu_owned)
{
    struct Delivery
    {
        std::shared_ptr<Upstream> up;
        bfcext::shared_sized_buffer view;
    };
    std::array<Delivery, WIFI_PDU_SLOTS * 8> pending{};
    size_t pending_count = 0;

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
        if (!view.is_valid_winject_frame() || view.get_domain() != domain_)
        {
            rx_drop_domain_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::array<std::vector<size_t>, 256> rx_by_bus;
        const auto& entries = table_.entries();
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const uint8_t bus = entries[i].bus_rx;
            if (bus != 0 && entries[i].up != nullptr)
            {
                rx_by_bus[bus].push_back(i);
            }
        }

        for (int slot = 0; slot < WIFI_PDU_SLOTS; ++slot)
        {
            const uint16_t pdu_len = view.slot_payload_size(
                static_cast<uint8_t>(slot));
            if (pdu_len == 0)
            {
                continue;
            }
            const bfc::const_buffer_view pdu = view.get_slot_payload(
                static_cast<uint8_t>(slot));
            if (pdu.empty() || pdu.size() != pdu_len ||
                pdu.size() < LCHeader::k_len)
            {
                return;
            }
            const uint8_t bus = LCHeader::read_bus(
                reinterpret_cast<const uint8_t*>(pdu.data()));
            if (bus == 0)
            {
                continue;
            }
            const auto& targets = rx_by_bus[bus];
            if (targets.empty())
            {
                rx_drop_bus_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            for (size_t idx : targets)
            {
                RadioUpstreamEntry& s = table_.entries()[idx];
                const uint8_t* payload = nullptr;
                size_t plen = 0;
                if (!s.up->accept_air(
                        reinterpret_cast<const uint8_t*>(pdu.data()),
                        pdu.size(), &payload, &plen))
                {
                    continue;
                }
                const size_t payload_off =
                    static_cast<size_t>(payload - mpdu_bytes);
                if (pending_count >= pending.size())
                {
                    return;
                }
                pending[pending_count].up = s.up;
                pending[pending_count].view =
                    mpdu_owned.subview(payload_off, plen);
                ++pending_count;
            }
        }
    }

    for (size_t i = 0; i < pending_count; ++i)
    {
        pending[i].up->on_radio_rx(std::move(pending[i].view));
    }
}

}  // namespace winject
