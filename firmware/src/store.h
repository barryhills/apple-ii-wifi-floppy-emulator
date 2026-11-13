#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Flash storage for disk images.
//
// Layout (flash offsets; flash is mapped at XIP_BASE):
//   0x000000 - 0x0FFFFF  firmware (checked at boot)
//   0x100000             image directory, copy A  (4 KB sector)
//   0x101000             image directory, copy B
//   0x102000             reserved: stored config (next step)
//   0x110000 - end       image spaces, 256 KB each (.dsk or .woz)
//
// The directory is written alternately to A and B with an increasing sequence
// number and a CRC32; at boot the newest valid copy wins, so a power cut
// during an update leaves the previous directory intact. A new image is
// always written to a free image space before the directory switches to it.
//
// For now the directory holds a single "current" (mounted) image; the
// format already has room for several.

#define DSK_SIZE        143360u     // 35 tracks x 16 sectors x 256 bytes
#define IMAGE_NAME_MAX  64          // including NUL
#define IMAGE_MAX_SIZE  (256u * 1024u)
#define STORE_SLOTS_MAX 32          // upper bound for "avoid" masks

typedef enum { FMT_NONE = 0, FMT_DSK = 1, FMT_WOZ1 = 2, FMT_WOZ2 = 3 } image_format_t;

typedef struct {
    char           name[IMAGE_NAME_MAX];
    uint32_t       size;
    image_format_t format;
    uint8_t        sha256[32];
    int64_t        uploaded;        // Unix seconds; 0 = unknown
    bool           verified;        // SHA-256 of the flash contents matches
    unsigned       slot;            // image space (internal)
    const uint8_t *data;            // memory-mapped flash (XIP)
} image_info_t;

void store_init(void);
bool store_image(image_info_t *out);   // false if no image is stored
const char *store_format_name(image_format_t f);    // "dsk", "woz1", "woz2"

// Streaming write of a new image into a free image space (never the
// mounted one, nor any space in avoid_mask). Flash is erased and programmed
// one 4 KB sector at a time with interrupts off, so call these from the main
// loop, never from lwIP callbacks. finish() validates the contents (a
// 143,360-byte .dsk, or a WOZ 1/2 that parses), then switches the directory;
// on any failure the mounted image is untouched. Return 0 or -1 with err.
int  store_write_begin(uint32_t size, uint32_t avoid_mask, char *err, size_t errn);
int  store_write_chunk(const uint8_t *data, size_t n, char *err, size_t errn);
int  store_write_finish(const char *name, int64_t uploaded, char *err, size_t errn);
void store_write_abort(void);

void sha256_bytes(const uint8_t *data, size_t n, uint8_t out[32]);
void sha256_hex(const uint8_t h[32], char out[65]);
bool sha256_selftest(void);
