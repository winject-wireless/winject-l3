// Runs winject::L3::run with a Config filled here. run() blocks, so it gets
// its own pthread: that thread becomes l3's callback thread. l3's
// reactor and the upstream reactor are threads run() starts; ThreadSetup sets
// their stack and name before each one.

#include "Config.h"
#include "WinjectL3.h"
#include "esp_event.h"
#include "esp_pthread.h"
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
    cfg.radio_device.fcs = winject::core::RadioFcsConfig::signal;
    cfg.radio_device.mplane = ipv4_port("192.168.253.11", 22);
    cfg.radio_device.dplane = ipv4_port("192.168.253.11", 9000);

    winject::core::UpstreamConfig to_peer;
    to_peer.index = 0;
    to_peer.mode = winject::core::UpstreamMode::udp_server;
    to_peer.bus_tx = 0xb2;
    to_peer.bus_rx = 0xa1;
    to_peer.scheduler_budget = 65536;
    to_peer.endpoint.rx = "127.0.0.1:29000";

    winject::core::UpstreamConfig from_peer;
    from_peer.index = 1;
    from_peer.mode = winject::core::UpstreamMode::udp_client;
    from_peer.bus_tx = 0xc3;
    from_peer.bus_rx = 0xd4;
    from_peer.scheduler_budget = 65536;
    from_peer.endpoint.tx = "127.0.0.1:9001";

    cfg.upstreams.push_back(std::move(to_peer));
    cfg.upstreams.push_back(std::move(from_peer));
    return cfg;
}

void set_thread_cfg(winject::L3::Thread thread)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    if (thread == winject::L3::Thread::l3)
    {
        // l3's reactor: radio m-plane, TX timers, handle calls.
        cfg.stack_size = 16 * 1024;
        cfg.thread_name = "l3_reactor";
    }
    else
    {
        cfg.thread_name = "l3_upstream";
    }
    esp_pthread_set_cfg(&cfg);
}

void* run(void*)
{
    const winject::Config cfg = make_config();
    // No UDP console here: started is empty.
    const int rc = winject::L3::run(cfg, {}, set_thread_cfg);
    printf("winject::L3::run returned %d\n", rc);
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
