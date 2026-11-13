#pragma once
#include <stddef.h>
#include <stdint.h>
#include "gcr.h"

// WOZ 2 writer for a 35-track DOS 3.3 disk (applesaucefdc.com/woz/reference2).
// Layout: 1536-byte header (12-byte file header, INFO, TMAP, TRKS chunk
// header + 160 TRK entries), then 35 track slots of 13 blocks each.

#define WOZ_HEADER_BYTES   1536
#define WOZ_TRACK_BLOCKS   13
#define WOZ_TRACK_BYTES    (WOZ_TRACK_BLOCKS * 512)     // == GCR_TRACK_BUF
#define WOZ_FILE_BYTES     (WOZ_HEADER_BYTES + GCR_TRACKS * WOZ_TRACK_BYTES)

// Fill the header. bits[t] = bit count of track t. crc covers file bytes
// 12..end (pass 0 first, compute, then call again with the real value).
void woz_header(uint8_t hdr[WOZ_HEADER_BYTES], const uint32_t bits[GCR_TRACKS],
                uint32_t crc, const char *creator, int write_protected);

// Encode a whole .dsk image: fills bits[] and returns the file CRC.
// scratch must hold WOZ_TRACK_BYTES.
uint32_t woz_prepare(const uint8_t *dsk, uint32_t bits[GCR_TRACKS],
                     uint8_t *scratch, const char *creator, int write_protected);
