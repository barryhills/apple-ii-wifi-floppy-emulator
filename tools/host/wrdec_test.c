// Host test of the write decoder (firmware/src/wrdec.h).
//   cc -O2 -Wall -Wextra -I../../firmware/src -o wrdec_test wrdec_test.c -lm && ./wrdec_test
// Builds DOS-like write streams (self-syncs FF+00, then random valid 6-and-2
// nibbles), turns them into WRDATA edge times at a given write cell with
// random edge jitter, quantizes to capture ticks (75 per us, as on the
// Pico), and checks the decoder recovers every bit after the first edge.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "wrdec.h"

static int fails;

static const unsigned char nib[64] = {
    0x96,0x97,0x9a,0x9b,0x9d,0x9e,0x9f,0xa6,0xa7,0xab,0xac,0xad,0xae,0xaf,0xb2,0xb3,
    0xb4,0xb5,0xb6,0xb7,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xcb,0xcd,0xce,0xcf,0xd3,
    0xd6,0xd7,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,0xe5,0xe6,0xe7,0xe9,0xea,0xeb,0xec,
    0xed,0xee,0xef,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff };

#define MAXB 60000
static unsigned char bits[MAXB], got[MAXB];

static void run(double cell_us, double jitter_us, int nbytes, unsigned seed)
{
    srand(seed);
    int n = 0;
    for (int i = 0; i < 40; i++)                        // 40 self-syncs
        for (int k = 0; k < 10; k++) bits[n++] = k < 8;
    for (int i = 0; i < nbytes; i++) {
        int b = nib[rand() % 64];
        for (int k = 7; k >= 0; k--) bits[n++] = (unsigned char)(b >> k & 1);
    }
    // Edge times; capture in ticks of 1/75 us.
    wrdec_t d;
    wrdec_init(&d, 75);
    double last = -1;
    int m = 0, first = -1;
    for (int i = 0; i < n; i++) {
        if (!bits[i]) continue;
        double t = i * cell_us + ((rand() / (double)RAND_MAX) * 2 - 1) * jitter_us;
        if (last >= 0) {
            uint32_t ticks = (uint32_t)lround((t - last) * 75);
            uint32_t c = wrdec_cells(&d, ticks);
            for (uint32_t k = 0; k + 1 < c && m < MAXB; k++) got[m++] = 0;
            if (m < MAXB) got[m++] = 1;
        } else {
            first = i;
        }
        last = t;
    }
    int lastone = n - 1;
    while (!bits[lastone]) lastone--;           // zeros after the last 1 need the write's end
    int bad = 0, cmp = lastone - first;
    if (m < cmp) bad = cmp - m;
    for (int i = 0; i < cmp && i < m; i++)
        if (got[i] != bits[first + 1 + i]) bad++;
    if (m > cmp + 2) bad += m - cmp;
    printf("cell %.3f us, jitter +-%.2f us: %d bits, %d wrong, final estimate %.3f us\n",
           cell_us, jitter_us, cmp, bad, d.cell16 / 16.0 / 75);
    if (bad) fails++;
}

int main(void)
{
    double cells[] = { 3.85, 3.91, 3.93, 3.958, 4.0, 4.05 };
    for (int i = 0; i < 6; i++) {
        run(cells[i], 0.0, 6000, 1);
        run(cells[i], 0.25, 6000, 2);
        run(cells[i], 0.5, 6000, 3);       // +-0.5 us per edge: 1 us worst-case interval error
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "all write-decode tests passed", fails);
    return fails != 0;
}
