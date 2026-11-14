// a2drive — WiFi Disk II drive emulator firmware.
// Milestone 2: WiFi, mDNS, HTTP status, and one .dsk image stored in flash
// (upload/download). Press Enter on the USB serial console for status.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "api.h"
#include "config.h"
#include "diag.h"
#include "net.h"
#include "oled.h"
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
    diag_paint_stack();
    stdio_init_all();

    if (cyw43_arch_init()) {
        for (;;) {
            printf("cyw43_arch_init failed\n");
            sleep_ms(1000);
        }
    }
    printf("\na2drive %s starting\n", A2DRIVE_VERSION);
    diag_init();
    printf("sha256 self-test: %s\n", sha256_selftest() ? "OK" : "FAILED");
    oled_init();
    store_init();
    config_load();
    net_init();
    diag_watchdog_start();

    // LED: solid on from power-on until joined to a WiFi network (and again
    // whenever the link drops); 1 Hz blink while joined. Once joined, a
    // steady LED means the main loop stopped. It can't light before
    // cyw43_arch_init(): the LED sits on the WiFi chip, so only send changes.
    int led = -1;
    for (;;) {
        diag_feed();
        net_poll();
        bool busy = api_poll();
        oled_poll();
        uint32_t ds = (uint32_t)(time_us_64() / 100000);       // 100 ms units
        int want = net_is_up() ? (ds % 10) < 5 : 1;
        if (want != led) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, want);
            led = want;
        }

        int ch = getchar_timeout_us(0);
        if (ch == '\r' || ch == '\n')
            print_status();

        if (!busy)
            sleep_ms(10);
    }
}
