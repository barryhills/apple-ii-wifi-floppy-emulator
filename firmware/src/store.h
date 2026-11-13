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
//   0x110000 - end       image spaces, 256 KB each (room for WOZ later)
//
// The directory is written alternately to A and B with an increasing sequence
// number and a CRC32; at boot the newest valid copy wins, so a power cut
// during an update leaves the previous directory intact. A new image is
// always written to a free image space before the directory switches to it.
//
// For now the directory holds a single "current" image; the format already
// has room for several.

#define DSK_SIZE        143360u     // 35 tracks x 16 sectors x 256 bytes
#define IMAGE_NAME_MAX  64          // including NUL

typedef struct {
    char           name[IMAGE_NAME_MAX];
    uint32_t       size;
    uint8_t        sha256[32];
    int64_t        uploaded;        // Unix seconds; 0 = unknown
    bool           verified;        // SHA-256 of the flash contents matches
    const uint8_t *data;            // memory-mapped flash (XIP)
} image_info_t;

void store_init(void);
bool store_image(image_info_t *out);   // false if no image is stored

// Write buf as the new current image. Uses flash_safe_execute (interrupts
// off per 4 KB sector), so call from the main loop, never from lwIP callbacks.
// Returns 0 on success, otherwise -1 with a message in err.
int  store_commit(const uint8_t *buf, uint32_t size, const char *name,
                  int64_t uploaded, char *err, size_t errn);

void sha256_bytes(const uint8_t *data, size_t n, uint8_t out[32]);
void sha256_hex(const uint8_t h[32], char out[65]);
bool sha256_selftest(void);
