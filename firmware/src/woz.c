// WOZ 2 writer. Field offsets from the WOZ 2 reference
// (applesaucefdc.com/woz/reference2, fetched 2026-11-13). Little-endian.

#include <stdio.h>
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
    unsigned vol = gcr_dsk_volume(dsk);

    for (int t = 0; t < GCR_TRACKS; t++)
        bits[t] = (uint32_t)gcr_encode_track(scratch, dsk + (size_t)t * GCR_TRACK_DATA,
                                             (unsigned)t, vol);
    woz_header(hdr, bits, 0, creator, write_protected);
    crc = crc32_update(crc, hdr + 12, WOZ_HEADER_BYTES - 12);
    for (int t = 0; t < GCR_TRACKS; t++) {
        gcr_encode_track(scratch, dsk + (size_t)t * GCR_TRACK_DATA,
                         (unsigned)t, vol);
        crc = crc32_update(crc, scratch, WOZ_TRACK_BYTES);
    }
    return crc;
}

// ---------------------------------------------------------------- reader

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

#define WOZ1_TRK_BYTES   6656       // WOZ 1: fixed-size TRK records
#define WOZ1_BITS_BYTES  6646       //   of which the bit stream
#define WOZ1_BIT_COUNT   6648       //   offset of the uint16 bit count

int woz_track_bits(const woz_info_t *w, unsigned q, const uint8_t **bits, uint32_t *nbits)
{
    if (q >= 160 || w->tmap[q] == 0xFF)
        return 0;
    unsigned idx = w->tmap[q];
    if (w->version == 1) {
        if ((size_t)(idx + 1) * WOZ1_TRK_BYTES > w->trks_len)
            return 0;
        const uint8_t *rec = w->trks + (size_t)idx * WOZ1_TRK_BYTES;
        uint32_t n = rd16(rec + WOZ1_BIT_COUNT);
        if (n == 0 || n > WOZ1_BITS_BYTES * 8u)
            return 0;
        *bits = rec;
        *nbits = n;
        return 1;
    }
    if (idx >= 160 || w->trks_len < 160 * 8)
        return 0;
    const uint8_t *e = w->trks + 8 * idx;
    uint32_t start = rd16(e), count = rd16(e + 2), n = rd32(e + 4);
    if (start < 3 || count == 0 || (size_t)(start + count) * 512 > w->len ||
        n == 0 || n > count * 4096u)
        return 0;
    *bits = w->file + (size_t)start * 512;
    *nbits = n;
    return 1;
}

int woz_parse(const uint8_t *f, size_t len, woz_info_t *w, char *err, size_t errn)
{
    memset(w, 0, sizeof *w);
    w->file = f;
    w->len = len;
    if (len < 256) {
        snprintf(err, errn, "too short for a WOZ file");
        return -1;
    }
    if (memcmp(f, "WOZ1\xFF\x0A\x0D\x0A", 8) == 0)
        w->version = 1;
    else if (memcmp(f, "WOZ2\xFF\x0A\x0D\x0A", 8) == 0)
        w->version = 2;
    else {
        snprintf(err, errn, "not a WOZ file (bad signature)");
        return -1;
    }
    uint32_t crc = rd32(f + 8);
    if (crc != 0 && crc32_update(0, f + 12, len - 12) != crc) {
        snprintf(err, errn, "WOZ CRC mismatch: file is damaged or truncated");
        return -1;
    }

    const uint8_t *info = NULL;
    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t *id = f + off;
        uint32_t sz = rd32(f + off + 4);
        size_t data = off + 8;
        if (sz > len - data) {
            snprintf(err, errn, "WOZ chunk %.4s runs past the end of the file", (const char *)id);
            return -1;
        }
        if (memcmp(id, "INFO", 4) == 0 && sz >= 60)
            info = f + data;
        else if (memcmp(id, "TMAP", 4) == 0 && sz >= 160)
            w->tmap = f + data;
        else if (memcmp(id, "TRKS", 4) == 0) {
            w->trks = f + data;
            w->trks_len = sz;
        }
        off = data + sz;
    }
    if (!info || !w->tmap || !w->trks) {
        snprintf(err, errn, "WOZ file lacks an INFO, TMAP or TRKS chunk");
        return -1;
    }
    if (info[1] != 1) {
        snprintf(err, errn, "only 5.25-inch WOZ images are supported (disk type %u)", info[1]);
        return -1;
    }
    w->write_protected = info[2];

    uint8_t seen[160] = { 0 };
    for (unsigned q = 0; q < 160; q++) {
        const uint8_t *b;
        uint32_t n;
        if (w->tmap[q] == 0xFF)
            continue;
        if (!woz_track_bits(w, q, &b, &n)) {
            snprintf(err, errn, "WOZ track entry %u (quarter track %u) is invalid",
                     w->tmap[q], q);
            return -1;
        }
        if (!seen[w->tmap[q]]++)
            w->tracks++;
    }
    if (w->tracks == 0) {
        snprintf(err, errn, "WOZ file has no tracks");
        return -1;
    }
    return 0;
}
