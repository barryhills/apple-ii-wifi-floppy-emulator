// HTTP routes and the upload/commit flow.
//
// Upload: PUT /image streams the body into upload_buf (lwIP context). When it
// is complete the connection is held and api_poll() (main loop) writes it to
// flash, then answers. The mounted image is untouched until store_commit()
// switches the directory, so a failed or abandoned upload changes nothing.
//
// Downloads are served straight from flash. A commit waits until no download
// is in progress, because a new image may reuse the space an old one is
// still being read from.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "api.h"
#include "clock.h"
#include "http.h"
#include "json.h"
#include "status.h"
#include "store.h"
#include "woz.h"

extern const unsigned char index_html[];
extern const unsigned int  index_html_len;

typedef enum { UP_IDLE, UP_RECEIVING, UP_READY, UP_COMMITTING } up_state_t;

static volatile up_state_t up_state;
static conn_t *up_conn;                 // connection owed the upload reply
static volatile int downloads_active;
static char up_name[IMAGE_NAME_MAX];
static absolute_time_t up_started;
static uint8_t upload_buf[DSK_SIZE] __attribute__((aligned(4)));

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

static void download_closed(conn_t *c)
{
    (void)c;
    downloads_active--;
}

static void upload_closed(conn_t *c)
{
    if (up_conn != c)
        return;
    up_conn = NULL;
    if (up_state == UP_RECEIVING) {
        up_state = UP_IDLE;
        printf("upload: connection lost before the image was complete; nothing changed\n");
    }
}

static void upload_complete(conn_t *c)
{
    (void)c;
    up_state = UP_READY;    // reply comes from api_poll() after the flash write
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
    downloads_active++;
    http_set_on_close(c, download_closed);
    http_respond(c, 200, "application/octet-stream", im.data, im.size, false, extra);
}

// ---- GET /image.woz: the stored .dsk encoded to GCR, as a WOZ 2 file.
// The file is generated while it is sent, one track at a time. The WOZ CRC
// needs every track first, so it is computed once per image and cached.

typedef struct {
    const uint8_t *dsk;
    int     track;                      // track held in trk[], -1 = none
    uint8_t hdr[WOZ_HEADER_BYTES];
    uint8_t trk[WOZ_TRACK_BYTES];
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
    free(http_user(c));
    downloads_active--;
}

static void get_image_woz(conn_t *c)
{
    image_info_t im;
    if (!store_image(&im)) {
        http_respond_text(c, 404, "no image stored\n");
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
    w->track = -1;
    woz_header(w->hdr, woz_cache.bits, woz_cache.crc, creator, 1);

    // name.dsk / name.do -> name.woz
    char name[IMAGE_NAME_MAX + 4], extra[200];
    sanitize_name(name, im.name);
    char *dot = strrchr(name, '.');
    if (dot && (strcmp(dot, ".dsk") == 0 || strcmp(dot, ".DSK") == 0 ||
                strcmp(dot, ".do") == 0 || strcmp(dot, ".DO") == 0))
        *dot = 0;
    strcat(name, ".woz");
    snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename=\"%s\"\r\n", name);

    downloads_active++;
    http_set_user(c, w);
    http_set_on_close(c, woz_closed);
    http_respond_stream(c, 200, "application/octet-stream", WOZ_FILE_BYTES,
                        woz_fill, w, extra);
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
    if (cl != (long)DSK_SIZE) {
        char msg[96];
        snprintf(msg, sizeof msg, "a .dsk image must be exactly %u bytes; got %ld\n",
                 DSK_SIZE, cl);
        http_respond(c, 400, "text/plain; charset=utf-8", msg, strlen(msg), true, NULL);
        return;
    }
    char raw[3 * IMAGE_NAME_MAX];
    if (!http_query_param(c, "name", raw, sizeof raw))
        raw[0] = 0;
    sanitize_name(up_name, raw);

    up_state = UP_RECEIVING;
    up_conn = c;
    up_started = get_absolute_time();
    http_set_on_close(c, upload_closed);
    http_receive_body(c, upload_buf, DSK_SIZE, upload_complete);
}

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

void api_poll(void)
{
    if (up_state != UP_READY)
        return;

    cyw43_arch_lwip_begin();
    bool go = up_state == UP_READY && downloads_active == 0;
    if (go)
        up_state = UP_COMMITTING;
    cyw43_arch_lwip_end();
    if (!go)
        return;

    int64_t rx_ms = absolute_time_diff_us(up_started, get_absolute_time()) / 1000;
    absolute_time_t t0 = get_absolute_time();
    char err[96];
    int rc = store_commit(upload_buf, DSK_SIZE, up_name, clock_unix(), err, sizeof err);
    int64_t wr_ms = absolute_time_diff_us(t0, get_absolute_time()) / 1000;

    char body[IMAGE_NAME_MAX * 6 + 160];
    int code;
    if (rc == 0) {
        image_info_t im;
        store_image(&im);
        char name[IMAGE_NAME_MAX * 6], hex[65];
        json_escape(name, sizeof name, im.name);
        sha256_hex(im.sha256, hex);
        snprintf(body, sizeof body,
                 "{\"ok\":true,\"name\":\"%s\",\"size\":%lu,\"sha256\":\"%s\"}\n",
                 name, (unsigned long)im.size, hex);
        code = 200;
        printf("upload: received in %lld ms, flash write+verify %lld ms\n",
               (long long)rx_ms, (long long)wr_ms);
    } else {
        snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}\n", err);
        code = 500;
        printf("upload: FAILED: %s\n", err);
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
