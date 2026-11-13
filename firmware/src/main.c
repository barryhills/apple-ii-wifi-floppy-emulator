// a2drive — WiFi Disk II drive emulator firmware.
// Milestone 1: WiFi + mDNS + HTTP status. Press Enter on the USB serial
// console to print the current status.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "net.h"
#include "status.h"

static void print_status(void)
{
    char buf[STATUS_JSON_MAX];
    status_json(buf, sizeof buf);
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
    net_init();

    // LED: fast blink while connecting, steady on when up.
    for (unsigned tick = 0;; tick++) {
        net_poll();
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, net_is_up() || (tick & 1));

        int ch = getchar_timeout_us(0);
        if (ch == '\r' || ch == '\n')
            print_status();

        sleep_ms(100);
    }
}
