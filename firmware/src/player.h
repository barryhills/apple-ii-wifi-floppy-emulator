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

#define PLAYER_PIN          10      // GP10 = RDDATA_3V (schematic r0.3)
#define PLAYER_MAX_CELLS    (6656 * 8)
#define PLAYER_BUF_WORDS    (2 + PLAYER_MAX_CELLS / 32)

// Build a player buffer from a bit stream packed MSB first in bytes (the
// encoder's format). Returns the number of words to play.
uint32_t player_pack(uint32_t *buf, const uint8_t *bits, uint32_t cells);

bool player_init(void);                                 // claims a PIO SM and 2 DMA channels
void player_play(const uint32_t *buf, uint32_t words);  // (re)start at cell 0
void player_stop(void);                                 // stream stops, RDDATA idle high
