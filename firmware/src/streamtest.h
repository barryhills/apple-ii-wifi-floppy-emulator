#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Milestone 4 bench test: play one track of the mounted image on RDDATA
// (GP10) and capture it back on GP8 (WRDATA_3V) through a jumper, then check
// the capture against the track, bit for bit, across a revolution boundary.
// Non-blocking: start, then poll from the main loop until it reports.
// The player keeps running afterwards, so a logic analyzer can look at GP10.

// cells: 0 = the whole track; else play only its first `cells` cells as one
// revolution (exercises lengths that are not a multiple of 32).
int  streamtest_start(unsigned track, uint32_t cells, char *err, size_t errn);   // 0 or -1 + err
bool streamtest_poll(char *report, size_t n);                    // true when done
