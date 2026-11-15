#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Disk II read-data player: PIO (src/disk2.pio) + DMA stream a track's bit
// cells out on RDDATA at 4 us per cell, looping one revolution forever with
// no CPU involvement.
//
// Track buffer layout (player_pack builds it):
//   word 0       cells in one revolution, less one
//   words 1..n   the cells, MSB first, n = cells / 32 + 1 (disk2.pio drops the
//                rest of the last word at the end of each revolution, and
//                takes one extra word when cells is a multiple of 32)
// so a buffer is PLAYER_WORDS(cells) words.
//
// player_start/player_stop run from RAM and touch only PIO/DMA registers, so
// the core-1 drive loop may call them while core 0 is writing flash. Only
// one core may use the player at a time (drive.c hands it over).

#define PLAYER_PIN          10      // GP10 = RDDATA_3V (schematic r0.3)
#define PLAYER_MAX_CELLS    65535u  // the drive's angle arithmetic is 32-bit
#define PLAYER_WORDS(cells) ((cells) / 32 + 2)

// Build a player buffer from a bit stream packed MSB first in bytes (the
// encoder's and WOZ's format). Returns the number of words written.
uint32_t player_pack(uint32_t *buf, const uint8_t *bits, uint32_t cells);

bool player_init(void);         // core 0, once: claims a PIO SM and 2 DMA channels

// (Re)start playing buf, beginning at cell start_cell (0 <= start_cell <
// cells) and looping whole revolutions from cell 0 after that.
void player_start(const uint32_t *buf, uint32_t start_cell);
void player_stop(void);         // stream stops, RDDATA idle

// RDDATA sense at the GP10 pin. Default (false): idle low, a 1 is a 1 us
// high pulse, as the Disk II analog board gives (MC3470 data sheet,
// Fig. 28 "Digital Output": low with a positive pulse per flux reversal;
// Apple dwg 050-0007-01: MC3470 pin 10 -> 74LS125 (non-inverting) ->
// connector pin 16). true: idle high, 1 us low pulse (RP2350-Floppy-
// Emulator's choice). Implemented as a GPIO output override, so the PIO
// program is the same either way. Core 0 only.
void player_set_idle_high(bool idle_high);
bool player_idle_high(void);
