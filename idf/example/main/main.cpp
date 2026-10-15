// Runs winject_l3::main with a config path. The manager blocks in its
// reactor, so it gets its own pthread (larger stack than app_main's).

#include "WinjectL3.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include <pthread.h>
#include <stdio.h>

static void* run(void*)
{
    char arg0[] = "winject-l3";
    char arg1[] = "/winject/winject.conf";
    char* argv[] = {arg0, arg1, nullptr};
    const int rc = winject_l3::main(2, argv);
    printf("winject_l3::main returned %d\n", rc);
    return nullptr;
}

extern "C" void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_t t;
    pthread_create(&t, &attr, run, nullptr);
}
