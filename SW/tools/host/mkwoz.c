// Host build of the firmware's GCR + WOZ encoder, for off-target testing.
//   cc -O2 -Wall -I../../firmware/src -o mkwoz mkwoz.c \
//      ../../firmware/src/gcr.c ../../firmware/src/woz.c ../../firmware/src/crc32.c
//   ./mkwoz in.dsk out.woz

#include <stdio.h>
#include <stdlib.h>
#include "gcr.h"
#include "woz.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s in.dsk out.woz\n", argv[0]);
        return 2;
    }
    static uint8_t dsk[GCR_TRACKS * GCR_TRACK_DATA + 1];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n = fread(dsk, 1, sizeof dsk, f);
    fclose(f);
    if (n != GCR_TRACKS * GCR_TRACK_DATA) {
        fprintf(stderr, "%s: %zu bytes, expected %d\n", argv[1], n, GCR_TRACKS * GCR_TRACK_DATA);
        return 1;
    }

    static uint8_t trk[WOZ_TRACK_BYTES];
    uint32_t bits[GCR_TRACKS];
    uint32_t crc = woz_prepare(dsk, bits, trk, "a2drive host", 0);
    uint8_t hdr[WOZ_HEADER_BYTES];
    woz_header(hdr, bits, crc, "a2drive host", 0);

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 1; }
    fwrite(hdr, 1, sizeof hdr, o);
    for (int t = 0; t < GCR_TRACKS; t++) {
        gcr_encode_track(trk, dsk + t * GCR_TRACK_DATA, (unsigned)t, gcr_dsk_volume(dsk));
        fwrite(trk, 1, sizeof trk, o);
    }
    fclose(o);
    printf("%s: %d tracks, %u bits/track (track 0), crc %08x\n",
           argv[2], GCR_TRACKS, (unsigned)bits[0], (unsigned)crc);
    return 0;
}
