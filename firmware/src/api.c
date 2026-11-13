// HTTP routes and the upload flow.
//
// Upload (PUT /image): the body streams through a RAM ring. lwIP callbacks
// put bytes in (upload_sink); api_poll() in the main loop takes them out and
// writes them to a free flash image space, acknowledging them to TCP only
// then, so flash speed throttles the sender. The ring is larger than the TCP
// window, so it can never overflow. When the last byte is in flash the image
// is validated (.dsk size, or a WOZ that parses) and only then does the
// directory switch to it; the reply goes out after that. A failed or
// abandoned upload leaves the mounted image untouched.
//
// Downloads are served straight from flash. Each records which image space
// it reads, and a new upload never writes into a space being read.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/opt.h"

#include "api.h"
#include "clock.h"
#include "http.h"
#include "json.h"
#include "status.h"
#include "store.h"
#include "woz.h"

extern const unsigned char index_html[];
extern const unsigned int  index_html_len;

#define RING_SIZE  16384
_Static_assert(RING_SIZE >= TCP_WND, "upload ring must hold a full TCP window");

typedef enum { UP_IDLE, UP_RECEIVING, UP_RECEIVED, UP_ABORTED } up_state_t;

// All of this is touched only with the lwIP lock held (callbacks hold it;
// api_poll takes it), except the flash writes, which run without it.
static up_state_t up_state;
static conn_t    *up_conn;              // connection owed the upload reply
static char       up_name[IMAGE_NAME_MAX];
static uint32_t   up_size;
static absolute_time_t up_started;
static uint8_t    ring[RING_SIZE];
static uint32_t   ring_head, ring_tail; // free-running byte counters
static uint8_t    readers[STORE_SLOTS_MAX];    // downloads per image space

// Printable ASCII only; no quote, backslash or slash (it goes into a
// Content-Disposition header and JSON).
static void sanitize_name(char *dst, const char *src)
{
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)src; *s && o < IMAGE_NAME_MAX - 1; s++) {
        unsigned char ch = *s;
        dst[o++] = (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\' || ch == '/') ? '_' : (char)ch;
    }
    dst[o] = 0;
    if (o == 0)
        strcpy(dst, "image.dsk");
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    if (k > n)
        return false;
    for (size_t i = 0; i < k; i++) {
        char a = s[n - k + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (a != b)
            return false;
    }
    return true;
}

static uint32_t reader_mask(void)
{
    uint32_t m = 0;
    for (int i = 0; i < STORE_SLOTS_MAX; i++)
        if (readers[i])
            m |= 1u << i;
    return m;
}

// ---------------------------------------------------------------- GET /image

static void download_closed(conn_t *c)
{
    unsigned slot = (unsigned)(uintptr_t)http_user(c) - 1;
    if (slot < STORE_SLOTS_MAX && readers[slot])
        readers[slot]--;
}

static void get_image(conn_t *c)
{
    image_info_t im;
    if (!store_image(&im)) {
        http_respond_text(c, 404, "no image stored\n");
        return;
    }
    char name[IMAGE_NAME_MAX], hex[65], extra[256];
    sanitize_name(name, im.name);
    sha256_hex(im.sha256, hex);
    snprintf(extra, sizeof extra,
             "Content-Disposition: attachment; filename=\"%s\"\r\n"
             "X-Image-SHA256: %s\r\n", name, hex);
    readers[im.slot]++;
    http_set_user(c, (void *)(uintptr_t)(im.slot + 1));
    http_set_on_close(c, download_closed);
    http_respond(c, 200, "application/octet-stream", im.data, im.size, false, extra);
}

// ---------------------------------------------------- GET /image.woz (test)
// Test endpoint, not on the page: a stored .dsk encoded to GCR and wrapped as
// WOZ 2, so an emulator can check the encoder. Generated while it is sent;
// the WOZ CRC needs every track first, so it is computed once per image.

typedef struct {
    const uint8_t *dsk;
    unsigned slot;
    int      track;                     // track held in trk[], -1 = none
    uint8_t  hdr[WOZ_HEADER_BYTES];
    uint8_t  trk[WOZ_TRACK_BYTES];
} woz_stream_t;

static struct {
    bool     valid;
    uint8_t  sha256[32];
    uint32_t crc;
    uint32_t bits[GCR_TRACKS];
} woz_cache;

static void woz_fill(void *ctx, size_t off, uint8_t *dst, size_t n)
{
    woz_stream_t *w = ctx;
    while (n) {
        size_t chunk;
        if (off < WOZ_HEADER_BYTES) {
            chunk = WOZ_HEADER_BYTES - off;
            if (chunk > n)
                chunk = n;
            memcpy(dst, w->hdr + off, chunk);
        } else {
            size_t t = (off - WOZ_HEADER_BYTES) / WOZ_TRACK_BYTES;
            size_t o = (off - WOZ_HEADER_BYTES) % WOZ_TRACK_BYTES;
            if (w->track != (int)t) {
                gcr_encode_track(w->trk, w->dsk + t * GCR_TRACK_DATA,
                                 (unsigned)t, GCR_DEFAULT_VOLUME);
                w->track = (int)t;
            }
            chunk = WOZ_TRACK_BYTES - o;
            if (chunk > n)
                chunk = n;
            memcpy(dst, w->trk + o, chunk);
        }
        dst += chunk;
        off += chunk;
        n -= chunk;
    }
}

static void woz_closed(conn_t *c)
{
    woz_stream_t *w = http_user(c);
    if (readers[w->slot])
        readers[w->slot]--;
    free(w);
}

static void get_image_woz(conn_t *c)
{
    image_info_t im;
    if (!store_image(&im)) {
        http_respond_text(c, 404, "no image stored\n");
        return;
    }
    if (im.format != FMT_DSK) {
        http_respond_text(c, 409, "the stored image is already WOZ; use GET /image\n");
        return;
    }
    woz_stream_t *w = malloc(sizeof *w);
    if (!w) {
        http_respond_text(c, 500, "out of memory\n");
        return;
    }
    static const char creator[] = "a2drive " A2DRIVE_VERSION;
    if (!woz_cache.valid || memcmp(woz_cache.sha256, im.sha256, 32) != 0) {
        woz_cache.crc = woz_prepare(im.data, woz_cache.bits, w->trk, creator, 1);
        memcpy(woz_cache.sha256, im.sha256, 32);
        woz_cache.valid = true;
    }
    w->dsk = im.data;
    w->slot = im.slot;
    w->track = -1;
    woz_header(w->hdr, woz_cache.bits, woz_cache.crc, creator, 1);

    char name[IMAGE_NAME_MAX + 4], extra[200];
    sanitize_name(name, im.name);
    char *dot = strrchr(name, '.');
    if (dot && (ends_with_ci(dot, ".dsk") || ends_with_ci(dot, ".do")))
        *dot = 0;
    strcat(name, ".woz");
    snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename=\"%s\"\r\n", name);

    readers[im.slot]++;
    http_set_user(c, w);
    http_set_on_close(c, woz_closed);
    http_respond_stream(c, 200, "application/octet-stream", WOZ_FILE_BYTES, woz_fill, w, extra);
}

// ---------------------------------------------------------------- PUT /image

static void upload_sink(conn_t *c, const uint8_t *d, size_t n)
{
    (void)c;
    if (up_state != UP_RECEIVING)
        return;
    if (n > RING_SIZE - (ring_head - ring_tail)) {      // cannot happen (see top)
        printf("upload: ring overflow; aborting\n");
        up_state = UP_ABORTED;
        return;
    }
    while (n) {
        uint32_t at = ring_head % RING_SIZE;
        size_t k = RING_SIZE - at;
        if (k > n)
            k = n;
        memcpy(ring + at, d, k);
        ring_head += (uint32_t)k;
        d += k;
        n -= k;
    }
}

static void upload_complete(conn_t *c)
{
    (void)c;
    if (up_state == UP_RECEIVING)
        up_state = UP_RECEIVED;     // reply comes from api_poll() after flash
}

static void upload_closed(conn_t *c)
{
    if (up_conn != c)
        return;
    up_conn = NULL;
    if (up_state == UP_RECEIVING)   // all bytes in hand (RECEIVED) still commit
        up_state = UP_ABORTED;
}

static void put_image(conn_t *c)
{
    if (up_state != UP_IDLE) {
        http_respond_text(c, 409, "another upload is in progress\n");
        return;
    }
    long cl = http_content_length(c);
    if (cl < 0) {
        http_respond_text(c, 411, "Content-Length required\n");
        return;
    }
    char raw[3 * IMAGE_NAME_MAX], msg[128];
    if (!http_query_param(c, "name", raw, sizeof raw))
        raw[0] = 0;
    sanitize_name(up_name, raw);

    if (cl == 0 || cl > (long)IMAGE_MAX_SIZE) {
        snprintf(msg, sizeof msg, "image must be 1 to %u bytes; got %ld\n", IMAGE_MAX_SIZE, cl);
        http_respond(c, 413, "text/plain; charset=utf-8", msg, strlen(msg), true, NULL);
        return;
    }
    if ((ends_with_ci(up_name, ".dsk") || ends_with_ci(up_name, ".do")) && cl != (long)DSK_SIZE) {
        snprintf(msg, sizeof msg, "a .dsk image must be exactly %u bytes; got %ld\n", DSK_SIZE, cl);
        http_respond(c, 400, "text/plain; charset=utf-8", msg, strlen(msg), true, NULL);
        return;
    }
    char err[96];
    if (store_write_begin((uint32_t)cl, reader_mask(), err, sizeof err)) {
        snprintf(msg, sizeof msg, "%s\n", err);
        http_respond(c, 507, "text/plain; charset=utf-8", msg, strlen(msg), true, NULL);
        return;
    }
    ring_head = ring_tail = 0;
    up_size = (uint32_t)cl;
    up_state = UP_RECEIVING;
    up_conn = c;
    up_started = get_absolute_time();
    http_set_on_close(c, upload_closed);
    http_stream_body(c, (size_t)cl, upload_sink, upload_complete);
}

// ---------------------------------------------------------------- routing

static void handle(conn_t *c)
{
    const char *m = http_method(c), *p = http_path(c);
    bool get = strcmp(m, "GET") == 0;
    bool put = strcmp(m, "PUT") == 0;

    if (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0) {
        if (!get)
            http_respond(c, 405, "text/plain", "GET only\n", 9, false, "Allow: GET\r\n");
        else
            http_respond(c, 200, "text/html; charset=utf-8", index_html, index_html_len, false, NULL);
    } else if (strcmp(p, "/status") == 0) {
        if (!get) {
            http_respond(c, 405, "text/plain", "GET only\n", 9, false, "Allow: GET\r\n");
        } else {
            static char body[STATUS_JSON_MAX];     // lwIP context is serialized
            size_t n = status_json(body, sizeof body);
            http_respond(c, 200, "application/json", body, n, true, NULL);
        }
    } else if (strcmp(p, "/image") == 0) {
        if (get)
            get_image(c);
        else if (put)
            put_image(c);
        else
            http_respond(c, 405, "text/plain", "GET or PUT\n", 11, false, "Allow: GET, PUT\r\n");
    } else if (strcmp(p, "/image.woz") == 0) {
        if (get)
            get_image_woz(c);
        else
            http_respond(c, 405, "text/plain", "GET only\n", 9, false, "Allow: GET\r\n");
    } else {
        http_respond_text(c, 404, "not found\n");
    }
}

void api_init(void)
{
    http_init(handle);
}

static void finish_upload(void)
{
    int64_t ms = absolute_time_diff_us(up_started, get_absolute_time()) / 1000;
    char err[128];
    int rc = store_write_finish(up_name, clock_unix(), err, sizeof err);

    static char e[sizeof err * 6];                      // static: large for the
    static char body[sizeof e + IMAGE_NAME_MAX * 6 + 200];  // shared IRQ stack
    int code;
    if (rc == 0) {
        image_info_t im;
        store_image(&im);
        char name[IMAGE_NAME_MAX * 6], hex[65];
        json_escape(name, sizeof name, im.name);
        sha256_hex(im.sha256, hex);
        snprintf(body, sizeof body,
                 "{\"ok\":true,\"name\":\"%s\",\"format\":\"%s\",\"size\":%lu,\"sha256\":\"%s\"}\n",
                 name, store_format_name(im.format), (unsigned long)im.size, hex);
        code = 200;
        printf("upload: %lu bytes stored in %lld ms\n", (unsigned long)up_size, (long long)ms);
    } else {
        json_escape(e, sizeof e, err);
        snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}\n", e);
        code = 422;
        printf("upload: REJECTED: %s\n", err);
    }

    cyw43_arch_lwip_begin();
    if (up_conn) {
        conn_t *c = up_conn;
        up_conn = NULL;
        http_respond_deferred(c, code, "application/json", body, strlen(body));
    }
    up_state = UP_IDLE;
    cyw43_arch_lwip_end();
}

bool api_poll(void)
{
    static uint8_t chunk[4096];

    if (up_state == UP_IDLE)
        return false;

    if (up_state == UP_ABORTED) {
        store_write_abort();
        cyw43_arch_lwip_begin();
        ring_head = ring_tail = 0;
        up_state = UP_IDLE;
        cyw43_arch_lwip_end();
        printf("upload: connection lost before the image was complete; nothing changed\n");
        return false;
    }

    // Move up to 4 KB from the ring to flash, then let TCP send more.
    cyw43_arch_lwip_begin();
    uint32_t n = ring_head - ring_tail;
    if (n > sizeof chunk)
        n = sizeof chunk;
    for (uint32_t i = 0; i < n; i++)
        chunk[i] = ring[(ring_tail + i) % RING_SIZE];
    ring_tail += n;
    if (n && up_conn)
        http_body_consumed(up_conn, n);
    bool all_in = up_state == UP_RECEIVED && ring_head == ring_tail;
    cyw43_arch_lwip_end();

    if (n) {
        char err[96];
        if (store_write_chunk(chunk, n, err, sizeof err)) {
            printf("upload: %s\n", err);
            cyw43_arch_lwip_begin();
            if (up_conn) {
                conn_t *c = up_conn;
                up_conn = NULL;
                http_body_consumed(c, ring_head - ring_tail);   // release held bytes
                static char e[sizeof err * 6], body[sizeof e + 40];
                json_escape(e, sizeof e, err);
                snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}\n", e);
                http_respond_deferred(c, 500, "application/json", body, strlen(body));
            }
            up_state = UP_ABORTED;
            cyw43_arch_lwip_end();
            return true;
        }
    }
    if (all_in)
        finish_upload();
    return true;
}
