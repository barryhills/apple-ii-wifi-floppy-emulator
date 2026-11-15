// HTTP routes, uploads, and directory operations.
//
// Rows are 1..10 in URLs, 0..9 in the store.
//   GET    /images        the whole list (JSON)
//   GET    /image/N       download row N (original file)
//   PUT    /image/N       upload into row N (?name=file), replacing it
//   DELETE /image/N       clear row N
//   POST   /mount/N       mount row N (refused if the row is empty)
//   POST   /eject         unmount: the drive becomes empty
//   POST   /protect/N?on=1|0    protect (P) or unprotect row N
//   GET    /settings      hostname, SSID, source (never the password)
//   POST   /settings      form body ssid=&password=&hostname= ; saves, restarts
//   POST   /restart       restart the drive
//   POST   /rename/N?name=X     rename row N (the file type is kept)
//   GET    /image         download the mounted image
//   GET    /image.woz     test only: mounted .dsk GCR-encoded as WOZ 2
//
// Mount and delete write the flash directory, so like uploads they run in
// the main loop (api_poll) and answer when done. One operation at a time;
// others get 409.
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
#include "hardware/watchdog.h"
#include "lwip/opt.h"

#include "api.h"
#include "clock.h"
#include "config.h"
#include "diag.h"
#include "http.h"
#include "json.h"
#include "benchtest.h"
#include "player.h"
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
static int        up_row;

typedef enum { OP_NONE, OP_MOUNT, OP_DELETE, OP_WRITABLE, OP_RENAME, OP_SETTINGS, OP_STREAM } op_t;
static op_t       op;
static int        op_row;
static bool       op_on;
static char       op_name[IMAGE_NAME_MAX];
static char       set_body[512 + 1];            // POST /settings form
static char       set_ssid[CFG_SSID_MAX + 1], set_pw[CFG_PASSWORD_MAX + 1],
                  set_host[CFG_HOSTNAME_MAX + 1];
static absolute_time_t restart_at;              // nil = no restart pending
static bool       restart_pending;
static conn_t    *op_conn;

static bool busy(void);

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

// The image's file extension (".dsk", ".woz", ...) from its stored name, or
// one implied by its format.
static const char *image_ext(const image_info_t *im)
{
    const char *dot = strrchr(im->name, '.');
    if (dot && (ends_with_ci(dot, ".dsk") || ends_with_ci(dot, ".do") || ends_with_ci(dot, ".woz")))
        return dot;
    return im->format == FMT_DSK ? ".dsk" : ".woz";
}

// Rename request: new display name + the image's existing extension. A
// typed extension of its own (".dsk/.do/.woz") is dropped.
static bool rename_target(int row, const char *raw, char *out, char *msg, size_t msgn)
{
    image_info_t im;
    if (!store_row(row, &im)) {
        snprintf(msg, msgn, "that row is empty\n");
        return false;
    }
    char base[IMAGE_NAME_MAX];
    size_t n = 0;
    while (*raw == ' ')
        raw++;
    for (; *raw && n < sizeof base - 1; raw++)
        base[n++] = *raw;
    base[n] = 0;
    while (n && base[n - 1] == ' ')
        base[--n] = 0;
    char *dot = strrchr(base, '.');
    if (dot && (ends_with_ci(dot, ".dsk") || ends_with_ci(dot, ".do") || ends_with_ci(dot, ".woz")))
        *dot = 0;
    const char *ext = image_ext(&im);
    if (!base[0]) {
        snprintf(msg, msgn, "the name is empty\n");
        return false;
    }
    if (strlen(base) + strlen(ext) >= IMAGE_NAME_MAX) {
        snprintf(msg, msgn, "name too long (max %u characters)\n",
                 (unsigned)(IMAGE_NAME_MAX - 1 - strlen(ext)));
        return false;
    }
    char full[2 * IMAGE_NAME_MAX];      // length already checked above
    snprintf(full, sizeof full, "%s%s", base, ext);
    sanitize_name(out, full);
    return true;
}

static uint32_t reader_mask(void)       // lwIP lock held
{
    uint32_t m = 0;
    for (int i = 0; i < STORE_SLOTS_MAX; i++)
        if (readers[i])
            m |= 1u << i;
    return m;
}

uint32_t api_reader_mask(void)          // main loop
{
    cyw43_arch_lwip_begin();
    uint32_t m = reader_mask();
    cyw43_arch_lwip_end();
    return m;
}

// ---------------------------------------------------------------- GET /image

static void download_closed(conn_t *c)
{
    unsigned slot = (unsigned)(uintptr_t)http_user(c) - 1;
    if (slot < STORE_SLOTS_MAX && readers[slot])
        readers[slot]--;
}

// "/image/7" -> 6; -1 if not a row number 1..STORE_ROWS.
static int parse_row(const char *s)
{
    if (!*s || strlen(s) > 2)
        return -1;
    int n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        n = n * 10 + (*s - '0');
    }
    return n >= 1 && n <= STORE_ROWS ? n - 1 : -1;
}

static void get_image(conn_t *c, int row)
{
    image_info_t im;
    if (!store_row(row, &im)) {
        http_respond_text(c, 404, row == store_mounted() || row < 0 ?
                          "no image mounted\n" : "that row is empty\n");
        return;
    }
    static char name[IMAGE_NAME_MAX], hex[65], extra[256];     // static: lwIP context, serialized
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
                                 (unsigned)t, gcr_dsk_volume(w->dsk));
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

    static char name[IMAGE_NAME_MAX + 4], extra[200];          // static: lwIP context, serialized
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

static void put_image(conn_t *c, int row)
{
    if (busy()) {
        http_respond_text(c, 409, "another upload is in progress\n");
        return;
    }
    long cl = http_content_length(c);
    if (cl < 0) {
        http_respond_text(c, 411, "Content-Length required\n");
        return;
    }
    static char raw[3 * IMAGE_NAME_MAX], msg[128];             // static: lwIP context, serialized
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
    up_row = row;
    up_size = (uint32_t)cl;
    up_state = UP_RECEIVING;
    up_conn = c;
    up_started = get_absolute_time();
    http_set_on_close(c, upload_closed);
    http_stream_body(c, (size_t)cl, upload_sink, upload_complete);
}

// ---------------------------------------------------------------- list

static void get_images(conn_t *c)
{
    static char body[STORE_ROWS * 400 + 64];
    size_t o = 0;
    o += (size_t)snprintf(body + o, sizeof body - o, "{\"mounted\":%d,\"busy\":%s,\"rows\":[",
                          store_mounted() + 1,
                          (up_state != UP_IDLE || op != OP_NONE) ? "true" : "false");
    for (int r = 0; r < STORE_ROWS; r++) {
        image_info_t im;
        const char *sep = r ? "," : "";
        if (!store_row(r, &im)) {
            o += (size_t)snprintf(body + o, sizeof body - o, "%snull", sep);
            continue;
        }
        static char name[IMAGE_NAME_MAX * 6], hex[65];         // static: lwIP context, serialized
        char up[24] = "null";
        json_escape(name, sizeof name, im.name);
        sha256_hex(im.sha256, hex);
        if (im.uploaded)
            snprintf(up, sizeof up, "%lld", (long long)im.uploaded);
        o += (size_t)snprintf(body + o, sizeof body - o,
            "%s{\"name\":\"%s\",\"format\":\"%s\",\"size\":%lu,\"sha256\":\"%s\","
            "\"uploaded\":%s,\"verified\":%s,\"protected\":%s}",
            sep, name, store_format_name(im.format), (unsigned long)im.size, hex, up,
            im.verified ? "true" : "false", (im.flags & IMG_FLAG_WRITABLE) ? "false" : "true");
        if (o >= sizeof body)
            break;
    }
    if (o < sizeof body)
        o += (size_t)snprintf(body + o, sizeof body - o, "]}\n");
    if (o >= sizeof body) {
        http_respond_text(c, 500, "list too large\n");
        return;
    }
    http_respond(c, 200, "application/json", body, o, true, NULL);
}

// ---------------------------------------------------------------- mount / delete

static void op_closed(conn_t *c)
{
    if (op_conn == c)
        op_conn = NULL;         // the operation still completes
}

static void start_op(conn_t *c, op_t what, int row)
{
    if (busy()) {
        http_respond_text(c, 409, "busy; try again\n");
        return;
    }
    op = what;
    op_row = row;
    op_conn = c;
    http_set_on_close(c, op_closed);
    http_hold(c);
}

static void run_op(void)
{
    char err[96];
    int rc = op == OP_MOUNT    ? store_mount(op_row, err, sizeof err) :
             op == OP_DELETE   ? store_delete(op_row, err, sizeof err) :
             op == OP_RENAME   ? store_rename(op_row, op_name, err, sizeof err) :
             op == OP_SETTINGS ? config_save(set_ssid, set_pw, set_host, err, sizeof err) :
                                 store_set_writable(op_row, op_on, err, sizeof err);
    if (op == OP_SETTINGS) {
        memset(set_pw, 0, sizeof set_pw);
        memset(set_body, 0, sizeof set_body);
        if (rc == 0) {
            restart_pending = true;     // after the reply has gone out
            restart_at = make_timeout_time_ms(1500);
        }
    }
    static char e[sizeof err * 6], body[sizeof e + 64];
    int code = 200;
    if (rc == 0) {
        snprintf(body, sizeof body, "{\"ok\":true,\"mounted\":%d}\n", store_mounted() + 1);
    } else {
        json_escape(e, sizeof e, err);
        snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}\n", e);
        code = strcmp(err, "that row is empty") == 0 ? 409 : 500;
    }
    cyw43_arch_lwip_begin();
    if (op_conn) {
        conn_t *c = op_conn;
        op_conn = NULL;
        http_respond_deferred(c, code, "application/json", body, strlen(body));
    }
    op = OP_NONE;
    cyw43_arch_lwip_end();
}

// ---------------------------------------------------------------- bench tests

// POST /test/stream?track=N[&cells=N][&start=N] and POST /test/seek?q=N (or
// ?track=N): run from the main loop: the first pass starts the test, later
// passes poll until the report is ready (~0.3 s; a seek blocks for its
// steps first, <= ~0.7 s), then it is the reply. See benchtest.h.
static bool stream_started;
static int test_kind;               // which test OP_STREAM runs: 0 stream, 1 seek, 2 write
static int test_sector;
static uint32_t test_cell_ns, test_seed;
static uint32_t stream_cells;       // 0 = whole track
static uint32_t stream_start;
static bool test_flash;             // seek: write flash during the capture

static void stream_reply(int code, const char *body)
{
    cyw43_arch_lwip_begin();
    if (op_conn) {
        conn_t *c = op_conn;
        op_conn = NULL;
        http_respond_deferred(c, code, "application/json", body, strlen(body));
    }
    op = OP_NONE;
    stream_started = false;
    cyw43_arch_lwip_end();
}

static void poll_stream(void)
{
    static char report[1024];
    if (!stream_started) {
        char err[96];
        int rc = test_kind == 1 ? benchtest_seek(op_row, test_flash, err, sizeof err) :
                 test_kind == 2 ? benchtest_write(op_row, test_sector, test_cell_ns, test_seed,
                                                  err, sizeof err) :
                 benchtest_stream((unsigned)op_row, stream_cells, stream_start, err, sizeof err);
        if (rc != 0) {
            static char e[sizeof err * 6];
            json_escape(e, sizeof e, err);
            snprintf(report, sizeof report, "{\"ok\":false,\"error\":\"%s\"}\n", e);
            stream_reply(409, report);
            return;
        }
        stream_started = true;
    } else if (benchtest_poll(report, sizeof report)) {
        stream_reply(200, report);
    }
}

// ---------------------------------------------------------------- settings / restart

bool api_busy(void) { return busy(); }
bool api_store_busy(void)
{
    return up_state != UP_IDLE || (op != OP_NONE && op != OP_STREAM) || restart_pending;
}

static bool busy(void)
{
    return up_state != UP_IDLE || op != OP_NONE || restart_pending;
}

static void get_settings(conn_t *c)
{
    static char h[CFG_HOSTNAME_MAX * 6 + 1], sd[CFG_SSID_MAX * 6 + 1], body[700];
    json_escape(h, sizeof h, config_hostname());
    json_escape(sd, sizeof sd, config_ssid());
    int n = snprintf(body, sizeof body,
        "{\"hostname\":\"%s\",\"ssid\":\"%s\",\"password_set\":%s,\"source\":\"%s\"}\n",
        h, sd, config_password()[0] ? "true" : "false", config_source_text());
    http_respond(c, 200, "application/json", body, (size_t)n, true, NULL);
}

static void settings_body_done(conn_t *c)
{
    // Body complete (lwIP context). Parse and check here; the flash write
    // happens in the main loop.
    char why[96];
    if (busy()) {
        http_respond_text(c, 409, "busy; try again\n");
        return;
    }
    if (!http_form_param(set_body, "ssid", set_ssid, sizeof set_ssid) ||
        !http_form_param(set_body, "hostname", set_host, sizeof set_host)) {
        http_respond_text(c, 400, "need ssid and hostname\n");
        return;
    }
    if (!http_form_param(set_body, "password", set_pw, sizeof set_pw))
        set_pw[0] = 0;                  // blank: keep the current password
    const char *bad = config_check(set_ssid, set_pw, set_host);
    if (bad) {
        snprintf(why, sizeof why, "%s\n", bad);
        memset(set_pw, 0, sizeof set_pw);
        http_respond(c, 400, "text/plain; charset=utf-8", why, strlen(why), true, NULL);
        return;
    }
    op = OP_SETTINGS;
    op_conn = c;
    http_set_on_close(c, op_closed);    // reply is sent by run_op()
}

static void post_settings(conn_t *c)
{
    long cl = http_content_length(c);
    if (busy())
        return http_respond_text(c, 409, "busy; try again\n");
    if (cl <= 0 || cl > (long)(sizeof set_body - 1))
        return http_respond_text(c, 400, "settings form missing or too long\n");
    memset(set_body, 0, sizeof set_body);
    http_receive_body(c, (uint8_t *)set_body, (size_t)cl, settings_body_done);
}

static void post_restart(conn_t *c)
{
    if (busy())
        return http_respond_text(c, 409, "busy; try again\n");
    restart_pending = true;
    restart_at = make_timeout_time_ms(1000);
    http_respond(c, 200, "application/json", "{\"ok\":true}\n", 12, false, NULL);
}

// ---------------------------------------------------------------- routing

static void method_not_allowed(conn_t *c, const char *allow)
{
    char h[48];
    snprintf(h, sizeof h, "Allow: %s\r\n", allow);
    http_respond(c, 405, "text/plain", "method not allowed\n", 19, false, h);  // h is copied
}

static void handle(conn_t *c)
{
    const char *m = http_method(c), *p = http_path(c);
    bool get = strcmp(m, "GET") == 0;
    bool put = strcmp(m, "PUT") == 0;
    bool del = strcmp(m, "DELETE") == 0;
    bool post = strcmp(m, "POST") == 0;

    if (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0) {
        if (!get)
            return method_not_allowed(c, "GET");
        http_respond(c, 200, "text/html; charset=utf-8", index_html, index_html_len, false, NULL);
    } else if (strcmp(p, "/status") == 0) {
        if (!get)
            return method_not_allowed(c, "GET");
        static char body[STATUS_JSON_MAX];     // lwIP context is serialized
        size_t n = status_json(body, sizeof body);
        http_respond(c, 200, "application/json", body, n, true, NULL);
    } else if (strcmp(p, "/images") == 0) {
        if (!get)
            return method_not_allowed(c, "GET");
        get_images(c);
    } else if (strncmp(p, "/image/", 7) == 0) {
        int row = parse_row(p + 7);
        if (row < 0)
            return http_respond_text(c, 404, "rows are 1 to 10\n");
        if (get)
            get_image(c, row);
        else if (put)
            put_image(c, row);
        else if (del)
            start_op(c, OP_DELETE, row);
        else
            method_not_allowed(c, "GET, PUT, DELETE");
    } else if (strncmp(p, "/mount/", 7) == 0) {
        int row = parse_row(p + 7);
        if (row < 0)
            return http_respond_text(c, 404, "rows are 1 to 10\n");
        if (!post)
            return method_not_allowed(c, "POST");
        start_op(c, OP_MOUNT, row);
    } else if (strncmp(p, "/rename/", 8) == 0) {
        int row = parse_row(p + 8);
        char raw[3 * IMAGE_NAME_MAX], msg[96];
        if (row < 0)
            return http_respond_text(c, 404, "rows are 1 to 10\n");
        if (!post)
            return method_not_allowed(c, "POST");
        if (!http_query_param(c, "name", raw, sizeof raw))
            return http_respond_text(c, 400, "need ?name=\n");
        if (busy())
            return http_respond_text(c, 409, "busy; try again\n");
        if (!rename_target(row, raw, op_name, msg, sizeof msg))
            return http_respond(c, 400, "text/plain; charset=utf-8", msg, strlen(msg), true, NULL);
        start_op(c, OP_RENAME, row);
    } else if (strcmp(p, "/settings") == 0) {
        if (get)
            get_settings(c);
        else if (post)
            post_settings(c);
        else
            method_not_allowed(c, "GET, POST");
    } else if (strcmp(p, "/restart") == 0) {
        if (!post)
            return method_not_allowed(c, "POST");
        post_restart(c);
    } else if (strcmp(p, "/eject") == 0) {
        if (!post)
            return method_not_allowed(c, "POST");
        start_op(c, OP_MOUNT, -1);
    } else if (strncmp(p, "/protect/", 9) == 0) {
        int row = parse_row(p + 9);
        char on[4];
        if (row < 0)
            return http_respond_text(c, 404, "rows are 1 to 10\n");
        if (!post)
            return method_not_allowed(c, "POST");
        if (!http_query_param(c, "on", on, sizeof on) || (strcmp(on, "0") && strcmp(on, "1")))
            return http_respond_text(c, 400, "need ?on=1 or ?on=0\n");
        op_on = on[0] == '0';           // stored as "writable" = not protected
        start_op(c, OP_WRITABLE, row);
    } else if (strcmp(p, "/image") == 0) {
        if (!get)
            return method_not_allowed(c, "GET");
        get_image(c, store_mounted());
    } else if (strcmp(p, "/test/stream") == 0) {
        char t[8] = "0";
        if (!post)
            return method_not_allowed(c, "POST");
        char n[12] = "0", st[12] = "0";
        http_query_param(c, "track", t, sizeof t);
        http_query_param(c, "cells", n, sizeof n);
        http_query_param(c, "start", st, sizeof st);
        if (busy())
            return http_respond_text(c, 409, "busy; try again\n");
        stream_cells = (uint32_t)strtoul(n, NULL, 10);
        stream_start = (uint32_t)strtoul(st, NULL, 10);
        test_kind = 0;
        start_op(c, OP_STREAM, atoi(t));
    } else if (strcmp(p, "/test/write") == 0) {
        char t[8] = "", sec[8] = "", cn[8] = "3910", sd[12] = "1";
        if (!post)
            return method_not_allowed(c, "POST");
        http_query_param(c, "track", t, sizeof t);
        http_query_param(c, "sector", sec, sizeof sec);
        http_query_param(c, "cell_ns", cn, sizeof cn);
        http_query_param(c, "seed", sd, sizeof sd);
        if (!t[0] || !sec[0])
            return http_respond_text(c, 400, "need ?track=N&sector=N (physical) [&cell_ns=3910&seed=N]\n");
        if (busy())
            return http_respond_text(c, 409, "busy; try again\n");
        test_kind = 2;
        test_sector = atoi(sec);
        test_cell_ns = (uint32_t)strtoul(cn, NULL, 10);
        test_seed = (uint32_t)strtoul(sd, NULL, 10);
        start_op(c, OP_STREAM, atoi(t));
    } else if (strcmp(p, "/test/seek") == 0) {
        char q[8] = "", t[8] = "", f[4] = "0";
        if (!post)
            return method_not_allowed(c, "POST");
        http_query_param(c, "q", q, sizeof q);
        http_query_param(c, "track", t, sizeof t);
        http_query_param(c, "flash", f, sizeof f);
        if (!q[0] && !t[0])
            return http_respond_text(c, 400, "need ?q=quarter-track or ?track=N\n");
        if (busy())
            return http_respond_text(c, 409, "busy; try again\n");
        test_kind = 1;
        test_flash = f[0] == '1';
        start_op(c, OP_STREAM, q[0] ? atoi(q) : 4 * atoi(t));
    } else if (strcmp(p, "/test/rdpulse") == 0) {
        // RDDATA sense, until restart: ?idle=low (default; Disk II analog
        // board) or ?idle=high. For trying the other sense on the //e.
        char v[8] = "";
        if (!post)
            return method_not_allowed(c, "POST");
        http_query_param(c, "idle", v, sizeof v);
        if (strcmp(v, "high") && strcmp(v, "low"))
            return http_respond_text(c, 400, "need ?idle=low or ?idle=high\n");
        player_set_idle_high(v[0] == 'h');
        static const char ok_low[] = "{\"ok\":true,\"rddata_idle\":\"low\"}\n";
        static const char ok_high[] = "{\"ok\":true,\"rddata_idle\":\"high\"}\n";
        if (v[0] == 'h')
            http_respond(c, 200, "application/json", ok_high, sizeof ok_high - 1, false, NULL);
        else
            http_respond(c, 200, "application/json", ok_low, sizeof ok_low - 1, false, NULL);
    } else if (strcmp(p, "/image.woz") == 0) {
        if (!get)
            return method_not_allowed(c, "GET");
        get_image_woz(c);
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
    int rc = store_write_finish(up_row, up_name, clock_unix(), err, sizeof err);

    static char e[sizeof err * 6];                      // static: large for the
    static char body[sizeof e + IMAGE_NAME_MAX * 6 + 200];  // shared IRQ stack
    int code;
    if (rc == 0) {
        image_info_t im;
        store_row(up_row, &im);
        static char name[IMAGE_NAME_MAX * 6], hex[65];         // static: main loop only
        json_escape(name, sizeof name, im.name);
        sha256_hex(im.sha256, hex);
        snprintf(body, sizeof body,
                 "{\"ok\":true,\"row\":%d,\"name\":\"%s\",\"format\":\"%s\",\"size\":%lu,\"sha256\":\"%s\"}\n",
                 up_row + 1, name, store_format_name(im.format), (unsigned long)im.size, hex);
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

    if (restart_pending && time_reached(restart_at)) {
        printf("restart requested\n");
        sleep_ms(50);                   // let the console drain
        watchdog_reboot(0, 0, 0);
        for (;;)
            ;
    }

    if (op == OP_STREAM) {
        poll_stream();
        return true;
    }
    if (op != OP_NONE) {
        run_op();
        return true;
    }
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
