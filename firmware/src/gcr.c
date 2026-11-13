// DOS 3.3 16-sector (6-and-2) track encoder.
//
// Sources:
//  [BAD]  Beneath Apple DOS, ch. 3 (text: git.applefritter.com/Apple-2-SW/
//         beneath-apple-dos ch03.txt): field layout, prologues/epilogues,
//         4-and-4 address encoding, XOR-chained data checksum, 10-bit
//         self-sync FF, gap sizes, the rules defining valid 6-and-2 disk bytes.
//  [D2W]  dsk2woz.c by Thomas Harte (MIT licence): 6-and-2 prenibble layout
//         (86 two-bit bytes, bit pairs swapped, then 256 six-bit bytes).
//  [RWTS] DOS 3.3 RWTS interleave table at $BFB8 (BAD ch. 8 listing;
//         Elite for Apple II disassembly, elite.bbcelite.com).
//
// Verified 2026-11-13: the 64-entry table below equals the list derived from
// [BAD]'s rules, and gcr_phys_to_logical is the exact inverse of the [RWTS]
// table (table[logical] = physical). The prenibble bit layout rests on [D2W]
// alone; the Virtual ][ boot/catalog test is its check.

#include <string.h>
#include "gcr.h"

// Gap lengths in self-sync bytes. [BAD]: gap 2 is 5-10 bytes ("six self-sync
// bytes provide [ample time]"), gap 3 is 14-24, every gap needs >= 5.
// Chosen so a track is ~50,000 bits: one revolution at 300 rpm in 4 us cells.
#define GAP1    16
#define GAP2    6
#define GAP3    16

// 6-bit value -> disk byte [BAD rules; equals D2W table].
static const uint8_t gcr62[64] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
    0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
    0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
    0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
    0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

// Inverse of RWTS $BFB8 {00 0D 0B 09 07 05 03 01 0E 0C 0A 08 06 04 02 0F}.
const uint8_t gcr_phys_to_logical[GCR_SECTORS] = {
    0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15,
};

typedef struct {
    uint8_t *buf;
    size_t   pos;       // in bits
    size_t   cap;       // in bits
} bits_t;

static void put_bits(bits_t *w, uint32_t v, int n)     // n bits, MSB first
{
    while (n--) {
        if (w->pos < w->cap && ((v >> n) & 1))
            w->buf[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
        w->pos++;
    }
}

static void put_byte(bits_t *w, uint8_t b) { put_bits(w, b, 8); }

// [BAD]: a self-sync is FF followed by two zero bits (10-bit byte).
static void put_syncs(bits_t *w, int n)
{
    while (n--)
        put_bits(w, 0xFF << 2, 10);
}

// [BAD]: odd bits first, then even bits; unused bits set to one.
static void put_4and4(bits_t *w, uint8_t v)
{
    put_byte(w, (uint8_t)((v >> 1) | 0xAA));
    put_byte(w, (uint8_t)(v | 0xAA));
}

// 256 data bytes -> 343 disk bytes (342 + checksum). Layout per [D2W];
// XOR chaining per [BAD] (each written value is the XOR of consecutive
// 6-bit values; the last is the checksum).
static void encode_6and2(uint8_t out[343], const uint8_t *src)
{
    static const uint8_t swap2[4] = { 0, 2, 1, 3 };
    uint8_t v[342];

    for (int i = 0; i < 86; i++) {
        uint8_t x = swap2[src[i] & 3] | (uint8_t)(swap2[src[i + 86] & 3] << 2);
        if (i + 172 < 256)
            x |= (uint8_t)(swap2[src[i + 172] & 3] << 4);
        v[i] = x;
    }
    for (int i = 0; i < 256; i++)
        v[86 + i] = src[i] >> 2;

    uint8_t prev = 0;
    for (int i = 0; i < 342; i++) {
        out[i] = gcr62[v[i] ^ prev];
        prev = v[i];
    }
    out[342] = gcr62[prev];
}

size_t gcr_encode_track(uint8_t out[GCR_TRACK_BUF], const uint8_t *dsk_track,
                        unsigned track, unsigned volume)
{
    memset(out, 0, GCR_TRACK_BUF);
    bits_t w = { out, 0, (size_t)GCR_TRACK_BUF * 8 };
    uint8_t nib[343];

    put_syncs(&w, GAP1);
    for (unsigned p = 0; p < GCR_SECTORS; p++) {
        // Address field [BAD]: D5 AA 96, vol trk sec chk (4-and-4), DE AA EB.
        put_byte(&w, 0xD5); put_byte(&w, 0xAA); put_byte(&w, 0x96);
        put_4and4(&w, (uint8_t)volume);
        put_4and4(&w, (uint8_t)track);
        put_4and4(&w, (uint8_t)p);
        put_4and4(&w, (uint8_t)(volume ^ track ^ p));
        put_byte(&w, 0xDE); put_byte(&w, 0xAA); put_byte(&w, 0xEB);

        put_syncs(&w, GAP2);

        // Data field [BAD]: D5 AA AD, 342 + checksum, DE AA EB.
        put_byte(&w, 0xD5); put_byte(&w, 0xAA); put_byte(&w, 0xAD);
        encode_6and2(nib, dsk_track + 256u * gcr_phys_to_logical[p]);
        for (int i = 0; i < 343; i++)
            put_byte(&w, nib[i]);
        put_byte(&w, 0xDE); put_byte(&w, 0xAA); put_byte(&w, 0xEB);

        put_syncs(&w, GAP3);
    }
    return w.pos;
}
