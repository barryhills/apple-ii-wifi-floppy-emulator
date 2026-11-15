// Host test of the firmware's GCR track decoder against its encoder.
//   cc -O2 -Wall -Wextra -I../../firmware/src -o gcrdec_test gcrdec_test.c \
//      ../../firmware/src/gcr.c && ./gcrdec_test [image.dsk]
// For every track: encode, decode, compare all 16 sectors; also with the
// track rotated (fields straddling the end), with a damaged data field
// (that sector must be reported missing, the others still found), and with
// one data field rewritten the way DOS does (5 syncs + data field written
// over the old one from just after the address field).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gcr.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t words[GCR_TRACK_BUF / 4 + 2];

static void pack(const uint8_t *bits, uint32_t cells, uint32_t rot)
{
    memset(words, 0, sizeof words);
    for (uint32_t i = 0; i < cells; i++) {
        uint32_t j = (i + rot) % cells;
        if (bits[j >> 3] >> (7 - (j & 7)) & 1)
            words[i >> 5] |= 1u << (31 - (i & 31));
    }
}
static int getb(uint32_t i) { return words[i >> 5] >> (31 - (i & 31)) & 1; }
static void setb(uint32_t i, int v)
{
    if (v) words[i >> 5] |= 1u << (31 - (i & 31));
    else   words[i >> 5] &= ~(1u << (31 - (i & 31)));
}

int main(int argc, char **argv)
{
    static uint8_t dsk[GCR_TRACKS * GCR_TRACK_DATA], enc[GCR_TRACK_BUF], out[GCR_TRACK_DATA];
    srand(1);
    for (size_t i = 0; i < sizeof dsk; i++)
        dsk[i] = (uint8_t)rand();
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f || fread(dsk, 1, sizeof dsk, f) != sizeof dsk) { printf("can't read %s\n", argv[1]); return 2; }
        fclose(f);
    }
    for (unsigned t = 0; t < GCR_TRACKS; t++) {
        const uint8_t *td = dsk + t * GCR_TRACK_DATA;
        uint32_t cells = (uint32_t)gcr_encode_track(enc, td, t, 254);
        for (uint32_t rot = 0; rot < cells; rot += cells / 7 + 1) {
            pack(enc, cells, rot);
            memset(out, 0, sizeof out);
            uint32_t m = gcr_decode_track(words, cells, out);
            CHECK(m == 0xFFFF, "track %u rot %u: mask %04x", t, rot, m);
            CHECK(memcmp(out, td, sizeof out) == 0, "track %u rot %u: data differs", t, rot);
        }
        // Damage one nibble in the middle of physical sector 5's data field:
        // find its data prologue (6th D5 AA AD) by decoding positions.
        pack(enc, cells, 0);
        uint32_t reg = 0, seen = 0, at = 0;
        int b0 = 0, b1 = 0;
        for (uint32_t i = 0; i < cells && !at; i++) {
            reg = reg << 1 | (uint32_t)getb(i);
            if (reg & 0x80) {
                if (b0 == 0xD5 && b1 == 0xAA && reg == 0xAD && seen++ == 5) at = i;
                b0 = b1; b1 = (int)reg; reg = 0;
            }
        }
        CHECK(at, "track %u: no 6th data field", t);
        // Wipe 20 nibbles: the reader runs into the epilogue, whose AA is not a
        // valid data nibble. (A small corruption can pass DOS's 6-bit checksum
        // 1 time in 64: that is the format, not the decoder.)
        for (int k = 0; k < 160; k++) setb(at + 800 + k, 0);
        uint32_t m = gcr_decode_track(words, cells, out);
        uint32_t want = 0xFFFF & ~(1u << gcr_phys_to_logical[5]);
        CHECK(m == want, "track %u damaged: mask %04x want %04x", t, m, want);

        // DOS-style rewrite of physical sector 5 with new data: from 2 cells
        // after the address epilogue's AA, 5 self-syncs, then the field.
        pack(enc, cells, 0);
        uint8_t nd[256], fld[GCR_TRACK_BUF];
        for (int i = 0; i < 256; i++) nd[i] = (uint8_t)(i * 7 + t);
        // Encode a whole track of nd-filled sectors just to borrow one data
        // field's bits: track with every sector = nd, take physical 5's field.
        static uint8_t tmp[GCR_TRACK_DATA];
        for (int s = 0; s < 16; s++) memcpy(tmp + 256 * s, nd, 256);
        uint32_t c2 = (uint32_t)gcr_encode_track(fld, tmp, t, 254);
        (void)c2;
        // In both encodings the fields sit at the same cells (same lengths),
        // so copy physical sector 5's data field (prologue to epilogue) over.
        // Find its start in fld the same way.
        uint32_t f_at = 0; seen = 0; reg = 0; b0 = b1 = 0;
        for (uint32_t i = 0; i < cells && !f_at; i++) {
            reg = reg << 1 | (uint32_t)(fld[i >> 3] >> (7 - (i & 7)) & 1);
            if (reg & 0x80) {
                if (b0 == 0xD5 && b1 == 0xAA && reg == 0xAD && seen++ == 5) f_at = i - 23;
                b0 = b1; b1 = (int)reg; reg = 0;
            }
        }
        // DOS's write lands a little off the old field: 5 self-syncs
        // (FF + two 0 bits) then the field, here 7 cells earlier than before.
        uint32_t len = 24 + 343 * 8 + 24, w0 = f_at - 7 - 50;
        for (uint32_t k = 0; k < 50; k++)
            setb(w0 + k, k % 10 < 8);
        for (uint32_t k = 0; k < len; k++)
            setb(w0 + 50 + k, fld[(f_at + k) >> 3] >> (7 - ((f_at + k) & 7)) & 1);
        m = gcr_decode_track(words, cells, out);
        CHECK(m == 0xFFFF, "track %u rewritten: mask %04x", t, m);
        CHECK(memcmp(out + 256 * gcr_phys_to_logical[5], nd, 256) == 0, "track %u rewritten: new data wrong", t);
        for (int s = 0; s < 16; s++)
            if (s != gcr_phys_to_logical[5])
                CHECK(memcmp(out + 256 * s, td + 256 * s, 256) == 0, "track %u rewritten: sector %d changed", t, s);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "all GCR decode tests passed", fails);
    return fails != 0;
}
