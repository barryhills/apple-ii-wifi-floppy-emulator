#pragma once
#include <stdbool.h>
#include <stdint.h>

// Disk II head stepper model, in quarter tracks. Portable C (host-tested by
// tools/host/stepper_test.c); the core-1 drive loop calls it from RAM.
//
// The four stepper phases are magnets 90 degrees apart on the rotor; one
// full rotor turn moves the head 8 quarter tracks (2 tracks). Phase p pulls
// the head to the quarter-track positions q with q mod 8 == 2p, so DOS 3.3's
// track t sits under phase 0 (t even) or phase 2 (t odd), and a seek steps
// through the phase between, one half track per phase change
// (tinyDiskII floppy_main.cpp, SDISK II sdisk2.c: one-hot phases move the
// head +-1 half track; Beneath Apple DOS ch. 6: two phases per track).
//
// Energized magnets are summed as vectors (the usual model for the Disk II
// stepper; it also gives the quarter tracks that two adjacent phases make,
// as in the WOZ TMAP's quarter-track positions). The sum of any set of the
// four unit vectors has components in {-1, 0, 1}, so it points to exactly
// one of the 8 quarter-track positions on the rotor, or is zero (no phase,
// or opposing phases cancel): no movement. The head then goes the short way
// to that position; a target exactly opposite (4 quarter tracks away) pulls
// straight through the axis and does not move it.
//
// The result for a given position and phase set is idempotent: calling it
// again with the same phases does not move the head further. So the drive
// loop can call it on every pass while the drive is enabled.

#define STEPPER_MAX_Q   159         // quarter tracks 0..159 (WOZ TMAP range)

static inline int stepper_next(int q, unsigned phases)
{
    int x = (int)(phases & 1) - (int)(phases >> 2 & 1);         // phase 0 (+x) vs phase 2 (-x)
    int y = (int)(phases >> 1 & 1) - (int)(phases >> 3 & 1);    // phase 1 (+y) vs phase 3 (-y)
    // Direction of (x, y) -> rotor position 0..7, none for (0, 0). Index
    // i = 3(y+1) + (x+1); positions for i = 0..7 packed as nibbles in an
    // immediate (a static const table would sit in flash, which core 1
    // must not read):  i: 0 1 2 3 4 5 6 7 8
    //                  a: 5 6 7 4 - 0 3 2 1
    int i = 3 * (y + 1) + (x + 1);
    if (i == 4)
        return q;
    int a = i == 8 ? 1 : (int)(0x230F4765u >> (4 * i) & 7);
    int d = (a - (q & 7)) & 7;          // 0..7 quarter tracks ahead on the rotor
    if (d == 4)
        return q;                       // directly opposite: no torque
    if (d > 4)
        d -= 8;                         // -3..3, the short way
    q += d;
    if (q < 0)
        q = 0;                          // track 0 stop
    if (q > STEPPER_MAX_Q)
        q = STEPPER_MAX_Q;
    return q;
}

// The drive loop's view: stepper_update() is called on every pass with the
// enable state, the phase lines and the time (1 us ticks, wrapping). A
// phase state moves the head only once it has held for STEPPER_SETTLE_US
// with the drive enabled. The head is a mass on a lead screw: it cannot
// follow a state that lasts microseconds, such as the moment between two
// softswitch accesses when software releases a two-phase (quarter-track)
// hold one phase at a time; an instant model would snap to the phase
// released last. Real step holds are milliseconds (DOS 3.3 steps every few
// ms), so the delay is insignificant. Returns true if the head moved.

#define STEPPER_SETTLE_US   100

typedef struct {
    int      q;             // head position, quarter tracks
    unsigned seen;          // phase state being timed
    uint32_t since;         // when it began (or the drive was enabled)
    bool     powered;       // enabled on the previous call
} stepper_t;

static inline bool stepper_update(stepper_t *s, bool enabled, unsigned phases, uint32_t now)
{
    if (!enabled) {
        s->powered = false;
        return false;
    }
    if (!s->powered || phases != s->seen) {
        s->powered = true;
        s->seen = phases;
        s->since = now;
        return false;
    }
    if (now - s->since < STEPPER_SETTLE_US)
        return false;
    int nq = stepper_next(s->q, phases);
    if (nq == s->q)
        return false;
    s->q = nq;
    return true;
}
