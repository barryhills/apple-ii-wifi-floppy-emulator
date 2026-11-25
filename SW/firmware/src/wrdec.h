#pragma once
#include <stdint.h>

// Disk II write decoding: interval between WRDATA transitions -> bit cells.
// Portable C, no tables (core 1 runs it from RAM); host-tested by
// tools/host/wrdec_test.c.
//
// The controller writes a flux transition (WRDATA toggles) for each 1 bit
// and none for a 0 (tinyDiskII; Beneath Apple DOS ch. 3), so an interval of
// n cells is n-1 zeros and a 1. The //e's write cell is not 4 us: its shift
// rate comes from the 2 MHz Q3 clock (~3.91 us per bit, 8 Q3 cycles; vibr77's
// README gives ~3.958 us). So the cell length is estimated from the data
// (every edge resyncs, as on the real drive): it starts at WRDEC_CELL0 and
// follows the intervals of 1-3 cells, the only ones valid 6-and-2 data has
// (at most two zeros in a row, Beneath Apple DOS ch. 3).
//
// Units: ticks of the capture clock; cell lengths are kept x16 (fixed point).

#define WRDEC_FRAC 16u

typedef struct {
    uint32_t cell16;        // cell length in ticks x16
    uint32_t lo16, hi16;    // clamp for the estimate
} wrdec_t;

// ticks_per_us: the capture clock. Starts at 3.93 us, clamps to 3.6-4.3 us.
static inline void wrdec_init(wrdec_t *d, uint32_t ticks_per_us)
{
    d->cell16 = ticks_per_us * WRDEC_FRAC * 393u / 100u;
    d->lo16 = ticks_per_us * WRDEC_FRAC * 360u / 100u;
    d->hi16 = ticks_per_us * WRDEC_FRAC * 430u / 100u;
}

// Cells in this interval (>= 1), updating the estimate.
static inline uint32_t wrdec_cells(wrdec_t *d, uint32_t ticks)
{
    if (ticks > 0x0FFFFFFFu)
        ticks = 0x0FFFFFFFu;                    // keep ticks x16 in 32 bits
    uint32_t t16 = ticks * WRDEC_FRAC;
    uint32_t n = (t16 + d->cell16 / 2) / d->cell16;
    if (n == 0)
        n = 1;                                  // a glitch: still an edge
    if (n <= 3) {
        int32_t err = (int32_t)(t16 / n) - (int32_t)d->cell16;
        int32_t c = (int32_t)d->cell16 + err / 8;   // follow slowly
        if (c < (int32_t)d->lo16) c = (int32_t)d->lo16;
        if (c > (int32_t)d->hi16) c = (int32_t)d->hi16;
        d->cell16 = (uint32_t)c;
    }
    return n;
}
