#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Flash storage for disk images.
//
// Layout (flash offsets; flash is mapped at XIP_BASE):
//   0x000000 - 0x0FFFFF  firmware (checked at boot)
//   0x100000, 0x101000   image directory ring sectors 0, 1 (4 KB each)
//   0x102000             network settings (config.c); 0x103000 reserved
//   0x104000 - 0x10FFFF  image directory ring sectors 2..13
//   0x110000 - end       11 image spaces, 256 KB each (.dsk or .woz)
//
// The directory has STORE_ROWS fixed rows (what the user sees, numbered
// 0..9 here, 1..10 in the API/GUI) and records which row is mounted. Each
// change is written to the next sector of a 14-sector ring (wear levelling;
// before v0.13.2 only A and B) with an increasing sequence number and a
// CRC32; at boot the newest valid copy wins, so a power cut during an update
// leaves the previous directory intact. There is one more image space than
// rows, so a replacement is always written in full to a free space before
// the directory switches to it.

#define STORE_CONFIG_OFFSET  0x102000u   // flash offset of the settings sector
#define STORE_ROWS      10
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
bool store_row(int row, image_info_t *out);     // false if the row is empty
int  store_mounted(void);                       // mounted row, -1 = drive empty
bool store_image(image_info_t *out);            // the mounted image
const char *store_format_name(image_format_t f);    // "dsk", "woz1", "woz2"

// Everything below writes flash with interrupts off, one 4 KB sector at a
// time: call from the main loop, never from lwIP callbacks. Each returns 0,
// or -1 with a message in err; on failure nothing visible has changed.

// Streaming write of a new image into a free image space (never one in use,
// nor any in avoid_mask). finish() validates the contents (a 143,360-byte
// .dsk, or a WOZ 1/2 that parses) and puts it in the row, replacing what
// was there; a mounted row stays mounted with the new image.
int  store_write_begin(uint32_t size, uint32_t avoid_mask, char *err, size_t errn);
int  store_write_chunk(const uint8_t *data, size_t n, char *err, size_t errn);
int  store_write_finish(int row, const char *name, int64_t uploaded, char *err, size_t errn);
// True if every byte of the free image space the next store_write_begin
// would pick is erased; else erase one more 4 KB sector of it (~50 ms) and
// return false. Lets idle time prepare a space, so a later write-back only
// programs (store_write_chunk skips erasing sectors already erased).
bool store_prepare_free(uint32_t avoid_mask);
void store_write_abort(void);

int  store_mount(int row, char *err, size_t errn);   // -1 = eject; an empty row is refused
int  store_delete(int row, char *err, size_t errn);  // deleting the mounted row ejects
int  store_rename(int row, const char *name, char *err, size_t errn);   // name already sanitized

// Erase one 4 KB sector at flash offset off and write len bytes there, then
// read back. Main loop only. The settings sector only (anything else is refused).
int  store_write_raw_sector(uint32_t off, const void *data, size_t len, char *err, size_t errn);

void sha256_bytes(const uint8_t *data, size_t n, uint8_t out[32]);
void sha256_hex(const uint8_t h[32], char out[65]);
bool sha256_selftest(void);
