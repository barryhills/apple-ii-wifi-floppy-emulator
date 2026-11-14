#pragma once
#include <stddef.h>
#include <stdint.h>

// Crash and hang diagnostics.
//  - Hard faults (including stack overflow, caught by the M33's MSPLIM via
//    PICO_USE_STACK_GUARDS) record PC/LR/CFSR in watchdog scratch 0-3, which
//    survive a watchdog reset, then reboot.
//  - A 4 s hardware watchdog reboots the chip if the main loop stops.
//  - The stack is painted at boot so its lowest point can be measured.

void        diag_paint_stack(void);     // first thing in main()
void        diag_init(void);            // after stdio: read last reset reason
void        diag_watchdog_start(void);  // once boot work is done
void        diag_feed(void);            // every main-loop pass
const char *diag_last_reset(void);
uint32_t    diag_stack_free(void);      // bytes never touched since boot
