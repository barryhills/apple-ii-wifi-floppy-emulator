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
// The directory rotates through DIR_RING sectors (since v0.13.2; before,
// only A and B), spreading erase wear 7x. Ring index -> sector: 0, 1 are
// the original A and B (so older directories are found after an upgrade);
// 2, 3 (0x102000-0x103FFF) are the settings sector and its reserved spare
// and are skipped; ring 2..13 are sectors 4..15 (0x104000-0x10FFFF).
#define DIR_RING        14
#define DIR_OFFSET(i)   (STORE_OFFSET + (uint32_t)((i) < 2 ? (i) : (i) + 2) * FLASH_SECTOR_SIZE)
#define SLOT_BASE       (STORE_OFFSET + 0x10000u)
#define SLOT_SIZE       IMAGE_MAX_SIZE
#define SLOT_COUNT      ((PICO_FLASH_SIZE_BYTES - SLOT_BASE) / SLOT_SIZE)
#define DIR_MAGIC       0x56443241u     // "A2DV"
#define DIR_VERSION     2               // v1: 8 entries, one image (v0.2-0.4)
#define DIR_V1_MAX      8

// Same 112-byte layout in v1 and v2 (v1's uint16 format == v2's format +
// flags=0 on little-endian), so v1 entries copy across unchanged.
// flags: unused since v0.14 (was the write-protect bit; every image is
// now writable). Kept so the on-flash layout is unchanged; written as 0.
typedef struct {
    uint8_t  used;
    uint8_t  slot;
    uint8_t  format;
    uint8_t  flags;
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
    int32_t     current;                // mounted row, -1 = none
    dir_entry_t e[STORE_ROWS];
    uint32_t    crc;                    // CRC32 of all bytes before it
} dir_t;

typedef struct {                        // v1, read only for migration
    uint32_t    magic;
    uint16_t    version;
    uint16_t    entry_size;
    uint32_t    seq;
    int32_t     current;
    dir_entry_t e[DIR_V1_MAX];
    uint32_t    crc;
} dir_v1_t;

static_assert(sizeof(dir_t) <= FLASH_SECTOR_SIZE, "directory must fit a sector");
static_assert(DIR_OFFSET(1) < STORE_CONFIG_OFFSET && DIR_OFFSET(2) > STORE_CONFIG_OFFSET + FLASH_SECTOR_SIZE,
              "directory ring must skip the settings sector and its spare");
static_assert(DIR_OFFSET(DIR_RING - 1) + FLASH_SECTOR_SIZE <= SLOT_BASE,
              "directory ring must end before the image spaces");
static_assert(SLOT_COUNT >= STORE_ROWS + 1, "need a spare image space");
static_assert(SLOT_COUNT <= STORE_SLOTS_MAX, "avoid masks are 32 bits");

static dir_t cur;               // live directory; swapped under the lwIP lock
static dir_t nd;                // the next directory being built (one at a time, main loop;
                                // static: keep 1.1 KB off the shared stack)
static int   cur_sector = -1;   // ring index holding cur (0..DIR_RING-1), -1 = none valid;
                                // main loop only, like every directory write
static bool  verified[STORE_ROWS];
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

static bool dir_v1_valid(const void *p)
{
    const dir_v1_t *d = p;
    return d->magic == DIR_MAGIC && d->version == 1 &&
           d->entry_size == sizeof(dir_entry_t) &&
           d->crc == crc32(d, offsetof(dir_v1_t, crc));
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
    for (int i = 0; i < STORE_ROWS; i++)
        if (d->e[i].used && d->e[i].slot == slot)
            return true;
    return false;
}

// Write nd as the new directory into the next ring sector (never the live
// one: it holds the oldest copy), read it back, then make it live. A power
// cut at any point leaves the live copy, the newest valid one, intact.
// Main loop only (all callers are); readers in lwIP context see cur swap
// under the lwIP lock, as before.
static int dir_commit(dir_t *nd, char *err, size_t errn)
{
    nd->magic = DIR_MAGIC;
    nd->version = DIR_VERSION;
    nd->entry_size = sizeof(dir_entry_t);
    nd->seq = cur.seq + 1;
    nd->crc = crc32(nd, offsetof(dir_t, crc));

    int ns = cur_sector < 0 ? 0 : (cur_sector + 1) % DIR_RING;
    memset(secbuf, 0xFF, sizeof secbuf);
    memcpy(secbuf, nd, sizeof *nd);
    size_t plen = (sizeof *nd + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    if (!flash_erase(DIR_OFFSET(ns), FLASH_SECTOR_SIZE) ||
        !flash_program(DIR_OFFSET(ns), secbuf, plen)) {
        snprintf(err, errn, "directory write failed");
        return -1;
    }
    if (!dir_valid(dir_in_flash(ns)) || memcmp(dir_in_flash(ns), nd, sizeof *nd) != 0) {
        snprintf(err, errn, "directory read-back mismatch");
        return -1;
    }
    cyw43_arch_lwip_begin();
    cur = *nd;
    cur_sector = ns;
    cyw43_arch_lwip_end();
    return 0;
}

void store_init(void)
{
    extern char __flash_binary_end;
    uint32_t fw_end = (uint32_t)(uintptr_t)&__flash_binary_end - XIP_BASE;
    if (fw_end > STORE_OFFSET)
        panic("firmware (%lu bytes) overlaps image storage at 0x%lx",
              (unsigned long)fw_end, (unsigned long)STORE_OFFSET);

    // Newest valid copy in the ring wins; a v1 directory (single image, only
    // ever in A or B) is converted in RAM and written as v2 on the next change.
    memset(&cur, 0, sizeof cur);
    cur.current = -1;
    cur_sector = -1;
    bool found = false;
    for (int i = 0; i < DIR_RING; i++) {
        const void *p = (const void *)(XIP_BASE + DIR_OFFSET(i));
        const dir_t *d2 = p;
        const dir_v1_t *d1 = p;
        if (dir_valid(d2)) {
            if (!found || (int32_t)(d2->seq - cur.seq) > 0) {
                cur = *d2;
                cur_sector = i;
                found = true;
            }
        } else if (i < 2 && dir_v1_valid(d1)) {
            if (!found || (int32_t)(d1->seq - cur.seq) > 0) {
                memset(&cur, 0, sizeof cur);
                cur.seq = d1->seq;
                cur.current = d1->current;
                memcpy(cur.e, d1->e, sizeof d1->e);
                cur_sector = i;
                found = true;
                printf("store: migrating v1 directory (copy %c)\n", i ? 'B' : 'A');
            }
        }
    }

    int rows = 0;
    for (int r = 0; r < STORE_ROWS; r++) {
        dir_entry_t *e = &cur.e[r];
        verified[r] = false;
        if (!e->used)
            continue;
        if (!entry_sane(e)) {
            printf("store: row %d is bad; ignoring it\n", r + 1);
            memset(e, 0, sizeof *e);
            continue;
        }
        uint8_t h[32];
        sha256_bytes(slot_data(e->slot), e->size, h);
        verified[r] = memcmp(h, e->sha256, 32) == 0;
        rows++;
        printf("store: row %d \"%s\" %s %lu bytes, flash check %s\n", r + 1, e->name,
               store_format_name((image_format_t)e->format), (unsigned long)e->size,
               verified[r] ? "OK" : "MISMATCH");
    }
    if (cur.current >= STORE_ROWS || (cur.current >= 0 && !cur.e[cur.current].used))
        cur.current = -1;

    printf("store: firmware %lu KB, %u image spaces, directory ring %d of %d seq %lu, %d image(s), mounted %d\n",
           (unsigned long)(fw_end / 1024), (unsigned)SLOT_COUNT, cur_sector, DIR_RING,
           (unsigned long)cur.seq, rows, (int)cur.current + 1);
}

bool store_row(int row, image_info_t *out)
{
    if (row < 0 || row >= STORE_ROWS || !cur.e[row].used)
        return false;
    const dir_entry_t *e = &cur.e[row];
    memcpy(out->name, e->name, IMAGE_NAME_MAX);
    out->size = e->size;
    out->format = (image_format_t)e->format;
    memcpy(out->sha256, e->sha256, 32);
    out->uploaded = e->uploaded;
    out->verified = verified[row];
    out->slot = e->slot;
    out->data = slot_data(e->slot);
    return true;
}

int store_mounted(void)
{
    return (int)cur.current;
}

bool store_image(image_info_t *out)
{
    return store_row((int)cur.current, out);
}

// Pick a free image space, rotating from the last one written to spread
// flash wear.
static int last_slot = -1;
static int prep_slot = -1;              // the space store_prepare_free works on
static unsigned prep_next;              // its first sector not known to be erased

static int pick_slot(uint32_t avoid_mask)
{
    for (int k = 1; k <= (int)SLOT_COUNT; k++) {
        int s = (last_slot + k + (int)SLOT_COUNT) % (int)SLOT_COUNT;
        if (!slot_in_use(&cur, (unsigned)s) && !(avoid_mask & (1u << s)))
            return s;
    }
    return -1;
}

static bool sector_erased(uint32_t off)
{
    const uint32_t *p = (const uint32_t *)(XIP_BASE + off);
    for (unsigned i = 0; i < FLASH_SECTOR_SIZE / 4; i++)
        if (p[i] != 0xFFFFFFFFu)
            return false;
    return true;
}

bool store_prepare_free(uint32_t avoid_mask)
{
    int s = pick_slot(avoid_mask);
    if (s < 0)
        return true;                    // nothing to prepare
    if (s != prep_slot) {
        prep_slot = s;
        prep_next = 0;
    }
    uint32_t base = SLOT_BASE + (uint32_t)s * SLOT_SIZE;
    while (prep_next < SLOT_SIZE / FLASH_SECTOR_SIZE) {
        uint32_t off = base + prep_next * FLASH_SECTOR_SIZE;
        prep_next++;
        if (!sector_erased(off)) {
            flash_erase(off, FLASH_SECTOR_SIZE);
            return false;
        }
    }
    return true;
}

int store_write_begin(uint32_t size, uint32_t avoid_mask, char *err, size_t errn)
{
    if (size == 0 || size > SLOT_SIZE) {
        snprintf(err, errn, "image must be 1 to %u bytes", (unsigned)SLOT_SIZE);
        return -1;
    }
    int slot = pick_slot(avoid_mask);
    if (slot < 0) {
        snprintf(err, errn, "no free image space (a download may be in progress; try again)");
        return -1;
    }
    prep_slot = -1;                     // being written: no longer "prepared"
    last_slot = slot;
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
    if ((!sector_erased(at) && !flash_erase(at, FLASH_SECTOR_SIZE)) ||
        !flash_program(at, wbuf, plen)) {
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

int store_write_finish(int row, const char *name, int64_t uploaded, char *err, size_t errn)
{
    if (!wr.active) {
        snprintf(err, errn, "no write in progress");
        return -1;
    }
    wr.active = false;
    if (row < 0 || row >= STORE_ROWS) {
        snprintf(err, errn, "bad row");
        return -1;
    }
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
    } else if (DSK_SIZE_OK(wr.size)) {
        fmt = FMT_DSK;
    } else {
        snprintf(err, errn, "not a .dsk (%u bytes, up to %u more tolerated) or a WOZ file",
                 DSK_SIZE, DSK_MAX_EXTRA);
        return -1;
    }

    nd = cur;
    dir_entry_t *e = &nd.e[row];
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->slot = (uint8_t)wr.slot;
    e->format = (uint8_t)fmt;
    e->size = wr.size;
    e->uploaded = uploaded;
    sha256_bytes(fl, wr.size, e->sha256);           // hash what is actually stored
    strncpy(e->name, name, IMAGE_NAME_MAX - 1);
    if (dir_commit(&nd, err, errn))
        return -1;
    verified[row] = true;

    char hex[65];
    sha256_hex(e->sha256, hex);
    printf("store: row %d <- \"%s\" (%s, %lu bytes) in image space %d, seq %lu, sha256 %s\n",
           row + 1, e->name, store_format_name(fmt), (unsigned long)e->size, wr.slot,
           (unsigned long)cur.seq, hex);
    return 0;
}

int store_mount(int row, char *err, size_t errn)
{
    if (row < -1 || row >= STORE_ROWS) {
        snprintf(err, errn, "bad row");
        return -1;
    }
    if (row >= 0 && !cur.e[row].used) {
        snprintf(err, errn, "that row is empty");
        return -1;
    }
    if (row == cur.current)
        return 0;
    nd = cur;
    nd.current = row;
    if (dir_commit(&nd, err, errn))
        return -1;
    printf("store: mounted %s\n", row < 0 ? "nothing (drive empty)" : cur.e[row].name);
    return 0;
}

int store_delete(int row, char *err, size_t errn)
{
    if (row < 0 || row >= STORE_ROWS) {
        snprintf(err, errn, "bad row");
        return -1;
    }
    if (!cur.e[row].used)
        return 0;
    nd = cur;
    memset(&nd.e[row], 0, sizeof nd.e[row]);
    if (nd.current == row)
        nd.current = -1;
    if (dir_commit(&nd, err, errn))
        return -1;
    verified[row] = false;
    printf("store: row %d deleted\n", row + 1);
    return 0;
}

int store_rename(int row, const char *name, char *err, size_t errn)
{
    if (row < 0 || row >= STORE_ROWS || !cur.e[row].used) {
        snprintf(err, errn, "that row is empty");
        return -1;
    }
    if (!name[0] || strlen(name) >= IMAGE_NAME_MAX) {
        snprintf(err, errn, "name must be 1 to %d characters", IMAGE_NAME_MAX - 1);
        return -1;
    }
    if (strcmp(name, cur.e[row].name) == 0)
        return 0;
    nd = cur;
    memset(nd.e[row].name, 0, IMAGE_NAME_MAX);
    strncpy(nd.e[row].name, name, IMAGE_NAME_MAX - 1);
    if (dir_commit(&nd, err, errn))
        return -1;
    printf("store: row %d renamed to \"%s\"\n", row + 1, name);
    return 0;
}

int store_write_raw_sector(uint32_t off, const void *data, size_t len, char *err, size_t errn)
{
    if (off != STORE_CONFIG_OFFSET || len > FLASH_SECTOR_SIZE) {   // never a directory sector
        snprintf(err, errn, "bad settings sector");
        return -1;
    }
    memset(secbuf, 0xFF, sizeof secbuf);
    memcpy(secbuf, data, len);
    size_t plen = (len + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    if (!flash_erase(off, FLASH_SECTOR_SIZE) || !flash_program(off, secbuf, plen)) {
        snprintf(err, errn, "flash write failed");
        return -1;
    }
    if (memcmp((const void *)(XIP_BASE + off), data, len) != 0) {
        snprintf(err, errn, "flash read-back mismatch");
        return -1;
    }
    return 0;
}
