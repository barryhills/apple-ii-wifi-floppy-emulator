// WOZ 2 writer. Field offsets from the WOZ 2 reference
// (applesaucefdc.com/woz/reference2, fetched 2026-11-13). Little-endian.

#include <string.h>
#include "crc32.h"
#include "woz.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void woz_header(uint8_t h[WOZ_HEADER_BYTES], const uint32_t bits[GCR_TRACKS],
                uint32_t crc, const char *creator, int write_protected)
{
    memset(h, 0, WOZ_HEADER_BYTES);

    // File header: "WOZ2" FF 0A 0D 0A, CRC32 of bytes 12..end.
    memcpy(h, "WOZ2\xFF\x0A\x0D\x0A", 8);
    put32(h + 8, crc);

    // INFO chunk at 12, 60 bytes of data at 20.
    memcpy(h + 12, "INFO", 4);
    put32(h + 16, 60);
    uint8_t *info = h + 20;
    info[0] = 2;                        // INFO version 2 (no FLUX fields)
    info[1] = 1;                        // 5.25"
    info[2] = write_protected ? 1 : 0;
    info[3] = 0;                        // not cross-track synchronized
    info[4] = 1;                        // cleaned: no fake MC3470 bits
    memset(info + 5, ' ', 32);          // creator, space padded, no NUL
    size_t cl = strlen(creator);
    memcpy(info + 5, creator, cl > 32 ? 32 : cl);
    info[37] = 1;                       // disk sides
    info[38] = 1;                       // boot sector format: 16-sector
    info[39] = 32;                      // optimal bit timing: 32 x 125 ns = 4 us
    put16(info + 40, 0);                // compatible hardware: unknown
    put16(info + 42, 0);                // required RAM: unknown
    put16(info + 44, WOZ_TRACK_BLOCKS); // largest track, in blocks

    // TMAP chunk at 80, 160 quarter-track entries at 88. Track t is read at
    // quarter tracks 4t-1, 4t, 4t+1 (spec example); 4t+2 is empty (FF).
    memcpy(h + 80, "TMAP", 4);
    put32(h + 84, 160);
    uint8_t *tmap = h + 88;
    memset(tmap, 0xFF, 160);
    for (int t = 0; t < GCR_TRACKS; t++) {
        if (t > 0)
            tmap[4 * t - 1] = (uint8_t)t;
        tmap[4 * t] = (uint8_t)t;
        tmap[4 * t + 1] = (uint8_t)t;
    }

    // TRKS chunk at 248, data at 256: 160 x 8-byte TRK entries, then the
    // bit data from block 3 (byte 1536). Unused entries stay zero.
    memcpy(h + 248, "TRKS", 4);
    put32(h + 252, (uint32_t)(WOZ_FILE_BYTES - 256));
    for (int t = 0; t < GCR_TRACKS; t++) {
        uint8_t *e = h + 256 + 8 * t;
        put16(e, (uint16_t)(3 + t * WOZ_TRACK_BLOCKS));     // starting block
        put16(e + 2, WOZ_TRACK_BLOCKS);                     // block count
        put32(e + 4, bits[t]);                              // bit count
    }
}

uint32_t woz_prepare(const uint8_t *dsk, uint32_t bits[GCR_TRACKS],
                     uint8_t *scratch, const char *creator, int write_protected)
{
    static uint8_t hdr[WOZ_HEADER_BYTES];  // static: firmware calls this on the IRQ stack
    uint32_t crc = 0;

    for (int t = 0; t < GCR_TRACKS; t++)
        bits[t] = (uint32_t)gcr_encode_track(scratch, dsk + (size_t)t * GCR_TRACK_DATA,
                                             (unsigned)t, GCR_DEFAULT_VOLUME);
    woz_header(hdr, bits, 0, creator, write_protected);
    crc = crc32_update(crc, hdr + 12, WOZ_HEADER_BYTES - 12);
    for (int t = 0; t < GCR_TRACKS; t++) {
        gcr_encode_track(scratch, dsk + (size_t)t * GCR_TRACK_DATA,
                         (unsigned)t, GCR_DEFAULT_VOLUME);
        crc = crc32_update(crc, scratch, WOZ_TRACK_BYTES);
    }
    return crc;
}
