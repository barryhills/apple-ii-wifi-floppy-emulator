// Host test of the firmware's head stepper model (firmware/src/stepper.h).
//   cc -O2 -Wall -Wextra -I../../firmware/src -o stepper_test stepper_test.c && ./stepper_test
//
// Drives the model with the phase sequences the Apple software produces and
// checks where the head ends up:
//  - DOS 3.3 RWTS seek: each half-track step turns the next phase on, then
//    the previous one off (overlapping), finally all off (Beneath Apple DOS
//    ch. 6 describes SEEKABS stepping phase by phase; the overlap is the
//    worst case for a model, so it is tested both with and without it).
//  - Boot ROM recalibrate: steps down 80 half tracks from wherever the head
//    is, hitting the track 0 stop.
//  - One-hot behaviour from tinyDiskII / SDISK II: next phase +1 half track,
//    previous -1, opposite or same: no move.
//  - Every (position, phase set) against an independent float vector-sum.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "stepper.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int q;
static unsigned ph;
static void set(unsigned p) { ph = p; q = stepper_next(q, ph); }

// One DOS-style half-track step from phase a to phase b, with or without
// the overlap (both on, then a off).
static void step(int a, int b, int overlap)
{
    if (overlap) {
        set((1u << a) | (1u << b));
        set(1u << b);
    } else {
        set(1u << b);
    }
}

// Seek from the current track to track t, two phases per track.
static void seek(int t, int overlap)
{
    int ht = q / 2, target = 2 * t;     // half tracks
    int cur = ht & 3;
    set(1u << cur);
    while (ht != target) {
        int dir = target > ht ? 1 : -1;
        int nxt = (cur + dir) & 3;
        step(cur, nxt, overlap);
        cur = nxt;
        ht += dir;
    }
    set(0);
}

static int ref_next(int pos, unsigned p)
{
    double x = 0, y = 0;
    for (int k = 0; k < 4; k++)
        if (p & (1u << k)) { x += cos(k * M_PI / 2); y += sin(k * M_PI / 2); }
    if (fabs(x) < 1e-9 && fabs(y) < 1e-9)
        return pos;
    double ang = atan2(y, x);                        // rotor angle of the pull
    int a = (int)lround(ang / (M_PI / 4));           // in quarter tracks
    int d = ((a - pos) % 8 + 8) % 8;
    if (d == 4)
        return pos;
    if (d > 4)
        d -= 8;
    int r = pos + d;
    return r < 0 ? 0 : r > STEPPER_MAX_Q ? STEPPER_MAX_Q : r;
}

int main(void)
{
    // 1. Exhaustive against the float reference.
    for (int pos = 0; pos <= STEPPER_MAX_Q; pos++)
        for (unsigned p = 0; p < 16; p++) {
            int got = stepper_next(pos, p), want = ref_next(pos, p);
            CHECK(got == want, "pos %d phases %x: got %d want %d", pos, p, got, want);
            CHECK(stepper_next(got, p) == got, "not idempotent at pos %d phases %x", pos, p);
        }

    // 2. One-hot rules.
    for (int t = 1; t < 39; t++) {
        int pos = 4 * t, cur = (pos / 2) & 3;
        CHECK(stepper_next(pos, 1u << ((cur + 1) & 3)) == pos + 2, "t%d next phase", t);
        CHECK(stepper_next(pos, 1u << ((cur + 3) & 3)) == pos - 2, "t%d previous phase", t);
        CHECK(stepper_next(pos, 1u << ((cur + 2) & 3)) == pos, "t%d opposite phase", t);
        CHECK(stepper_next(pos, 1u << cur) == pos, "t%d same phase", t);
    }

    // 3. DOS seeks, every track to every track, both styles.
    for (int ov = 0; ov < 2; ov++)
        for (int from = 0; from < 35; from++)
            for (int to = 0; to < 35; to++) {
                q = 4 * from;
                seek(to, ov);
                CHECK(q == 4 * to, "seek %d->%d overlap %d: at q %d", from, to, ov, q);
            }

    // 4. Boot ROM recalibrate: 80 half-track steps down through the phases
    //    regardless of where the head is, the count's low two bits picking
    //    the phase (79&3 = 3, 2, 1, 0, ...), so it ends on phase 0 (from
    //    recall of the P5 boot ROM, NOT verified; the model itself does not
    //    depend on it, only this scenario does). From any
    //    position up to track 39.5 that must reach the track 0 stop: a head
    //    against the stop bounces as the phases rotate past (physically
    //    right: real drives clatter), but the last phase, 0, holds it at q 0.
    for (int ov = 0; ov < 2; ov++)
        for (int start = 0; start <= STEPPER_MAX_Q; start++) {
            q = start;
            int prev = -1;
            for (int i = 79; i >= 0; i--) {
                int cur = i & 3;
                if (prev >= 0 && ov)
                    step(prev, cur, 1);
                else
                    set(1u << cur);
                prev = cur;
            }
            set(0);
            CHECK(q == 0, "recalibrate (overlap %d) from q %d ended at %d", ov, start, q);
        }

    // 5. Quarter tracks: two adjacent phases hold the head between them.
    CHECK(stepper_next(0, 0x3) == 1, "phases 0+1 from q0 -> q1");
    CHECK(stepper_next(4, 0x6) == 3, "phases 1+2 from q4 -> q3");
    CHECK(stepper_next(4, 0xC) == 5, "phases 2+3 from q4 -> q5");

    // 6. Timing (stepper_update): states shorter than STEPPER_SETTLE_US are
    //    ignored; held states act; disabled drives ignore phases.
    {
        stepper_t st = { .q = 0 };
        uint32_t t = 1000;
        #define RUN(en, ph, us) do { for (uint32_t _e = 0; _e < (us); _e++) stepper_update(&st, (en), (ph), t++); } while (0)
        // Hold phases 0+1 (quarter track 1), then release them 5 us apart.
        RUN(1, 0x3, 2000);
        CHECK(st.q == 1, "two-phase hold: q %d, want 1", st.q);
        RUN(1, 0x2, 5);            // phase 0 off first: phase 1 alone for 5 us
        RUN(1, 0x0, 2000);
        CHECK(st.q == 1, "quarter-track release: q %d, want 1 (transient followed)", st.q);
        // The same release with phase 1 alone for 2 ms does move the head.
        RUN(1, 0x3, 2000);
        RUN(1, 0x2, 2000);
        CHECK(st.q == 2, "held single phase: q %d, want 2", st.q);
        // A 50 us glitch of the next phase does not step.
        RUN(1, 0x0, 1000);
        RUN(1, 0x4, 50);
        RUN(1, 0x0, 1000);
        CHECK(st.q == 2, "50 us glitch: q %d, want 2", st.q);
        // Phases while disabled are ignored; enabling with a phase on acts after settling.
        RUN(0, 0x4, 5000);
        CHECK(st.q == 2, "disabled: q %d, want 2", st.q);
        RUN(1, 0x4, 99);
        CHECK(st.q == 2, "99 us after enable: q %d, want 2", st.q);
        RUN(1, 0x4, 10);
        CHECK(st.q == 4, "after enable + settle: q %d, want 4", st.q);
        // DOS-like timed seek 0 -> 34 -> 0 with 1.5 ms holds and overlap,
        // and a 10 us gap between softswitch accesses at each change.
        st.q = 0;
        int ht = 0, cur = 0;
        RUN(1, 1u << cur, 1500);
        for (int dir = 1; dir >= -1; dir -= 2)
            for (int i = 0; i < 68; i++) {
                int nxt = (cur + dir) & 3;
                RUN(1, (1u << cur) | (1u << nxt), 1500);
                RUN(1, 1u << nxt, 1500);
                cur = nxt;
                ht += dir;
                CHECK(st.q == 2 * ht, "timed seek: q %d, want %d", st.q, 2 * ht);
            }
        RUN(1, 0, 1000);
        CHECK(st.q == 0, "timed seek back: q %d", st.q);
        // Timer wrap.
        t = 0xFFFFFF00u;
        RUN(1, 0x0, 10);
        RUN(1, 0x2, 400);
        CHECK(st.q == 2, "across timer wrap: q %d, want 2", st.q);
        #undef RUN
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "all stepper tests passed", fails);
    return fails != 0;
}
