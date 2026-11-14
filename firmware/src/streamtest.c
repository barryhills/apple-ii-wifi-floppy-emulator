// Milestone 4 bench test. See streamtest.h.
//
// Capture: a second PIO state machine samples GP8 at 2 MHz (8 samples per
// 4 us cell) into a 64 KB buffer by DMA: 524,288 samples = 65,536 cells,
// about 1.3 revolutions of a 50,144-cell DOS 3.3 track, so the capture
// always spans a revolution boundary.
//
// Decode (independent of the player's logic): find each falling edge (start
// of a 1 us low pulse); the gap to the next is n cells = round(gap / 8),
// i.e. n-1 zeros and a 1. Then find where in the track the decoded bits
// start and compare every decoded bit with the track, wrapping at its exact
// length: a revolution one cell long or short shows up as mismatches after
// the wrap.
//
// Both state machines run from the same system clock, so this checks the
// logic (cell count, order, revolution length, pulse shape), not the
// crystal; the logic analyzer checks absolute timing.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "gcr.h"
#include "player.h"
#include "sample.pio.h"
#include "store.h"
#include "streamtest.h"
#include "woz.h"

#define CAP_PIN         8               // GP8 = WRDATA_3V; jumpered to GP10
#define CAP_HZ          2000000
#define SPC             8               // samples per cell
#define CAP_WORDS       16384           // 64 KB
#define MAX_DECODED     (CAP_WORDS * 32 / SPC + 32)
#define ALIGN_BITS      1024

static uint32_t track_buf[PLAYER_BUF_WORDS];
static uint8_t  enc[GCR_TRACK_BUF];
static uint32_t cap[CAP_WORDS];
static uint8_t  dec[MAX_DECODED / 8 + 1];

static bool inited, running;
static PIO cpio;
static uint csm, coff;
static int cch = -1;
static const uint8_t *trk_bits;         // the track as played, MSB first
static uint32_t trk_cells;
static unsigned trk_no;

static inline int bit_at(const uint8_t *b, uint32_t i) { return b[i >> 3] >> (7 - (i & 7)) & 1; }
static inline int sample_at(uint32_t i) { return cap[i >> 5] >> (31 - (i & 31)) & 1; }

static bool init_once(char *err, size_t errn)
{
    if (inited)
        return true;
    if (!player_init()) {
        snprintf(err, errn, "player: no free PIO state machine");
        return false;
    }
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&sample_program, &cpio, &csm, &coff,
                                                          CAP_PIN, 1, true)) {
        snprintf(err, errn, "capture: no free PIO state machine");
        return false;
    }
    pio_gpio_init(cpio, CAP_PIN);
    pio_sm_set_consecutive_pindirs(cpio, csm, CAP_PIN, 1, false);
    cch = dma_claim_unused_channel(true);
    inited = true;
    return true;
}

int streamtest_start(unsigned track, uint32_t cells, char *err, size_t errn)
{
    if (running) {
        snprintf(err, errn, "a test is already running");
        return -1;
    }
    image_info_t im;
    if (!store_image(&im)) {
        snprintf(err, errn, "no image mounted");
        return -1;
    }
    if (track >= GCR_TRACKS) {
        snprintf(err, errn, "track must be 0 to %d", GCR_TRACKS - 1);
        return -1;
    }
    if (im.format == FMT_DSK) {
        trk_cells = (uint32_t)gcr_encode_track(enc, im.data + track * GCR_TRACK_DATA,
                                               track, GCR_DEFAULT_VOLUME);
        trk_bits = enc;
    } else {
        static woz_info_t w;
        if (woz_parse(im.data, im.size, &w, err, errn) != 0)
            return -1;
        if (!woz_track_bits(&w, track * 4, &trk_bits, &trk_cells)) {
            snprintf(err, errn, "track %u is empty in this WOZ", track);
            return -1;
        }
    }
    if (cells && cells < trk_cells)
        trk_cells = cells;      // test any revolution length: the first `cells` cells
    if (trk_cells < 1000 || trk_cells > PLAYER_MAX_CELLS) {
        snprintf(err, errn, "track length %lu cells is out of range", (unsigned long)trk_cells);
        return -1;
    }
    if (!init_once(err, errn))
        return -1;
    trk_no = track;

    player_play(track_buf, player_pack(track_buf, trk_bits, trk_cells));

    // Capture: restart the sampler and point its DMA at the buffer.
    pio_sm_set_enabled(cpio, csm, false);
    pio_sm_config c = sample_program_get_default_config(coff);
    sm_config_set_in_pins(&c, CAP_PIN);
    sm_config_set_in_shift(&c, false, true, 32);       // left = MSB first; autopush 32
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / CAP_HZ);
    pio_sm_init(cpio, csm, coff, &c);

    dma_channel_config d = dma_channel_get_default_config(cch);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_dreq(&d, pio_get_dreq(cpio, csm, false));
    dma_channel_configure(cch, &d, cap, &cpio->rxf[csm], CAP_WORDS, true);
    pio_sm_set_enabled(cpio, csm, true);

    running = true;
    printf("streamtest: track %u, %lu cells, capturing\n", track, (unsigned long)trk_cells);
    return 0;
}

bool streamtest_poll(char *report, size_t n)
{
    if (!running || dma_channel_is_busy(cch))
        return false;
    pio_sm_set_enabled(cpio, csm, false);
    running = false;

    // Edges -> cells. gap_err[k] counts gaps that were 8n + (k-3) samples.
    uint32_t total = CAP_WORDS * 32, ndec = 0, edges = 0, bad_gaps = 0;
    uint32_t gap_err[7] = {0}, low_w[8] = {0}, low_other = 0, first_edge = 0;
    int64_t last = -1;
    for (uint32_t i = 1; i < total; i++) {
        if (!(sample_at(i - 1) == 1 && sample_at(i) == 0))
            continue;
        uint32_t lw = 0;                                // pulse width in samples
        while (i + lw < total && sample_at(i + lw) == 0 && lw < 64)
            lw++;
        if (lw < 8) low_w[lw]++; else low_other++;
        if (last < 0) {
            first_edge = i;
        } else {
            uint32_t gap = i - (uint32_t)last;
            uint32_t cells = (gap + SPC / 2) / SPC;
            int e = (int)gap - (int)(cells * SPC);
            if (e >= -3 && e <= 3) gap_err[e + 3]++; else bad_gaps++;
            if (cells == 0 || ndec + cells > MAX_DECODED - 1) { bad_gaps++; break; }
            for (uint32_t k = 0; k + 1 < cells; k++, ndec++)
                dec[ndec >> 3] &= (uint8_t)~(0x80 >> (ndec & 7));
            dec[ndec >> 3] |= (uint8_t)(0x80 >> (ndec & 7));
            ndec++;
        }
        if (last < 0) {
            // The first edge's 1 opens the decoded stream.
            dec[0] |= 0x80;
            ndec = 1;
        }
        last = i;
        edges++;
    }

    // Align: find the track offset where a window of ALIGN_BITS decoded bits
    // matches. A window inside a long run of sync nibbles (real-disk WOZ
    // gaps) matches many offsets, so slide it along until exactly one does;
    // offset is then where decoded bit 0 sits in the track.
    uint32_t matches = 0, offset = 0, win = 0;
    for (win = 0; win + ALIGN_BITS <= ndec; win += 2048) {
        matches = 0;
        for (uint32_t o = 0; o < trk_cells && matches < 2; o++) {
            uint32_t k = 0, t = o;
            while (k < ALIGN_BITS && bit_at(dec, win + k) == bit_at(trk_bits, t)) {
                k++;
                if (++t == trk_cells) t = 0;
            }
            if (k == ALIGN_BITS && matches++ == 0)
                offset = (o + trk_cells - win % trk_cells) % trk_cells;
        }
        if (matches == 1)
            break;
    }

    // Compare everything decoded, wrapping at the track's exact length.
    uint32_t mism = 0, first_mism = 0;
    if (matches == 1) {
        uint32_t t = offset;
        for (uint32_t k = 0; k < ndec; k++) {
            if (bit_at(dec, k) != bit_at(trk_bits, t) && mism++ == 0)
                first_mism = k;
            if (++t == trk_cells) t = 0;
        }
    }
    bool pass = matches == 1 && mism == 0 && bad_gaps == 0 && ndec > trk_cells;

    // Wiring diagnostics: share of high samples on GP8, and GP7/GP8/GP10 as
    // the SIO sees them now (GP7 is physical pin 10, next to GND on pin 8).
    uint32_t ones = 0;
    for (uint32_t w = 0; w < CAP_WORDS; w++)
        ones += (uint32_t)__builtin_popcount(cap[w]);
    gpio_init(7);
    gpio_set_dir(7, GPIO_IN);
    gpio_pull_up(7);
    sleep_us(10);
    int g7 = gpio_get(7), g8 = gpio_get(CAP_PIN), g10 = gpio_get(PLAYER_PIN);
    gpio_disable_pulls(7);

    snprintf(report, n,
        "{\"pass\":%s,\"track\":%u,\"track_cells\":%lu,\"decoded_cells\":%lu,"
        "\"revolutions\":%.3f,\"edges\":%lu,\"first_edge_sample\":%lu,"
        "\"align_matches\":%lu,\"align_window\":%lu,\"offset\":%lu,\"mismatches\":%lu,\"first_mismatch\":%lu,"
        "\"gap_error_samples_-3..+3\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"bad_gaps\":%lu,"
        "\"pulse_width_samples_0..7\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"pulse_width_other\":%lu,"
        "\"sample_us\":0.5,\"player\":\"running on GP10\","
        "\"gp8_high_share\":%.3f,\"gp7_pulled_up_reads\":%d,\"gp8_now\":%d,\"gp10_now\":%d}\n",
        pass ? "true" : "false", trk_no, (unsigned long)trk_cells, (unsigned long)ndec,
        (double)ndec / trk_cells, (unsigned long)edges, (unsigned long)first_edge,
        (unsigned long)matches, (unsigned long)win, (unsigned long)offset, (unsigned long)mism,
        (unsigned long)first_mism,
        (unsigned long)gap_err[0], (unsigned long)gap_err[1], (unsigned long)gap_err[2],
        (unsigned long)gap_err[3], (unsigned long)gap_err[4], (unsigned long)gap_err[5],
        (unsigned long)gap_err[6], (unsigned long)bad_gaps,
        (unsigned long)low_w[0], (unsigned long)low_w[1], (unsigned long)low_w[2],
        (unsigned long)low_w[3], (unsigned long)low_w[4], (unsigned long)low_w[5],
        (unsigned long)low_w[6], (unsigned long)low_w[7], (unsigned long)low_other,
        (double)ones / (CAP_WORDS * 32.0), g7, g8, g10);
    printf("streamtest: %s", report);
    return true;
}
