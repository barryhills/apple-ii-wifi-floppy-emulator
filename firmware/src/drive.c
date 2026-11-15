// The drive: core-1 loop (ENABLE*, stepper, read-data player) and core-0
// loading of the mounted image into a RAM track pool. See drive.h.
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

#include <stdio.h>
#include <string.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"

#include "drive.h"
#include "gcr.h"
#include "json.h"
#include "player.h"
#include "stepper.h"
#include "store.h"
#include "woz.h"

// GPIO (schematic r0.3). PH0..PH3 must stay consecutive: one read gets all.
#define PIN_PH0      2      // GP2..GP5 = PH0_3V..PH3_3V
#define PIN_ENABLE   6      // ENABLE_3V*, active low
#define PIN_WRREQ    7      // WRREQ_3V*, active low (milestone 6)
#define PIN_WRDATA   8      // WRDATA_3V (milestone 6)
#define PIN_WRPROT   11     // WRPROT_3V: high = write-protected

#define QPOS         (STEPPER_MAX_Q + 1)
#define NO_TRACK     0xFF

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

// Core 0 state.
static bool loaded, suspended;
static int loaded_row = -2;         // -2 = nothing tried yet
static unsigned loaded_slot;
static image_format_t loaded_fmt;
static uint32_t load_ms;
static char error[96];
static uint8_t enc[GCR_TRACK_BUF];

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
    gpio_put(PIN_WRPROT, 1);            // protected until milestone 6

    memset(qmap, NO_TRACK, sizeof qmap);
    if (!player_init()) {
        snprintf(error, sizeof error, "no PIO state machine for RDDATA");
        return;
    }
    multicore_launch_core1(core1_main);
    printf("drive: core 1 running, pool %u KB\n", (unsigned)(sizeof pool / 1024));
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
    loaded = true;
    if (!suspended)
        send(CMD_RUN);
    printf("drive: serving %s (%lu ms)\n", im.name, (unsigned long)load_ms);
}

void drive_poll(void)
{
    image_info_t im;
    bool have = store_image(&im);
    int row = store_mounted();
    if (row != loaded_row || (have && (im.slot != loaded_slot || im.format != loaded_fmt)))
        reload();
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

void drive_reset_loop_max(void)
{
    gap_reset = true;
    while (gap_reset)
        tight_loop_contents();
}

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
    snprintf(s->error, sizeof s->error, "%s", error);
}

size_t drive_status_json(char *buf, size_t n)
{
    drive_status_t s;
    drive_get_status(&s);
    static char e[sizeof s.error * 6];      // a WOZ error can quote file bytes
    json_escape(e, sizeof e, s.error);
    int len = snprintf(buf, n,
        "{\"serving\":%s,\"enabled\":%s,\"track\":%d.%02d,\"quarter_track\":%d,"
        "\"on_data\":%s,\"steps\":%lu,\"selects\":%lu,\"load_ms\":%lu,\"loop_max_us\":%lu,"
        "\"rddata_idle\":\"%s\",\"error\":\"%s\"}",
        s.loaded ? "true" : "false", s.enabled ? "true" : "false",
        s.qtrack / 4, 25 * (s.qtrack % 4), s.qtrack, s.track_index >= 0 ? "true" : "false",
        (unsigned long)s.steps, (unsigned long)s.selects, (unsigned long)s.load_ms,
        (unsigned long)s.loop_max_us,
        player_idle_high() ? "high" : "low", e);
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
    if (s.error[0]) {
        snprintf(buf, n, "%s", s.error);
        return;
    }
    static const char *frac[4] = { "", ".25", ".5", ".75" };
    snprintf(buf, n, "Track %d%s%s", s.qtrack / 4, frac[s.qtrack % 4], s.enabled ? "  ON" : "");
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
