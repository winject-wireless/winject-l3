#ifndef WINJECT_MANAGER_RADIO_RX_DEMUX_H_
#define WINJECT_MANAGER_RADIO_RX_DEMUX_H_

#include "radio/RadioUpstreamTable.h"

#include <bfcext/shared_sized_buffer.hpp>

namespace winject
{

class RxDemux
{
public:
    explicit RxDemux(RadioUpstreamTable& table);

    void on_mpdu(bfcext::shared_sized_buffer mpdu);

private:
    RadioUpstreamTable& table_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RADIO_RX_DEMUX_H_
