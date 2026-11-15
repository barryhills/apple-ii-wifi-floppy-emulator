#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The drive: what the Disk II controller sees on the 20-pin connector.
//
// Core 1 runs the drive loop, entirely from RAM (it keeps running while
// core 0 writes flash, which stalls flash reads): it watches ENABLE* and the
// four phase lines, moves the head (stepper.h), and points the read-data
// player (player.h) at the track under the head, keeping the disk's angle
// across head moves. WRPROT is driven high (protected) until writes exist
// (milestone 6).
//
// Core 0 owns everything else: when the mounted image changes it parks
// core 1, builds every track of the image in a RAM pool (a .dsk is encoded
// to 6-and-2 GCR, 35 tracks; a WOZ's tracks are copied), and lets core 1 go
// on. A head step is then only a DMA repoint.

void drive_init(void);      // core 0, once: GPIO, player, launch core 1
void drive_poll(void);      // core 0 main loop: follows the mounted image

// Hand the player to core 0 (bench tests) and back. While suspended the
// drive is silent; the head model keeps running.
void drive_suspend(void);
void drive_resume(void);

typedef struct {
    bool     loaded;        // an image is in the pool and being served
    bool     enabled;       // ENABLE* active now
    int      qtrack;        // head position, quarter tracks 0..159
    int      track_index;   // track playing (pool index), -1 = none (silence)
    uint32_t steps;         // head moves since boot
    uint32_t selects;       // ENABLE* falling edges since boot
    uint32_t load_ms;       // time the last load took
    uint32_t loop_max_us;   // longest core-1 loop pass since boot or reset
    char     error[96];     // why the mounted image is not served, or ""
} drive_status_t;

void drive_get_status(drive_status_t *s);
void drive_reset_loop_max(void);
size_t drive_status_json(char *buf, size_t n);          // a JSON object
void drive_oled_line(char *buf, size_t n);              // e.g. "Track 17  ON"

// Bench self-test hook (benchtest.c). The cell under the head now, as core 1 reckons it; false if core 1 has
// not started a track yet.
bool drive_angle(uint32_t *cell, uint32_t *cells, uint32_t *at_us);
