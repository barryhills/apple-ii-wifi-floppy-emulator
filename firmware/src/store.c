// Flash storage for disk images. See store.h for the layout.

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/sha256.h"
#include "pico/cyw43_arch.h"
#include "hardware/flash.h"

#include "crc32.h"
#include "store.h"
#include "woz.h"

#define STORE_OFFSET    (1u << 20)
#define DIR_OFFSET(i)   (STORE_OFFSET + (uint32_t)(i) * FLASH_SECTOR_SIZE)
#define SLOT_BASE       (STORE_OFFSET + 0x10000u)
#define SLOT_SIZE       IMAGE_MAX_SIZE
#define SLOT_COUNT      ((PICO_FLASH_SIZE_BYTES - SLOT_BASE) / SLOT_SIZE)
#define DIR_MAX         8
#define DIR_MAGIC       0x56443241u     // "A2DV"
#define DIR_VERSION     1

typedef struct {
    uint8_t  used;
    uint8_t  slot;
    uint16_t format;
    uint32_t size;
    int64_t  uploaded;
    uint8_t  sha256[32];
    char     name[IMAGE_NAME_MAX];
} dir_entry_t;

typedef struct {
    uint32_t    magic;
    uint16_t    version;
    uint16_t    entry_size;
    uint32_t    seq;
    int32_t     current;                // entry index, -1 = none
    dir_entry_t e[DIR_MAX];
    uint32_t    crc;                    // CRC32 of all bytes before it
} dir_t;

static_assert(sizeof(dir_t) <= FLASH_SECTOR_SIZE, "directory must fit a sector");
static_assert(SLOT_COUNT >= 2, "need at least two image spaces");
static_assert(SLOT_COUNT <= STORE_SLOTS_MAX, "avoid masks are 32 bits");

static dir_t cur;               // live directory; swapped under the lwIP lock
static int   cur_sector = -1;   // 0/1 = which copy holds cur, -1 = none valid
static bool  cur_verified;
static uint8_t secbuf[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

// In-progress streaming write.
static struct {
    bool     active;
    int      slot;
    uint32_t off;           // flash offset of the image space
    uint32_t size;          // announced total
    uint32_t written;       // bytes accepted so far
    uint32_t flushed;       // bytes already in flash (multiple of 4 KB)
    uint32_t staged;        // bytes waiting in wbuf
} wr;
static uint8_t wbuf[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

// ---------------------------------------------------------------- helpers

static uint32_t crc32(const void *data, size_t n)
{
    return crc32_update(0, data, n);
}

void sha256_bytes(const uint8_t *data, size_t n, uint8_t out[32])
{
    pico_sha256_state_t st;
    if (pico_sha256_start_blocking(&st, SHA256_BIG_ENDIAN, false) != PICO_OK) {
        memset(out, 0, 32);
        return;
    }
    pico_sha256_update_blocking(&st, data, n);
    sha256_result_t r;
    pico_sha256_finish(&st, &r);
    memcpy(out, r.bytes, 32);
}

void sha256_hex(const uint8_t h[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = hx[h[i] >> 4];
        out[2 * i + 1] = hx[h[i] & 15];
    }
    out[64] = 0;
}

// FIPS 180-2 test vector: SHA-256("abc").
bool sha256_selftest(void)
{
    static const uint8_t want[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
        0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    uint8_t got[32];
    sha256_bytes((const uint8_t *)"abc", 3, got);
    return memcmp(got, want, 32) == 0;
}

typedef struct { uint32_t off; const uint8_t *src; size_t len; } fop_t;

static void fop_erase(void *p)
{
    const fop_t *f = p;
    flash_range_erase(f->off, f->len);
}

static void fop_program(void *p)
{
    const fop_t *f = p;
    flash_range_program(f->off, f->src, f->len);
}

// One sector per flash_safe_execute call, so interrupts (WiFi, USB) are
// serviced between sectors.
static bool flash_erase(uint32_t off, size_t len)
{
    for (size_t i = 0; i < len; i += FLASH_SECTOR_SIZE) {
        fop_t f = { off + (uint32_t)i, NULL, FLASH_SECTOR_SIZE };
        if (flash_safe_execute(fop_erase, &f, 100) != PICO_OK)
            return false;
    }
    return true;
}

static bool flash_program(uint32_t off, const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; i += FLASH_SECTOR_SIZE) {
        size_t n = len - i < FLASH_SECTOR_SIZE ? len - i : FLASH_SECTOR_SIZE;
        fop_t f = { off + (uint32_t)i, src + i, n };
        if (flash_safe_execute(fop_program, &f, 100) != PICO_OK)
            return false;
    }
    return true;
}

static const dir_t *dir_in_flash(int i)
{
    return (const dir_t *)(XIP_BASE + DIR_OFFSET(i));
}

static const uint8_t *slot_data(unsigned slot)
{
    return (const uint8_t *)(XIP_BASE + SLOT_BASE + slot * SLOT_SIZE);
}

static bool dir_valid(const dir_t *d)
{
    return d->magic == DIR_MAGIC && d->version == DIR_VERSION &&
           d->entry_size == sizeof(dir_entry_t) &&
           d->crc == crc32(d, offsetof(dir_t, crc));
}

static bool entry_sane(const dir_entry_t *e)
{
    return e->used && e->slot < SLOT_COUNT && e->size > 0 &&
           e->size <= SLOT_SIZE && e->format >= FMT_DSK && e->format <= FMT_WOZ2 &&
           memchr(e->name, 0, sizeof e->name);
}

const char *store_format_name(image_format_t f)
{
    switch (f) {
    case FMT_DSK:  return "dsk";
    case FMT_WOZ1: return "woz1";
    case FMT_WOZ2: return "woz2";
    default:       return "none";
    }
}

static bool slot_in_use(const dir_t *d, unsigned slot)
{
    for (int i = 0; i < DIR_MAX; i++)
        if (d->e[i].used && d->e[i].slot == slot)
            return true;
    return false;
}

// ---------------------------------------------------------------- public

void store_init(void)
{
    extern char __flash_binary_end;
    uint32_t fw_end = (uint32_t)(uintptr_t)&__flash_binary_end - XIP_BASE;
    if (fw_end > STORE_OFFSET)
        panic("firmware (%lu bytes) overlaps image storage at 0x%lx",
              (unsigned long)fw_end, (unsigned long)STORE_OFFSET);

    const dir_t *a = dir_in_flash(0), *b = dir_in_flash(1);
    bool va = dir_valid(a), vb = dir_valid(b);
    if (va && (!vb || (int32_t)(a->seq - b->seq) > 0)) {
        cur = *a;
        cur_sector = 0;
    } else if (vb) {
        cur = *b;
        cur_sector = 1;
    } else {
        memset(&cur, 0, sizeof cur);
        cur.current = -1;
        cur_sector = -1;
    }

    if (cur.current >= DIR_MAX || (cur.current >= 0 && !entry_sane(&cur.e[cur.current]))) {
        printf("store: directory entry %ld is bad; ignoring it\n", (long)cur.current);
        cur.current = -1;
    }

    printf("store: firmware %lu KB, %u image spaces, directory %s (seq %lu)\n",
           (unsigned long)(fw_end / 1024), (unsigned)SLOT_COUNT,
           cur_sector < 0 ? "empty" : cur_sector ? "B" : "A",
           (unsigned long)cur.seq);

    cur_verified = false;
    if (cur.current >= 0) {
        const dir_entry_t *e = &cur.e[cur.current];
        uint8_t h[32];
        sha256_bytes(slot_data(e->slot), e->size, h);
        cur_verified = memcmp(h, e->sha256, 32) == 0;
        printf("store: image \"%s\", %lu bytes, flash check %s\n", e->name,
               (unsigned long)e->size, cur_verified ? "OK" : "MISMATCH");
    } else {
        printf("store: no image stored\n");
    }
}

bool store_image(image_info_t *out)
{
    if (cur.current < 0)
        return false;
    const dir_entry_t *e = &cur.e[cur.current];
    memcpy(out->name, e->name, IMAGE_NAME_MAX);
    out->size = e->size;
    out->format = (image_format_t)e->format;
    out->slot = e->slot;
    memcpy(out->sha256, e->sha256, 32);
    out->uploaded = e->uploaded;
    out->verified = cur_verified;
    out->data = slot_data(e->slot);
    return true;
}

int store_write_begin(uint32_t size, uint32_t avoid_mask, char *err, size_t errn)
{
    if (size == 0 || size > SLOT_SIZE) {
        snprintf(err, errn, "image must be 1 to %u bytes", (unsigned)SLOT_SIZE);
        return -1;
    }
    // Pick a free image space, rotating to spread flash wear.
    int cur_slot = cur.current >= 0 ? cur.e[cur.current].slot : -1;
    int slot = -1;
    for (int k = 1; k <= (int)SLOT_COUNT; k++) {
        int s = (cur_slot + k + (int)SLOT_COUNT) % (int)SLOT_COUNT;
        if (!slot_in_use(&cur, (unsigned)s) && !(avoid_mask & (1u << s))) {
            slot = s;
            break;
        }
    }
    if (slot < 0) {
        snprintf(err, errn, "no free image space");
        return -1;
    }
    memset(&wr, 0, sizeof wr);
    wr.active = true;
    wr.slot = slot;
    wr.off = SLOT_BASE + (uint32_t)slot * SLOT_SIZE;
    wr.size = size;
    return 0;
}

// Erase the next sector and program the staged bytes (padded with FF to a
// whole page), then read them back.
static int flush_sector(char *err, size_t errn)
{
    uint32_t at = wr.off + wr.flushed;
    size_t plen = (wr.staged + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    memset(wbuf + wr.staged, 0xFF, FLASH_SECTOR_SIZE - wr.staged);
    if (!flash_erase(at, FLASH_SECTOR_SIZE) || !flash_program(at, wbuf, plen)) {
        snprintf(err, errn, "flash write failed at 0x%lx", (unsigned long)at);
        return -1;
    }
    if (memcmp((const void *)(XIP_BASE + at), wbuf, wr.staged) != 0) {
        snprintf(err, errn, "flash read-back mismatch at 0x%lx", (unsigned long)at);
        return -1;
    }
    wr.flushed += FLASH_SECTOR_SIZE;
    wr.staged = 0;
    return 0;
}

int store_write_chunk(const uint8_t *data, size_t n, char *err, size_t errn)
{
    if (!wr.active || n > wr.size - wr.written) {
        snprintf(err, errn, "write past the announced size");
        return -1;
    }
    while (n) {
        size_t take = FLASH_SECTOR_SIZE - wr.staged;
        if (take > n)
            take = n;
        memcpy(wbuf + wr.staged, data, take);
        wr.staged += take;
        wr.written += take;
        data += take;
        n -= take;
        if (wr.staged == FLASH_SECTOR_SIZE && flush_sector(err, errn))
            return -1;
    }
    return 0;
}

void store_write_abort(void)
{
    wr.active = false;      // the space holds junk but stays free in the directory
}

int store_write_finish(const char *name, int64_t uploaded, char *err, size_t errn)
{
    if (!wr.active) {
        snprintf(err, errn, "no write in progress");
        return -1;
    }
    wr.active = false;
    if (wr.written != wr.size) {
        snprintf(err, errn, "short image: %lu of %lu bytes",
                 (unsigned long)wr.written, (unsigned long)wr.size);
        return -1;
    }
    if (wr.staged && flush_sector(err, errn))
        return -1;

    // What did we get? WOZ is recognised by its signature; anything else
    // must be exactly the size of a 16-sector .dsk.
    const uint8_t *fl = slot_data((unsigned)wr.slot);
    image_format_t fmt;
    if (wr.size >= 4 && (memcmp(fl, "WOZ1", 4) == 0 || memcmp(fl, "WOZ2", 4) == 0)) {
        woz_info_t wi;
        if (woz_parse(fl, wr.size, &wi, err, errn))
            return -1;
        fmt = wi.version == 1 ? FMT_WOZ1 : FMT_WOZ2;
    } else if (wr.size == DSK_SIZE) {
        fmt = FMT_DSK;
    } else {
        snprintf(err, errn, "not a .dsk (exactly %u bytes) or a WOZ file", DSK_SIZE);
        return -1;
    }

    dir_t nd;
    memset(&nd, 0, sizeof nd);
    nd.magic = DIR_MAGIC;
    nd.version = DIR_VERSION;
    nd.entry_size = sizeof(dir_entry_t);
    nd.seq = cur.seq + 1;
    nd.current = 0;
    dir_entry_t *e = &nd.e[0];
    e->used = 1;
    e->slot = (uint8_t)wr.slot;
    e->format = (uint16_t)fmt;
    e->size = wr.size;
    e->uploaded = uploaded;
    sha256_bytes(fl, wr.size, e->sha256);           // hash what is actually stored
    strncpy(e->name, name, IMAGE_NAME_MAX - 1);
    nd.crc = crc32(&nd, offsetof(dir_t, crc));

    int ns = cur_sector == 0 ? 1 : 0;               // never overwrite the live copy
    memset(secbuf, 0xFF, sizeof secbuf);
    memcpy(secbuf, &nd, sizeof nd);
    size_t plen = (sizeof nd + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    if (!flash_erase(DIR_OFFSET(ns), FLASH_SECTOR_SIZE) ||
        !flash_program(DIR_OFFSET(ns), secbuf, plen)) {
        snprintf(err, errn, "directory write failed");
        return -1;
    }
    if (!dir_valid(dir_in_flash(ns)) || memcmp(dir_in_flash(ns), &nd, sizeof nd) != 0) {
        snprintf(err, errn, "directory read-back mismatch");
        return -1;
    }

    cyw43_arch_lwip_begin();
    cur = nd;
    cur_sector = ns;
    cur_verified = true;
    cyw43_arch_lwip_end();

    char hex[65];
    sha256_hex(e->sha256, hex);
    printf("store: wrote \"%s\" (%s, %lu bytes) to image space %d, directory %c seq %lu, sha256 %s\n",
           e->name, store_format_name(fmt), (unsigned long)e->size, wr.slot,
           ns ? 'B' : 'A', (unsigned long)nd.seq, hex);
    return 0;
}
