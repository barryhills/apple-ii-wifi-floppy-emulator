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

// ---- Reading WOZ 1 / WOZ 2 files (applesaucefdc.com/woz/reference and
// /reference2). Works on memory-mapped flash.

typedef struct {
    int            version;         // 1 or 2
    const uint8_t *file;
    size_t         len;
    const uint8_t *tmap;            // 160 quarter-track entries
    const uint8_t *trks;            // TRKS chunk data
    size_t         trks_len;
    int            write_protected;
    int            tracks;          // distinct TRK entries referenced
} woz_info_t;

// Check signature, CRC (if non-zero), chunks, 5.25" disk type and every
// referenced track. Returns 0, or -1 with a message in err.
int  woz_parse(const uint8_t *f, size_t len, woz_info_t *w, char *err, size_t errn);

// Bit stream for quarter track q (0-159). False if that position is empty.
int  woz_track_bits(const woz_info_t *w, unsigned q, const uint8_t **bits, uint32_t *nbits);

// Encode a whole .dsk image: fills bits[] and returns the file CRC.
// scratch must hold WOZ_TRACK_BYTES.
uint32_t woz_prepare(const uint8_t *dsk, uint32_t bits[GCR_TRACKS],
                     uint8_t *scratch, const char *creator, int write_protected);
