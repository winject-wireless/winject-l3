#ifndef WINJECT_MANAGER_CONSOLE_CHANNEL_INFO_VIEW_H_
#define WINJECT_MANAGER_CONSOLE_CHANNEL_INFO_VIEW_H_

#include <stddef.h>
#include <stdint.h>

namespace winject
{

struct ChannelInfoView
{
    bool flow_valid = false;
    uint8_t tx_queue_size = 0;
    uint8_t tx_queue_capacity = 0;
    char flow_t[32] = "-";
    bool air_valid = false;
    int8_t rssi = 0;
    int8_t snr = 0;
    char rssi_t[32] = "-";
    uint64_t tx_byte = 0;  // sum of radio on-air bytes (FEC shards + seq)
    uint64_t rx_byte = 0;
    uint64_t tx_pkt = 0;
    uint64_t rx_pkt = 0;
    uint64_t rx_pkt_loss = 0;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_CHANNEL_INFO_VIEW_H_
