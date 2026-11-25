// The drive: core-1 loop (ENABLE*, stepper, read-data player, write
// capture) and core-0 loading of the mounted image into a RAM track pool and
// writing back what the Apple wrote. See drive.h.
//
// Core 1 must never touch flash: core 0's flash writes (uploads, directory
// updates) run with only core 0's interrupts off (PICO_FLASH_ASSUME_CORE1_SAFE,
// CMakeLists.txt), and flash is unreadable meanwhile. So everything core 1
// runs is __not_in_flash_func or inline, it reads only RAM, and
// tools/check_core1_ram.py checks the built ELF for calls out of RAM.
//
// The disk's angle: core 1 keeps an anchor (cell a_cell of an a_cells-cell
// track at time a_time, 1 us timer). The cell under the head now is
// a_cell + elapsed/4 us, mod a_cells. On a head move to a track of N cells it
// starts the new track at the same fraction of a revolution, c * N / a_cells,
// so a real disk's rotation carries across steps (DOS does not need this;
// software that times sectors across tracks may). Silence (empty position or
// parked) keeps the old anchor turning.
//
// Writes (milestone 6). While ENABLE* and WRREQ* are both low and an image
// is served, core 1 takes WRDATA edge intervals from a PIO timer
// (wrcap.pio), turns them into bit cells (wrdec.h) and writes those bits
// into the RAM track under the head, from the cell under the head when the
// write began, one track cell per written cell. (The //e writes ~3.91 us
// cells, the track plays 4 us cells: a real disk would hold the written
// bits ~2% closer together; in a fixed-length bit track, as WOZ is, they
// simply take consecutive cells. A DOS sector write runs ~70 cells past
// where the real head would be when it ends, inside gap 3.) Every image is
// writable (Barry, 2026-11-19: no write-protect, not even a WOZ's INFO
// flag); WRPROT reads high, and nothing is written, only while no image is
// served.
//
// Each track has a generation counter (core 1: +1 when a write starts, +1
// when it ends, so odd = being written). Core 0 writes the image back to
// flash once no write has happened for WRITEBACK_QUIET_MS: a whole new copy
// in a free image space, then the directory switch (the upload path; a
// power cut leaves the old or the new image, never a mix). Tracks whose
// generation moved during the write-back stay dirty for the next one. A
// .dsk takes the sectors decoded from each written track (a sector that no
// longer decodes keeps its old contents); a WOZ takes the track bits as is.

#include <stdio.h>
#include <string.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"

#include "api.h"
#include "clock.h"
#include "crc32.h"
#include "diag.h"
#include "drive.h"
#include "gcr.h"
#include "json.h"
#include "net.h"
#include "player.h"
#include "stepper.h"
#include "store.h"
#include "woz.h"
#include "wrcap.pio.h"
#include "wrdec.h"

// GPIO (schematic r0.3). PH0..PH3 must stay consecutive: one read gets all.
#define PIN_PH0      2      // GP2..GP5 = PH0_3V..PH3_3V
#define PIN_ENABLE   6      // ENABLE_3V*, active low
#define PIN_WRREQ    7      // WRREQ_3V*, active low
#define PIN_WRDATA   8      // WRDATA_3V
#define PIN_WRPROT   11     // WRPROT_3V: high = write-protected
// Activity LED (rev B bodge, Barry 2026-11-19): GP16 (Pico pin 21) -> R5
// -> D2 -> GND, R5's +3V3 hole left empty. Lit while the drive is selected
// or writes are unsaved/being saved: "don't switch off now". Driven by
// core 0 only (gpio_put = atomic SIO set/clr, so no read-modify-write
// against core 1's WRPROT/RDDATA pins). From power-on until the first WiFi
// join (when the onboard LED starts its heartbeat) it is simply on (Barry,
// 2026-11-19): a lamp test and "booting"; activity only after that. Setup
// mode: no boot lamp (off once setup mode is decided, activity as usual).
#define PIN_LED      16

#define QPOS         (STEPPER_MAX_Q + 1)
#define NO_TRACK     0xFF
#define WRITEBACK_QUIET_MS  500     // write back after this long without a write
#define MAX_TRAIL_ZEROS     16      // zeros written after the last edge of a write
#define WRCAP_EDGE_COUNTS   2       // wrcap.pio spends 4-5 cycles per edge outside its count

// Track pool. Big enough for any image the store accepts: a WOZ file is at
// most IMAGE_MAX_SIZE, its bit data at most that less the 1,536-byte WOZ 2
// header (WOZ 1 records are smaller), and each track costs 2 words beyond
// its data in player layout (count word, spare word).
#define POOL_WORDS   ((IMAGE_MAX_SIZE - 1536u) / 4 + 2 * QPOS)
static uint32_t pool[POOL_WORDS];
static uint32_t *tracks[QPOS];      // by pool index: DSK track, or WOZ TRK index
static uint8_t qmap[QPOS];          // quarter track -> pool index, NO_TRACK = empty

// Core 0 -> core 1.
enum { CMD_RUN = 1, CMD_PARK = 2 };
static volatile uint32_t cmd, cmd_seq, ack_seq;

// Core 1 -> core 0.
static volatile int pub_q, pub_track = -1;
static volatile bool pub_en;
static volatile uint32_t pub_steps, pub_selects;
static volatile uint32_t pub_max_gap_us;    // longest core-1 loop pass seen
static volatile bool gap_reset;
static volatile uint32_t ang_seq, ang_cell, ang_cells, ang_time;    // seqlock
static volatile uint32_t trk_gen[QPOS];     // +1 at write start and end (odd = writing)
static volatile bool pub_writing;
static volatile uint32_t pub_writes, pub_write_bits, pub_write_end, pub_fifo_full, pub_resyncs;
static volatile uint32_t pub_write_cell16;  // the last write's cell estimate (ticks x16)

// Core 0 -> core 1, set once or while no write is in progress.
static volatile bool want_wp = true;        // WRPROT level, and no writes
static PIO wpio;                            // write capture state machine
static uint wsm, woff;
static uint32_t ticks_per_us;

// Core 0 state.
static bool loaded, suspended;
static int loaded_row = -2;         // -2 = nothing tried yet
static unsigned loaded_slot;
static image_format_t loaded_fmt;
static uint32_t load_ms;
static char error[96];
static uint8_t enc[GCR_TRACK_BUF];          // encode / write-back scratch (main loop only)
static uint32_t saved_gen[QPOS];            // trk_gen as last written to flash
static bool scratch_lent;                   // a bench test holds enc
static uint32_t flush_count, flush_fail, flush_ms, flush_lost_sectors;
static char flush_msg[176];
static absolute_time_t flush_retry_at, prepare_at;

// ---------------------------------------------------------------- core 1

static void __not_in_flash_func(publish_angle)(uint32_t cell, uint32_t cells, uint32_t t)
{
    ang_seq++;
    __dmb();
    ang_cell = cell;
    ang_cells = cells;
    ang_time = t;
    __dmb();
    ang_seq++;
}

// Core 1's write in progress.
static struct {
    bool     on, first;
    int      trk;
    uint32_t *buf;
    uint32_t cells, pos, t0, t_last, bits;
    wrdec_t  dec;
} W;

static inline __attribute__((always_inline)) void put_cell(int one)
{
    uint32_t *w = W.buf + 1 + (W.pos >> 5), m = 1u << (31 - (W.pos & 31));
    if (one)
        *w |= m;
    else
        *w &= ~m;
    if (++W.pos == W.cells)
        W.pos = 0;
    W.bits++;
}

// n cells: n-1 zeros and a 1 (n is at most one revolution).
static void __not_in_flash_func(put_interval)(uint32_t n)
{
    if (n > W.cells)
        n = W.cells;
    while (--n)
        put_cell(0);
    put_cell(1);
}

static uint32_t __not_in_flash_func(us_to_cells)(uint32_t us)
{
    if (us > 100000)
        us = 100000;
    return (us * ticks_per_us * WRDEC_FRAC + W.dec.cell16 / 2) / W.dec.cell16;
}

static inline __attribute__((always_inline)) int get_cell(uint32_t p)
{
    return W.buf[1 + (p >> 5)] >> (31 - (p & 31)) & 1;
}

// After a write: if the old bits that follow are a run of 1s running into a
// D5 AA prologue, as in images whose gaps are plain 8-bit FF bytes (Virtual
// ]['s WOZ files), the run now starts at an arbitrary bit, and a reader
// framing nibbles through all-1s never realigns: it would miss that next
// field. Rewrite the run as 10-bit self-sync bytes (FF + two 0s, Beneath
// Apple DOS ch. 3) ending exactly at the D5, which realign it as on a disk
// DOS formatted. Gaps that already hold 0s (self-syncs) are left alone.
static void __not_in_flash_func(resync_tail)(void)
{
    uint32_t p = W.pos, run = 0;
    while (run < 4096 && get_cell((p + run) % W.cells))
        run++;
    if (run < 22)
        return;
    uint32_t d5 = (p + run - 2) % W.cells;      // D5 = 11010101: its first two 1s end the run
    // D5 AA then 96 or AD, bit for bit.
    uint32_t v = 0;
    for (uint32_t k = 0; k < 24; k++)
        v = v << 1 | (uint32_t)get_cell((d5 + k) % W.cells);
    if ((v >> 8) != 0xD5AA || ((v & 0xFF) != 0x96 && (v & 0xFF) != 0xAD))
        return;
    uint32_t n = run - 2, save = W.pos;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t back = n - 1 - k;              // cells before the D5
        W.pos = (p + k) % W.cells;
        put_cell(back % 10 >= 2 && k >= n % 10);    // ...1111111100 | D5; leading partial: 0s
    }
    W.pos = save;
    W.bits -= n;
    pub_resyncs++;
}

static void __not_in_flash_func(write_end)(uint32_t now)
{
    if (!W.first) {
        // The write current stays on after the last transition: the cells
        // up to WRREQ* going high are zeros (a few at most for DOS).
        uint32_t z = us_to_cells(now - W.t_last);
        if (z > MAX_TRAIL_ZEROS)
            z = MAX_TRAIL_ZEROS;
        while (z--)
            put_cell(0);
        resync_tail();
    }
    trk_gen[W.trk]++;                       // even: done
    W.on = false;
    pub_write_bits = W.bits;
    pub_write_cell16 = W.dec.cell16;
    pub_write_end = now;
    pub_writes++;
    pub_writing = false;
}

static void __not_in_flash_func(core1_main)(void)
{
    stepper_t st = { .q = 0 };          // head position unknown at power-on; DOS recalibrates
    int playing = -1;
    bool running = false, was_en = false;
    uint32_t a_cell = 0, a_cells = 50000, a_time = timer_hw->timerawl;
    uint32_t last = a_time;

    for (;;) {
        uint32_t s = cmd_seq;
        if (s != ack_seq) {
            __dmb();
            if (cmd == CMD_PARK) {
                if (W.on)
                    write_end(timer_hw->timerawl);
                player_stop();
                playing = -1;
                running = false;
            } else {
                running = true;
            }
            __dmb();
            ack_seq = s;
        }

        // Inputs, all at once. The stepper is powered only while the drive
        // is enabled (CLAUDE.md, Apple dwg 050-0007-01), so phases are
        // ignored otherwise: with two drives on one card, the other drive's
        // seeks must not move this head. Phase states shorter than
        // STEPPER_SETTLE_US are ignored too (stepper.h).
        uint32_t in = sio_hw->gpio_in;
        uint32_t now = timer_hw->timerawl;
        bool en = !(in & (1u << PIN_ENABLE));
        // Loop health: a pass is normally well under a microsecond; a stall
        // (e.g. a flash fetch while core 0 writes flash) would show here.
        uint32_t gap = now - last;
        last = now;
        if (gap_reset) {
            gap_reset = false;
            pub_max_gap_us = 0;
        } else if (gap > pub_max_gap_us) {
            pub_max_gap_us = gap;
        }
        if (en && !was_en)
            pub_selects++;
        was_en = en;
        if (stepper_update(&st, en, (in >> PIN_PH0) & 0xF, now))
            pub_steps++;
        int q = st.q;

        int want = running && qmap[q] != NO_TRACK ? qmap[q] : -1;
        if (want != playing) {
            uint32_t c = (a_cell + (now - a_time) / 4) % a_cells;
            if (want >= 0) {
                const uint32_t *buf = tracks[want];
                uint32_t n = buf[0] + 1;
                uint32_t start = c * n / a_cells;   // c < 65536, n < 65536: fits 32 bits
                player_start(buf, start);
                a_cell = start;
                a_cells = n;
            } else {
                player_stop();
                a_cell = c;
            }
            a_time = now;
            playing = want;
            publish_angle(a_cell, a_cells, a_time);
        } else if (now - a_time > 1000000) {
            uint32_t e = (now - a_time) / 4;        // whole cells elapsed
            a_cell = (a_cell + e) % a_cells;
            a_time += e * 4;
            publish_angle(a_cell, a_cells, a_time);
        }

        // Write protect, and writes. The capture FIFO is drained at every
        // pass so no interval is lost (it holds 8, >= 31 us of edges).
        bool wp = want_wp;
        if (wp)
            sio_hw->gpio_set = 1u << PIN_WRPROT;
        else
            sio_hw->gpio_clr = 1u << PIN_WRPROT;
        bool wreq = en && !(in & (1u << PIN_WRREQ)) && !wp && playing >= 0;
        uint32_t empty = 1u << (PIO_FSTAT_RXEMPTY_LSB + wsm);
        if (!W.on && wreq) {
            while (!(wpio->fstat & empty))
                (void)wpio->rxf[wsm];               // edges from before this write
            W.on = W.first = true;
            W.trk = playing;
            W.buf = tracks[playing];
            W.cells = W.buf[0] + 1;
            W.pos = (a_cell + (now - a_time) / 4) % a_cells;    // under the head now
            W.t0 = W.t_last = now;
            W.bits = 0;
            wrdec_init(&W.dec, ticks_per_us);
            trk_gen[W.trk]++;                       // odd: being written
            pub_writing = true;
        }
        if (W.on) {
            if (((wpio->flevel >> (wsm * 8 + 4)) & 0xF) >= 8)
                pub_fifo_full++;
            while (!(wpio->fstat & empty)) {
                uint32_t ticks = wpio->rxf[wsm];
                uint32_t t = timer_hw->timerawl;
                if (W.first) {
                    // The interval before the first edge began long before
                    // the write; place the first 1 by time since WRREQ*.
                    uint32_t n = us_to_cells(t - W.t0);
                    put_interval(n ? n : 1);
                    W.first = false;
                } else {
                    put_interval(wrdec_cells(&W.dec, ticks + WRCAP_EDGE_COUNTS));
                }
                W.t_last = t;
            }
            if (!wreq || playing != W.trk)
                write_end(timer_hw->timerawl);
        }

        pub_q = st.q;
        pub_en = en;
        pub_track = playing;
    }
}

// ---------------------------------------------------------------- core 0

static void send(uint32_t c)
{
    cmd = c;
    __dmb();
    uint32_t s = cmd_seq + 1;
    cmd_seq = s;
    absolute_time_t until = make_timeout_time_ms(100);
    while (ack_seq != s) {
        if (time_reached(until)) {
            printf("drive: core 1 did not answer\n");
            break;
        }
    }
    __dmb();
}

void drive_led_power_on(void)
{
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);
}

void drive_init(void)
{
    for (int p = PIN_PH0; p <= PIN_WRDATA; p++) {
        gpio_init((uint)p);
        gpio_set_dir((uint)p, GPIO_IN);
    }
    // ENABLE* and WRREQ* idle "off" with nothing connected (bare Pico; on
    // the board R3/R4 do this on the 5 V side and U1 drives these pins).
    // No pull-downs: RP2350 erratum E9 makes them unreliable.
    gpio_pull_up(PIN_ENABLE);
    gpio_pull_up(PIN_WRREQ);

    gpio_init(PIN_WRPROT);
    gpio_set_dir(PIN_WRPROT, GPIO_OUT);
    gpio_put(PIN_WRPROT, 1);            // protected until an image is served writable

    // Write capture: times WRDATA's transitions (the JMP pin) at the system
    // clock, 2 cycles per count.
    if (!pio_claim_free_sm_and_add_program(&wrcap_program, &wpio, &wsm, &woff)) {
        snprintf(error, sizeof error, "no PIO state machine for WRDATA");
        return;
    }
    pio_sm_config c = wrcap_program_get_default_config(woff);
    sm_config_set_jmp_pin(&c, PIN_WRDATA);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(wpio, wsm, woff + wrcap_offset_start, &c);
    pio_sm_set_enabled(wpio, wsm, true);
    ticks_per_us = clock_get_hz(clk_sys) / 2000000u;

    memset(qmap, NO_TRACK, sizeof qmap);
    if (!player_init()) {
        snprintf(error, sizeof error, "no PIO state machine for RDDATA");
        return;
    }
    multicore_launch_core1(core1_main);
    printf("drive: core 1 running, pool %u KB, write capture PIO%u SM%u at %lu counts/us\n",
           (unsigned)(sizeof pool / 1024), (unsigned)pio_get_index(wpio), wsm,
           (unsigned long)ticks_per_us);
}

void drive_set_wrdata_pin(unsigned pin)
{
    hw_write_masked(&wpio->sm[wsm].execctrl, pin << PIO_SM0_EXECCTRL_JMP_PIN_LSB,
                    PIO_SM0_EXECCTRL_JMP_PIN_BITS);
}

// Fill the pool from the image. Core 1 must be parked.
static int build(const image_info_t *im, char *err, size_t errn)
{
    memset(qmap, NO_TRACK, sizeof qmap);
    memset(tracks, 0, sizeof tracks);
    uint32_t used = 0;

    if (im->format == FMT_DSK) {
        unsigned vol = gcr_dsk_volume(im->data);
        for (unsigned t = 0; t < GCR_TRACKS; t++) {
            uint32_t cells = (uint32_t)gcr_encode_track(enc, im->data + t * GCR_TRACK_DATA, t, vol);
            if (used + PLAYER_WORDS(cells) > POOL_WORDS) {
                snprintf(err, errn, "track pool full");
                return -1;
            }
            tracks[t] = pool + used;
            used += player_pack(tracks[t], enc, cells);
        }
        // Track t is read at quarter tracks 4t-1, 4t, 4t+1; the half-track
        // position between tracks reads nothing (the same TMAP as woz.c
        // writes, from the WOZ 2 reference's example).
        for (int q = 0; q < QPOS; q++) {
            unsigned t = (unsigned)(q + 1) / 4;
            if (q % 4 != 2 && t < GCR_TRACKS)
                qmap[q] = (uint8_t)t;
        }
        return 0;
    }

    static woz_info_t w;
    if (woz_parse(im->data, im->size, &w, err, errn) != 0)
        return -1;
    for (int q = 0; q < QPOS; q++) {
        uint8_t idx = w.tmap[q];
        if (idx == 0xFF || idx >= QPOS)
            continue;
        if (!tracks[idx]) {
            const uint8_t *bits;
            uint32_t cells;
            if (!woz_track_bits(&w, (unsigned)q, &bits, &cells))
                continue;
            if (cells > PLAYER_MAX_CELLS || cells < 64) {
                snprintf(err, errn, "WOZ track %u: %lu bits", idx, (unsigned long)cells);
                return -1;
            }
            if (used + PLAYER_WORDS(cells) > POOL_WORDS) {
                snprintf(err, errn, "WOZ too big for the track pool");
                return -1;
            }
            tracks[idx] = pool + used;
            used += player_pack(tracks[idx], bits, cells);
        }
        qmap[q] = idx;
    }
    return 0;
}

// Make the pool match the mounted image (or nothing).
static void reload(void)
{
    image_info_t im;
    bool have = store_image(&im);
    send(CMD_PARK);
    loaded = false;
    error[0] = 0;
    loaded_row = store_mounted();
    loaded_slot = have ? im.slot : 0;
    loaded_fmt = have ? im.format : FMT_NONE;
    if (!have) {
        memset(qmap, NO_TRACK, sizeof qmap);
        printf("drive: empty\n");
        return;
    }
    uint64_t t0 = time_us_64();
    char err[96];
    if (build(&im, err, sizeof err) != 0) {
        memset(qmap, NO_TRACK, sizeof qmap);
        snprintf(error, sizeof error, "%s", err);
        printf("drive: can't serve %s: %s\n", im.name, err);
        return;
    }
    load_ms = (uint32_t)((time_us_64() - t0) / 1000);
    for (int i = 0; i < QPOS; i++)
        saved_gen[i] = trk_gen[i];              // the pool matches flash
    loaded = true;
    if (!suspended)
        send(CMD_RUN);
    printf("drive: serving %s (%lu ms)\n", im.name, (unsigned long)load_ms);
}

// ---------------------------------------------------------------- write-back

static int dirty_tracks(void)
{
    int n = 0;
    for (int i = 0; i < QPOS; i++)
        if (tracks[i] && trk_gen[i] != saved_gen[i])
            n++;
    return n;
}

// Byte k of pool track idx, as it would sit in a WOZ file (MSB-first bits).
static inline uint8_t pool_byte(int idx, size_t k)
{
    return (uint8_t)(tracks[idx][1 + k / 4] >> (24 - 8 * (k % 4)));
}

typedef struct { size_t off, len; int idx; } span_t;
static span_t spans[QPOS];
static int nspans;

// dst holds file bytes [o, o + n) from flash; lay the dirty tracks over it.
static void overlay(uint8_t *dst, size_t o, size_t n)
{
    for (int i = 0; i < nspans; i++) {
        size_t a = spans[i].off > o ? spans[i].off : o;
        size_t b = spans[i].off + spans[i].len < o + n ? spans[i].off + spans[i].len : o + n;
        for (size_t k = a; k < b; k++)
            dst[k - o] = pool_byte(spans[i].idx, k - spans[i].off);
    }
}

// Write the pool's dirty tracks back into row's image (im, which the pool
// was built from). 0 = done (or nothing to do), 1 = try again later, -1 =
// failed (message in flush_msg).
static int writeback(int row, const image_info_t *im)
{
    static uint32_t g[QPOS];
    static bool d[QPOS];
    bool any = false;
    for (int i = 0; i < QPOS; i++) {
        d[i] = tracks[i] && trk_gen[i] != saved_gen[i];
        g[i] = trk_gen[i];
        if (d[i] && (g[i] & 1))
            return 1;                           // being written right now
        any |= d[i];
    }
    if (!any)
        return 0;
    uint64_t t0 = time_us_64();
    char err[96];
    unsigned lost = 0;

    if (store_write_begin(im->size, api_reader_mask(), err, sizeof err)) {
        snprintf(flush_msg, sizeof flush_msg, "%s", err);
        return 1;
    }
    if (im->format == FMT_DSK) {
        for (unsigned t = 0; t < GCR_TRACKS; t++) {
            memcpy(enc, im->data + t * GCR_TRACK_DATA, GCR_TRACK_DATA);
            if (d[t]) {
                uint32_t m = gcr_decode_track(tracks[t] + 1, tracks[t][0] + 1, enc);
                if (m != 0xFFFF) {
                    lost += 16 - (unsigned)__builtin_popcount(m);
                    printf("drive: track %u: sectors %04lx did not decode; kept as they were\n",
                           t, (unsigned long)(~m & 0xFFFF));
                }
            }
            if (store_write_chunk(enc, GCR_TRACK_DATA, err, sizeof err))
                goto fail;
            diag_feed();
        }
        // Junk after the disk (DSK_MAX_EXTRA): carried over unchanged.
        if (im->size > DSK_SIZE &&
            store_write_chunk(im->data + DSK_SIZE, im->size - DSK_SIZE, err, sizeof err))
            goto fail;
    } else {
        static woz_info_t w;
        if (woz_parse(im->data, im->size, &w, err, sizeof err))
            goto fail;
        nspans = 0;
        for (int i = 0; i < QPOS; i++) {
            uint32_t nb;
            if (d[i] && woz_trk_span(&w, (unsigned)i, &spans[nspans].off, &nb)) {
                spans[nspans].len = (nb + 7) / 8;
                spans[nspans].idx = i;
                nspans++;
            }
        }
        // The CRC covers bytes 12..end of the new file: one pass to compute
        // it, a second to write.
        uint32_t crc = 0;
        for (size_t o = 12; o < im->size; o += 4096) {
            size_t n = im->size - o < 4096 ? im->size - o : 4096;
            memcpy(enc, im->data + o, n);
            overlay(enc, o, n);
            crc = crc32_update(crc, enc, n);
        }
        for (size_t o = 0; o < im->size; o += 4096) {
            size_t n = im->size - o < 4096 ? im->size - o : 4096;
            memcpy(enc, im->data + o, n);
            overlay(enc, o, n);
            if (o == 0)
                memcpy(enc + 8, (uint8_t[4]){ (uint8_t)crc, (uint8_t)(crc >> 8),
                                              (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) }, 4);
            if (store_write_chunk(enc, n, err, sizeof err))
                goto fail;
            diag_feed();
        }
    }
    for (int i = 0; i < QPOS; i++)
        if (d[i] && trk_gen[i] != g[i]) {
            store_write_abort();                // written to meanwhile: next time
            return 1;
        }
    if (store_write_finish(row, im->name, clock_unix(), err, sizeof err))
        goto fail;
    for (int i = 0; i < QPOS; i++)
        if (d[i])
            saved_gen[i] = g[i];
    image_info_t now;
    if (store_row(row, &now))
        loaded_slot = now.slot;                 // the pool already holds this image
    flush_count++;
    flush_lost_sectors += lost;
    flush_ms = (uint32_t)((time_us_64() - t0) / 1000);
    snprintf(flush_msg, sizeof flush_msg, "saved %s (%lu ms)", im->name, (unsigned long)flush_ms);
    printf("drive: %s\n", flush_msg);
    return 0;

fail:
    store_write_abort();
    flush_fail++;
    snprintf(flush_msg, sizeof flush_msg, "save failed: %s", err);
    printf("drive: %s\n", flush_msg);
    return -1;
}

void drive_poll(void)
{
    // Activity LED first: it then holds "on" through a write-back below
    // (dirty is already set), and goes off on the first pass after it.
    // Until the first WiFi join it stays on (see PIN_LED); not in setup
    // mode (Barry): there it is lit only until setup mode is decided (~2 s).
    static bool boot_lamp = true;
    if (boot_lamp && (net_is_up() || net_setup_mode()))
        boot_lamp = false;
    gpio_put(PIN_LED, boot_lamp || pub_en || pub_writing || (loaded && dirty_tracks()));

    image_info_t im;
    bool have = store_image(&im);
    int row = store_mounted();
    bool changed = row != loaded_row || (have && (im.slot != loaded_slot || im.format != loaded_fmt));

    if (loaded && dirty_tracks() && !api_store_busy() && !scratch_lent) {
        // Write back what the Apple wrote: after a quiet spell, or at once
        // before the pool is rebuilt for another image (if the row still
        // holds the image the writes were made to; an upload over it wins).
        image_info_t old;
        bool same = store_row(loaded_row, &old) && old.slot == loaded_slot;
        bool quiet = !pub_writing && timer_hw->timerawl - pub_write_end > WRITEBACK_QUIET_MS * 1000u;
        if (changed && same) {
            send(CMD_PARK);                     // ends any write; no more into this pool
            if (writeback(loaded_row, &old) != 0)
                printf("drive: writes to %s NOT saved (%s)\n", old.name, flush_msg);
        } else if (!changed && quiet && time_reached(flush_retry_at)) {
            int rc = writeback(row, &im);
            if (rc != 0)
                flush_retry_at = make_timeout_time_ms(rc > 0 ? 500 : 5000);
            return;
        }
    }
    if (changed) {
        reload();
        return;
    }
    // Write protect only while nothing is served.
    want_wp = !loaded;

    // Idle: prepare a free image space so the next write-back only programs.
    if (!api_store_busy() && !pub_writing && time_reached(prepare_at)) {
        bool ready = store_prepare_free(api_reader_mask());
        prepare_at = make_timeout_time_ms(ready ? 2000 : 250);
    }
}

void drive_suspend(void)
{
    if (suspended)
        return;
    suspended = true;
    send(CMD_PARK);
}

void drive_resume(void)
{
    if (!suspended)
        return;
    suspended = false;
    if (loaded)
        send(CMD_RUN);
}

uint8_t *drive_scratch(void) { scratch_lent = true; return enc; }
void drive_scratch_release(void) { scratch_lent = false; }

const uint32_t *drive_track_at(int q)
{
    if (q < 0 || q >= QPOS || qmap[q] == NO_TRACK)
        return NULL;
    return tracks[qmap[q]];
}

void drive_reset_loop_max(void)
{
    gap_reset = true;
    while (gap_reset)
        tight_loop_contents();
}

const char *drive_error(void) { return error; }
const char *drive_save_text(void) { return flush_msg; }

void drive_get_status(drive_status_t *s)
{
    s->loaded = loaded;
    s->enabled = pub_en;
    s->qtrack = pub_q;
    s->track_index = pub_track;
    s->steps = pub_steps;
    s->selects = pub_selects;
    s->load_ms = load_ms;
    s->loop_max_us = pub_max_gap_us;
    s->writable = !want_wp;
    s->writing = pub_writing;
    s->writes = pub_writes;
    s->dirty = dirty_tracks();
    s->saves = flush_count;
    s->save_failures = flush_fail;
    s->lost_sectors = flush_lost_sectors;
    s->write_cell_ns = ticks_per_us ? pub_write_cell16 * 1000u / WRDEC_FRAC / ticks_per_us : 0;
    s->fifo_full = pub_fifo_full;

}

size_t drive_status_json(char *buf, size_t n)
{
    drive_status_t s;
    drive_get_status(&s);
    static char e[sizeof error * 6];        // a WOZ error can quote file bytes
    static char sm[sizeof flush_msg * 2];
    json_escape(e, sizeof e, error);
    json_escape(sm, sizeof sm, flush_msg);
    // The connector as the Pico sees it now (3.3 V side of U1/U2), for
    // bring-up: phases PH0..PH3, ENABLE*, WRREQ*, WRDATA in; WRPROT, RDDATA out.
    uint32_t in = sio_hw->gpio_in;
    char pins[72];
    snprintf(pins, sizeof pins, "PH0-3=%lu%lu%lu%lu EN*=%lu WRREQ*=%lu WRDATA=%lu WRPROT=%lu RDDATA=%lu LED=%lu",
             (unsigned long)(in >> 2 & 1), (unsigned long)(in >> 3 & 1),
             (unsigned long)(in >> 4 & 1), (unsigned long)(in >> 5 & 1),
             (unsigned long)(in >> PIN_ENABLE & 1), (unsigned long)(in >> PIN_WRREQ & 1),
             (unsigned long)(in >> PIN_WRDATA & 1), (unsigned long)(in >> PIN_WRPROT & 1),
             (unsigned long)(in >> 10 & 1), (unsigned long)(in >> PIN_LED & 1));
    int len = snprintf(buf, n,
        "{\"pins\":\"%s\",\"serving\":%s,\"enabled\":%s,\"track\":%d.%02d,\"quarter_track\":%d,"
        "\"on_data\":%s,\"steps\":%lu,\"selects\":%lu,\"load_ms\":%lu,\"loop_max_us\":%lu,"
        "\"rddata_idle\":\"%s\",\"writable\":%s,\"writing\":%s,\"writes\":%lu,"
        "\"last_write_bits\":%lu,\"write_cell_ns\":%lu,\"fifo_full\":%lu,\"gap_resyncs\":%lu,\"dirty_tracks\":%d,"
        "\"saves\":%lu,\"save_failures\":%lu,\"lost_sectors\":%lu,\"save\":\"%s\","
        "\"error\":\"%s\"}",
        pins, s.loaded ? "true" : "false", s.enabled ? "true" : "false",
        s.qtrack / 4, 25 * (s.qtrack % 4), s.qtrack, s.track_index >= 0 ? "true" : "false",
        (unsigned long)s.steps, (unsigned long)s.selects, (unsigned long)s.load_ms,
        (unsigned long)s.loop_max_us,
        player_idle_high() ? "high" : "low", s.writable ? "true" : "false",
        s.writing ? "true" : "false", (unsigned long)s.writes, (unsigned long)pub_write_bits,
        (unsigned long)s.write_cell_ns, (unsigned long)s.fifo_full, (unsigned long)pub_resyncs, s.dirty,
        (unsigned long)s.saves, (unsigned long)s.save_failures, (unsigned long)s.lost_sectors,
        sm, e);
    if (len < 0)
        len = 0;
    if ((size_t)len >= n)
        len = (int)n - 1;
    return (size_t)len;
}

void drive_oled_line(char *buf, size_t n)
{
    drive_status_t s;
    drive_get_status(&s);
    if (error[0]) {
        snprintf(buf, n, "%s", error);
        return;
    }
    static const char *frac[4] = { "", ".25", ".5", ".75" };
    // Track only (Barry, 2026-11-19): selected/writing/unsaved is the
    // activity LED's job.
    snprintf(buf, n, "Track %d%s", s.qtrack / 4, frac[s.qtrack % 4]);
}

bool drive_angle(uint32_t *cell, uint32_t *cells, uint32_t *at_us)
{
    uint32_t s;
    do {
        s = ang_seq;
        __dmb();
        *cell = ang_cell;
        *cells = ang_cells;
        *at_us = ang_time;
        __dmb();
    } while ((s & 1) || s != ang_seq);
    return s != 0;
}
