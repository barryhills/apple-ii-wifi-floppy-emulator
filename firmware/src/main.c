// a2drive — WiFi Disk II drive emulator firmware.
// Milestone 2: WiFi, mDNS, HTTP status, and one .dsk image stored in flash
// (upload/download). Press Enter on the USB serial console for status.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "api.h"
#include "net.h"
#include "store.h"
#include "status.h"

static void print_status(void)
{
    static char buf[STATUS_JSON_MAX];
    cyw43_arch_lwip_begin();
    status_json(buf, sizeof buf);
    cyw43_arch_lwip_end();
    printf("net: %s\n%s", net_state_str(), buf);
}

int main(void)
{
    stdio_init_all();

    if (cyw43_arch_init()) {
        for (;;) {
            printf("cyw43_arch_init failed\n");
            sleep_ms(1000);
        }
    }
    printf("\na2drive %s starting\n", A2DRIVE_VERSION);
    printf("sha256 self-test: %s\n", sha256_selftest() ? "OK" : "FAILED");
    store_init();
    net_init();

    // LED heartbeat from the main loop (100 ms ticks): 5 Hz while connecting
    // to WiFi, 1 Hz once up. A steady LED, on or off, means the loop stopped.
    for (unsigned tick = 0;; tick++) {
        net_poll();
        api_poll();
        bool led = net_is_up() ? (tick % 10) < 5 : (tick & 1);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);

        int ch = getchar_timeout_us(0);
        if (ch == '\r' || ch == '\n')
            print_status();

        sleep_ms(100);
    }
}
