// Bench self-tests. See benchtest.h.
//
// Capture: a PIO state machine samples GP10 at 2 MHz (8 samples per 4 us
// cell) into a 64 KB buffer by DMA: 524,288 samples = 65,536 cells, about
// 1.3 revolutions of a 50,144-cell DOS 3.3 track, so the capture always
// spans a revolution boundary. It does not touch GP10's function or
// direction: it only reads the pad.
//
// Decode (independent of the player's logic): find the start of each pulse
// (idle level -> pulse level, either sense); the gap to the next is
// n cells = round(gap / 8), i.e. n-1 zeros and a 1. Then find where in the
// track the decoded bits start and compare every decoded bit with the
// track, wrapping at its exact length: a revolution one cell long or short
// shows up as mismatches after the wrap.
//
// Both state machines run from the same system clock, so this checks the
// logic (cell count, order, revolution length, pulse shape, start cell,
// head position, angle), not the crystal; a scope checks absolute timing.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/timer.h"

#include "benchtest.h"
#include "diag.h"
#include "drive.h"
#include "gcr.h"
#include "player.h"
#include "sample.pio.h"
#include "store.h"
#include "woz.h"

#define CAP_PIN         PLAYER_PIN
#define CAP_HZ          2000000
#define SPC             8               // samples per cell
#define CAP_WORDS       16384           // 64 KB
#define MAX_DECODED     (CAP_WORDS * 32 / SPC + 32)
#define ALIGN_BITS      1024
#define PULSE_AT_US     3               // a 1's pulse starts 12 PIO cycles into its cell

#define PIN_PH0         2
#define PIN_ENABLE      6

static uint32_t track_buf[PLAYER_WORDS(GCR_TRACK_BUF * 8)];
static uint8_t  enc[GCR_TRACK_BUF];
static uint32_t cap[CAP_WORDS];
static uint8_t  dec[MAX_DECODED / 8 + 1];

static bool inited, running;
static enum { T_STREAM, T_SEEK } kind;
static PIO cpio;
static uint csm, coff;
static int cch = -1;
static uint32_t cap_t0;                 // timer at capture start (us)

static const uint8_t *trk_bits;         // the expected track, MSB first; NULL = expect silence
static uint32_t trk_cells;
static unsigned trk_no;
static uint32_t start_cell;             // stream: where playing began
static int seek_q, seek_q_got;          // seek: target, and where the head went
static uint32_t seek_steps, seek_ms;
static uint32_t a_cell, a_cells, a_time;    // seek: core 1's angle anchor
static int flash_writes = -1;               // seek: directory writes during capture, -1 = none asked
static uint32_t flash_us, flash_loop_max_us;

static inline int bit_at(const uint8_t *b, uint32_t i) { return b[i >> 3] >> (7 - (i & 7)) & 1; }
static inline int sample_at(uint32_t i) { return cap[i >> 5] >> (31 - (i & 31)) & 1; }

static bool init_once(char *err, size_t errn)
{
    if (inited)
        return true;
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&sample_program, &cpio, &csm, &coff,
                                                          CAP_PIN, 1, true)) {
        snprintf(err, errn, "capture: no free PIO state machine");
        return false;
    }
    cch = dma_claim_unused_channel(true);
    inited = true;
    return true;
}

static void capture_start(void)
{
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
    cap_t0 = timer_hw->timerawl;
    pio_sm_set_enabled(cpio, csm, true);
}

// The track as the drive should serve it, straight from the image (not from
// the drive's pool): t = DSK track, or q = WOZ quarter track.
static int expected_bits(int q, char *err, size_t errn)
{
    image_info_t im;
    trk_bits = NULL;
    trk_cells = 0;
    if (!store_image(&im)) {
        snprintf(err, errn, "no image mounted");
        return -1;
    }
    if (im.format == FMT_DSK) {
        unsigned t = (unsigned)(q + 1) / 4;
        if (q % 4 == 2 || t >= GCR_TRACKS)
            return 0;                                   // silence
        trk_cells = (uint32_t)gcr_encode_track(enc, im.data + t * GCR_TRACK_DATA, t,
                                               gcr_dsk_volume(im.data));
        trk_bits = enc;
        trk_no = t;
        return 0;
    }
    static woz_info_t w;
    if (woz_parse(im.data, im.size, &w, err, errn) != 0)
        return -1;
    if (woz_track_bits(&w, (unsigned)q, &trk_bits, &trk_cells))
        trk_no = w.tmap[q];
    else
        trk_bits = NULL;
    return 0;
}

int benchtest_stream(unsigned track, uint32_t cells, uint32_t start, char *err, size_t errn)
{
    if (running) {
        snprintf(err, errn, "a test is already running");
        return -1;
    }
    if (track >= 40) {
        snprintf(err, errn, "track must be 0 to 39");
        return -1;
    }
    if (expected_bits((int)track * 4, err, errn) != 0)
        return -1;
    if (!trk_bits) {
        snprintf(err, errn, "track %u is empty in this image", track);
        return -1;
    }
    if (cells && cells < trk_cells)
        trk_cells = cells;      // test any revolution length: the first `cells` cells
    if (trk_cells < 1000 || trk_cells > GCR_TRACK_BUF * 8) {
        snprintf(err, errn, "track length %lu cells is out of range", (unsigned long)trk_cells);
        return -1;
    }
    if (start >= trk_cells) {
        snprintf(err, errn, "start must be below %lu", (unsigned long)trk_cells);
        return -1;
    }
    if (!init_once(err, errn))
        return -1;

    player_pack(track_buf, trk_bits, trk_cells);
    drive_suspend();
    capture_start();                    // first, so the capture sees playing begin
    sleep_us(20);
    player_start(track_buf, start);
    start_cell = start;
    kind = T_STREAM;
    running = true;
    printf("benchtest: stream track %u, %lu cells from %lu\n", track,
           (unsigned long)trk_cells, (unsigned long)start);
    return 0;
}

// ---------------------------------------------------------------- seek

static void phases(unsigned mask)
{
    for (int p = 0; p < 4; p++)
        gpio_set_inover((uint)(PIN_PH0 + p), mask >> p & 1 ? GPIO_OVERRIDE_HIGH : GPIO_OVERRIDE_LOW);
    sleep_us(2000);                     // RWTS-like hold per phase change
    diag_feed();
}

static int head(void)
{
    drive_status_t s;
    sleep_us(50);                       // core 1 loops in well under this
    drive_get_status(&s);
    return s.qtrack;
}

// DOS 3.3 style: the head's current phase on, then for each half track the
// next phase on, the previous off (overlapping), finally all off. For a
// quarter-track target, finish holding two adjacent phases.
static void seek_to(int target)
{
    int q = head();
    int ht = q / 2, cur = ht & 3, want = target / 2;
    phases(1u << cur);
    while (ht != want) {
        int dir = want > ht ? 1 : -1;
        int nxt = (cur + dir) & 3;
        phases((1u << cur) | (1u << nxt));
        phases(1u << nxt);
        cur = nxt;
        ht += dir;
    }
    if (target & 1)
        phases((1u << cur) | (1u << ((cur + 1) & 3)));
    phases(0);
}

int benchtest_seek(int q, bool flash, char *err, size_t errn)
{
    if (running) {
        snprintf(err, errn, "a test is already running");
        return -1;
    }
    if (q < 0 || q > 159) {
        snprintf(err, errn, "q must be 0 to 159");
        return -1;
    }
    drive_status_t s;
    drive_get_status(&s);
    if (!s.loaded) {
        snprintf(err, errn, "the drive is not serving an image%s%s", s.error[0] ? ": " : "", s.error);
        return -1;
    }
    if (expected_bits(q, err, errn) != 0 || !init_once(err, errn))
        return -1;

    uint32_t steps0 = s.steps, t0 = timer_hw->timerawl;
    // Take over the inputs: phases off first, then select the drive.
    for (int p = 0; p < 4; p++)
        gpio_set_inover((uint)(PIN_PH0 + p), GPIO_OVERRIDE_LOW);
    gpio_set_inover(PIN_ENABLE, GPIO_OVERRIDE_LOW);
    seek_to(q);
    seek_q_got = head();
    // Deselect, then give the pins back.
    gpio_set_inover(PIN_ENABLE, GPIO_OVERRIDE_HIGH);
    sleep_us(50);
    for (int p = 0; p < 4; p++)
        gpio_set_inover((uint)(PIN_PH0 + p), GPIO_OVERRIDE_NORMAL);
    gpio_set_inover(PIN_ENABLE, GPIO_OVERRIDE_NORMAL);
    drive_get_status(&s);
    seek_steps = s.steps - steps0;
    seek_ms = (timer_hw->timerawl - t0) / 1000;
    seek_q = q;

    sleep_us(100);                      // core 1 has switched tracks
    drive_angle(&a_cell, &a_cells, &a_time);
    capture_start();
    kind = T_SEEK;
    running = true;
    flash_writes = -1;
    if (flash) {
        // Two directory writes inside the ~260 ms capture. Core 0 is stalled
        // while flash is busy; core 1, the DMA and the PIO must not be.
        image_info_t im;
        int row = store_mounted();
        char e2[96];
        flash_writes = 0;
        drive_reset_loop_max();
        uint32_t f0 = timer_hw->timerawl;
        if (row >= 0 && store_row(row, &im)) {
            bool w = im.flags & IMG_FLAG_WRITABLE;
            if (store_set_writable(row, !w, e2, sizeof e2) == 0)
                flash_writes++;
            if (store_set_writable(row, w, e2, sizeof e2) == 0)
                flash_writes++;
        }
        flash_us = timer_hw->timerawl - f0;
        drive_status_t ds;
        drive_get_status(&ds);
        flash_loop_max_us = ds.loop_max_us;
    }
    printf("benchtest: seek to q %d (head at %d, %lu steps)\n", q, seek_q_got,
           (unsigned long)seek_steps);
    return 0;
}

// ---------------------------------------------------------------- poll

bool benchtest_poll(char *report, size_t n)
{
    if (!running || dma_channel_is_busy(cch))
        return false;
    pio_sm_set_enabled(cpio, csm, false);
    running = false;
    if (kind == T_STREAM) {
        player_stop();
        drive_resume();
    }

    // Edges -> cells. gap_err[k] counts gaps that were 8n + (k-3) samples.
    int pl = player_idle_high() ? 0 : 1;            // pulse level
    uint32_t total = CAP_WORDS * 32, ndec = 0, edges = 0, bad_gaps = 0;
    static uint32_t gap_err[7], pw[8];        // static: keep this off the 4 KB main stack
    memset(gap_err, 0, sizeof gap_err);
    memset(pw, 0, sizeof pw);
    uint32_t pw_other = 0, first_edge = 0;
    int64_t last = -1;
    for (uint32_t i = 1; i < total; i++) {
        if (!(sample_at(i - 1) != pl && sample_at(i) == pl))
            continue;
        uint32_t lw = 0;                                // pulse width in samples
        while (i + lw < total && sample_at(i + lw) == pl && lw < 64)
            lw++;
        if (lw < 8) pw[lw]++; else pw_other++;
        if (last < 0) {
            first_edge = i;
            dec[0] |= 0x80;                             // the first edge's 1 opens the stream
            ndec = 1;
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
        last = i;
        edges++;
    }

    // Align: find the track offset where a window of ALIGN_BITS decoded bits
    // matches. A window inside a long run of sync nibbles (real-disk WOZ
    // gaps) matches many offsets, so slide it along until exactly one does;
    // offset is then where decoded bit 0 sits in the track.
    uint32_t matches = 0, offset = 0, win = 0, mism = 0, first_mism = 0;
    if (trk_bits) {
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
        if (matches == 1) {
            uint32_t t = offset;
            for (uint32_t k = 0; k < ndec; k++) {
                if (bit_at(dec, k) != bit_at(trk_bits, t) && mism++ == 0)
                    first_mism = k;
                if (++t == trk_cells) t = 0;
            }
        }
    }
    bool data_ok = trk_bits ? matches == 1 && mism == 0 && bad_gaps == 0 && ndec > trk_cells
                            : edges == 0;

    static char extra[256];
    bool pass = data_ok;
    if (kind == T_STREAM) {
        // Playing began at start_cell: decoded bit 0 is the first 1 at or
        // after it. player_start begins a start within the last 8 words of
        // the revolution at cell 0 instead (see player.c).
        uint32_t want = start_cell / 32 + 8 > trk_cells / 32 + 1 ? 0 : start_cell;
        for (uint32_t k = 0; k < trk_cells && !bit_at(trk_bits, want); k++)
            if (++want == trk_cells) want = 0;
        bool start_ok = matches == 1 && offset == want;
        pass = pass && start_ok;
        snprintf(extra, sizeof extra,
                 "\"test\":\"stream\",\"start\":%lu,\"first_one_expected\":%lu,\"start_ok\":%s",
                 (unsigned long)start_cell, (unsigned long)want, start_ok ? "true" : "false");
    } else {
        // The cell core 1 reckons was under the head when decoded bit 0's
        // cell began; the player must be within a few cells of it.
        long diff = 0;
        bool angle_ok = true;
        if (trk_bits && matches == 1) {
            uint32_t t = cap_t0 + first_edge / 2 - PULSE_AT_US;
            uint32_t c = (a_cell + (t - a_time) / 4) % a_cells;
            diff = (long)offset - (long)c;
            if (diff > (long)a_cells / 2) diff -= (long)a_cells;
            if (diff < -(long)a_cells / 2) diff += (long)a_cells;
            angle_ok = a_cells == trk_cells && diff >= -3 && diff <= 3;
        }
        bool head_ok = seek_q_got == seek_q;
        pass = pass && head_ok && angle_ok;
        snprintf(extra, sizeof extra,
                 "\"test\":\"seek\",\"q\":%d,\"head_q\":%d,\"head_ok\":%s,\"steps\":%lu,\"seek_ms\":%lu,"
                 "\"expect\":\"%s\",\"angle_cells\":%lu,\"angle_diff_cells\":%ld,\"angle_ok\":%s,"
                 "\"flash_writes\":%d,\"flash_ms\":%lu,\"core1_max_loop_us_during_flash\":%lu",
                 seek_q, seek_q_got, head_ok ? "true" : "false", (unsigned long)seek_steps,
                 (unsigned long)seek_ms, trk_bits ? "data" : "silence", (unsigned long)a_cells, diff,
                 angle_ok ? "true" : "false", flash_writes, (unsigned long)(flash_us / 1000),
                 (unsigned long)flash_loop_max_us);
        if (flash_writes >= 0 && (flash_writes != 2 || flash_loop_max_us > 50))
            pass = false;
    }

    snprintf(report, n,
        "{\"pass\":%s,%s,\"track\":%u,\"track_cells\":%lu,\"decoded_cells\":%lu,"
        "\"revolutions\":%lu.%03lu,\"edges\":%lu,\"first_edge_sample\":%lu,"
        "\"align_matches\":%lu,\"align_window\":%lu,\"offset\":%lu,\"mismatches\":%lu,\"first_mismatch\":%lu,"
        "\"gap_error_samples_-3..+3\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"bad_gaps\":%lu,"
        "\"pulse_width_samples_0..7\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"pulse_width_other\":%lu,"
        "\"sample_us\":0.5,\"rddata_idle\":\"%s\"}\n",
        pass ? "true" : "false", extra, trk_no, (unsigned long)trk_cells, (unsigned long)ndec,
        (unsigned long)(trk_cells ? ndec / trk_cells : 0),
        (unsigned long)(trk_cells ? (ndec % trk_cells) * 1000ull / trk_cells : 0), (unsigned long)edges, (unsigned long)first_edge,
        (unsigned long)matches, (unsigned long)win, (unsigned long)offset, (unsigned long)mism,
        (unsigned long)first_mism,
        (unsigned long)gap_err[0], (unsigned long)gap_err[1], (unsigned long)gap_err[2],
        (unsigned long)gap_err[3], (unsigned long)gap_err[4], (unsigned long)gap_err[5],
        (unsigned long)gap_err[6], (unsigned long)bad_gaps,
        (unsigned long)pw[0], (unsigned long)pw[1], (unsigned long)pw[2],
        (unsigned long)pw[3], (unsigned long)pw[4], (unsigned long)pw[5],
        (unsigned long)pw[6], (unsigned long)pw[7], (unsigned long)pw_other,
        player_idle_high() ? "high" : "low");
    printf("benchtest: %s", report);
    return true;
}
