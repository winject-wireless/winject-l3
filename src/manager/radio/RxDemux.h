#ifndef WINJECT_MANAGER_RADIO_RX_DEMUX_H_
#define WINJECT_MANAGER_RADIO_RX_DEMUX_H_

#include "radio/RadioUpstreamTable.h"

#include <atomic>
#include <bfcext/shared_sized_buffer.hpp>
#include <stdint.h>
#include <vector>

namespace winject
{

class RxDemux
{
public:
    explicit RxDemux(RadioUpstreamTable& table);

    void set_domain(uint16_t domain);
    uint64_t rx_drop_domain() const;
    uint64_t rx_drop_bus() const;

    void on_mpdu(bfcext::shared_sized_buffer mpdu);

private:
    RadioUpstreamTable& table_;
    uint16_t domain_ = 0;
    std::atomic<uint64_t> rx_drop_domain_{0};
    std::atomic<uint64_t> rx_drop_bus_{0};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RX_DEMUX_H_
