// a2drive — WiFi Disk II drive emulator firmware.
// Core 0: WiFi, HTTP, flash storage, OLED and buttons (this loop). Core 1:
// the drive itself (drive.c). Press Enter on the USB serial console for status.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "api.h"
#include "config.h"
#include "diag.h"
#include "drive.h"
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
    drive_led_power_on();           // GP16 LED on at once (before the 2 s USB wait)
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
    drive_init();
    // Setup mode (access point for entering WiFi settings): UP+DOWN held at
    // power-on, or no network configured. Nothing else (see net.c).
    bool keys = oled_setup_keys_held(), none = config_ssid()[0] == 0;
    if (keys || none)
        printf("setup mode: %s\n", keys ? "UP+DOWN held at power-on" : "no WiFi network configured");
    net_init(keys || none);
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
        drive_poll();
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
