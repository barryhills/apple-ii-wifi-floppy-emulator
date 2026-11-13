// Milestone 0: blink the Pico 2 W onboard LED (driven through the CYW43 chip,
// not a GPIO) and print a counter on USB CDC serial.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"

int main(void)
{
    stdio_init_all();

    if (cyw43_arch_init()) {
        // No LED without the CYW43; report on serial forever.
        for (;;) {
            printf("cyw43_arch_init failed\n");
            sleep_ms(1000);
        }
    }

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof id);

    for (unsigned n = 0;; n++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, n & 1);
        printf("blink %u  board %s  clk_sys %lu Hz\n", n, id,
               (unsigned long)clock_get_hz(clk_sys));
        sleep_ms(500);
    }
}
