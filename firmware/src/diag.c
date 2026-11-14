#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include "diag.h"

#define DIAG_MAGIC      0xA2D1A600u     // low byte: reason
#define REASON_FAULT    1
#define REASON_SETUP    2       // reboot into WiFi setup mode (net.c)
#define WATCHDOG_MS     4000
#define PAINT           0xA5A5A5A5u

#define CFSR  (*(volatile uint32_t *)0xE000ED28)   // Configurable Fault Status
#define HFSR  (*(volatile uint32_t *)0xE000ED2C)   // HardFault Status
#define CFSR_STKOF      (1u << 20)                  // UsageFault: stack overflow (MSPLIM)

extern uint32_t __StackBottom;          // linker: lowest address of core 0's stack

static char last_reset[128];
static bool setup_requested;

// ---------------------------------------------------------------- hard fault

void diag_hardfault_c(uint32_t *frame, uint32_t exc_return);

// Replaces the SDK's weak isr_hardfault (crt0.S). Naked: the fault may be a
// stack overflow, so lift MSPLIM before anything touches the stack.
void __attribute__((naked, used)) isr_hardfault(void)
{
    __asm volatile(
        "movs r0, #0          \n"
        "msr  msplim, r0      \n"
        "tst  lr, #4          \n"
        "ite  eq              \n"
        "mrseq r0, msp        \n"
        "mrsne r0, psp        \n"
        "mov  r1, lr          \n"
        "b    diag_hardfault_c\n");
}

void __attribute__((used, noreturn)) diag_hardfault_c(uint32_t *frame, uint32_t exc_return)
{
    (void)exc_return;
    volatile uint32_t *s = watchdog_hw->scratch;
    s[0] = DIAG_MAGIC | REASON_FAULT;
    s[1] = frame[6];        // stacked PC (garbage if stacking itself overflowed)
    s[2] = frame[5];        // stacked LR
    s[3] = CFSR;
    (void)HFSR;
    watchdog_reboot(0, 0, 0);
    for (;;)
        ;
}

// ---------------------------------------------------------------- stack

void diag_paint_stack(void)
{
    uint32_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    // Interrupts may push below sp while we paint, but they finish before we
    // continue, so painting their old frames is harmless.
    for (uint32_t *p = &__StackBottom; (uint32_t)p < sp - 64; p++)
        *p = PAINT;
}

uint32_t diag_stack_free(void)
{
    const uint32_t *p = &__StackBottom;
    while (*p == PAINT)
        p++;
    return (uint32_t)((const uint8_t *)p - (const uint8_t *)&__StackBottom);
}

// ---------------------------------------------------------------- reset reason

void diag_init(void)
{
    volatile uint32_t *s = watchdog_hw->scratch;
    if (s[0] == (DIAG_MAGIC | REASON_FAULT)) {
        snprintf(last_reset, sizeof last_reset,
                 "crash: hard fault at pc=%08lx lr=%08lx cfsr=%08lx%s",
                 (unsigned long)s[1], (unsigned long)s[2], (unsigned long)s[3],
                 (s[3] & CFSR_STKOF) ? " (stack overflow)" : "");
    } else if (s[0] == (DIAG_MAGIC | REASON_SETUP)) {
        setup_requested = true;
        snprintf(last_reset, sizeof last_reset, "reboot into WiFi setup (no network joined)");
    } else if (watchdog_enable_caused_reboot()) {
        snprintf(last_reset, sizeof last_reset, "hang: watchdog reset (main loop stopped)");
    } else if (watchdog_caused_reboot()) {
        snprintf(last_reset, sizeof last_reset, "reboot");
    } else {
        snprintf(last_reset, sizeof last_reset, "power-on or reset button");
    }
    s[0] = s[1] = s[2] = s[3] = 0;
    printf("last reset: %s\n", last_reset);
}

void diag_watchdog_start(void)
{
    watchdog_enable(WATCHDOG_MS, true);     // paused while a debugger halts us
}

void diag_feed(void)
{
    watchdog_update();
}

void diag_reboot_into_setup(void)
{
    watchdog_hw->scratch[0] = DIAG_MAGIC | REASON_SETUP;
    watchdog_reboot(0, 0, 0);
    for (;;)
        ;
}

bool diag_setup_requested(void)
{
    return setup_requested;
}

const char *diag_last_reset(void)
{
    return last_reset;
}
