// Runs winject_l3::main with a Config filled here. The manager blocks in
// its reactor, so it gets its own pthread (larger stack than app_main's).

#include "Config.h"
#include "WinjectL3.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <utility>

namespace
{

sockaddr_in ipv4_port(const char* host, uint16_t port)
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, host, &addr.sin_addr);
    addr.sin_port = htons(port);
    return addr;
}

winject::Config make_config()
{
    // Same shape as configuration/winject-tests/esp32/bw_a.cfg. Edit this
    // struct for the board; there is no config file and no argv.
    winject::Config cfg;
    cfg.channel = 1;
    cfg.modulation = "OFDM_24M";
    cfg.power_dbm = 20;
    cfg.domain = 0x1234;
    cfg.stats_sec = 1;
    cfg.radio_device.fcs = winject::RadioFcsConfig::signal;
    cfg.radio_device.mplane = ipv4_port("192.168.253.11", 22);
    cfg.radio_device.dplane = ipv4_port("192.168.253.11", 9000);

    winject::UpstreamConfig to_peer;
    to_peer.index = 0;
    to_peer.mode = winject::UpstreamMode::udp_server;
    to_peer.bus_tx = 0xb2;
    to_peer.bus_rx = 0xa1;
    to_peer.scheduler_budget = 65536;
    to_peer.endpoint.rx = "127.0.0.1:29000";

    winject::UpstreamConfig from_peer;
    from_peer.index = 1;
    from_peer.mode = winject::UpstreamMode::udp_client;
    from_peer.bus_tx = 0xc3;
    from_peer.bus_rx = 0xd4;
    from_peer.scheduler_budget = 65536;
    from_peer.endpoint.tx = "127.0.0.1:9001";

    cfg.upstreams.push_back(std::move(to_peer));
    cfg.upstreams.push_back(std::move(from_peer));
    return cfg;
}

void* run(void*)
{
    const winject::Config cfg = make_config();
    const int rc = winject_l3::main(cfg);
    printf("winject_l3::main returned %d\n", rc);
    return nullptr;
}

}  // namespace

extern "C" void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_t thread;
    pthread_create(&thread, &attr, run, nullptr);
}
