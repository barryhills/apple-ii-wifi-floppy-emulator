// Small HTTP/1.1 server engine on lwIP's raw TCP API.
//
// Connection life cycle (one request per connection):
//
//   HEAD     collecting the request head (up to REQ_MAX bytes)
//   BODY     streaming a request body into the handler's buffer
//   DISCARD  draining a body nobody wanted (so the client reads our reply)
//   HELD     body complete, waiting for a deferred response
//   SEND     response queued; close once it is all handed to TCP
//
// The response is a malloc'd head plus a body that is either copied to the
// heap or referenced in place (static data, flash). It is fed to tcp_write()
// as send-buffer space allows, continuing from the sent and poll callbacks.
// All callbacks run in the cyw43 async context with the lwIP lock held.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lwip/tcp.h"

#include "http.h"

#define HTTP_PORT   80
#define REQ_MAX     1024
#define POLL_TICKS  2       // tcp_poll interval in 500 ms ticks -> 1 s
#define IDLE_POLLS  20      // -> 20 s without progress closes the connection

typedef enum { ST_HEAD, ST_BODY, ST_DISCARD, ST_HELD, ST_SEND } st_t;

struct conn {
    struct tcp_pcb *pcb;
    st_t     st;
    uint8_t  idle;
    bool     closing;           // tcp_close() failed for lack of memory; retry

    // request
    uint16_t len;               // bytes in req
    const char *method, *path, *query;
    long     content_length;
    bool     expect_continue;

    // request body
    bool     want_body;
    uint8_t *body_dst;          // copy the body here, or
    http_sink_fn sink;          // hand it to sink (TCP ack deferred)
    size_t   body_len, body_got;
    size_t   held_now;          // sink bytes in the current on_recv call
    http_conn_fn body_done;
    size_t   discard_left;

    // response
    bool     responded;
    char    *head;
    size_t   hlen, hoff;
    const uint8_t *body;
    size_t   blen, boff;
    void    *body_heap;
    http_fill_fn fill;          // body generated on demand (body == NULL)
    void    *fill_ctx;
    void    *user;

    http_conn_fn on_close;
    char     req[REQ_MAX + 1];
};

static http_handler_t handler;
static uint8_t fill_buf[HTTP_FILL_MAX];   // lwIP context is single-threaded

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
static err_t on_poll(void *arg, struct tcp_pcb *pcb);
static void  on_err(void *arg, err_t err);

// ---------------------------------------------------------------- teardown

static void conn_free(conn_t *c)
{
    if (c->on_close)
        c->on_close(c);
    free(c->head);
    free(c->body_heap);
    free(c);
}

static void conn_detach(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
}

static err_t conn_abort(conn_t *c)
{
    conn_detach(c->pcb);
    tcp_abort(c->pcb);
    conn_free(c);
    return ERR_ABRT;
}

// Graceful close: lwIP still delivers queued data, then sends FIN.
static err_t conn_close(conn_t *c)
{
    struct tcp_pcb *pcb = c->pcb;
    conn_detach(pcb);
    if (tcp_close(pcb) == ERR_OK) {
        conn_free(c);
        return ERR_OK;
    }
    c->closing = true;
    tcp_arg(pcb, c);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, POLL_TICKS);
    return ERR_OK;
}

// ---------------------------------------------------------------- sending

// Queue as much of the response as fits; close when all of it is queued
// and the request side is finished. Returns ERR_ABRT if aborted.
static err_t conn_push(conn_t *c)
{
    if (!c->responded)
        return ERR_OK;
    for (;;) {
        const uint8_t *src;
        size_t left;
        bool in_head = c->hoff < c->hlen;
        if (in_head) {
            src = (const uint8_t *)c->head + c->hoff;
            left = c->hlen - c->hoff;
        } else if (c->boff < c->blen) {
            src = c->fill ? fill_buf : c->body + c->boff;
            left = c->blen - c->boff;
        } else {
            break;
        }
        size_t n = tcp_sndbuf(c->pcb);
        if (n == 0)
            break;
        if (n > left)
            n = left;
        if (!in_head && c->fill) {
            if (n > HTTP_FILL_MAX)
                n = HTTP_FILL_MAX;
            c->fill(c->fill_ctx, c->boff, fill_buf, n);
        }
        err_t e;
        while ((e = tcp_write(c->pcb, src, (u16_t)n, TCP_WRITE_FLAG_COPY)) == ERR_MEM && n > 512)
            n /= 2;             // heap tight: try a smaller piece
        if (e == ERR_MEM)
            break;              // resume from on_sent / on_poll
        if (e != ERR_OK)
            return conn_abort(c);
        if (in_head)
            c->hoff += n;
        else
            c->boff += n;
    }
    tcp_output(c->pcb);
    if (c->st == ST_SEND && c->hoff == c->hlen && c->boff == c->blen)
        return conn_close(c);
    return ERR_OK;
}

static const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default:  return "";
    }
}

void http_respond(conn_t *c, int code, const char *ctype, const void *body,
                  size_t len, bool copy_body, const char *extra_headers)
{
    if (c->responded)
        return;
    c->responded = true;
    c->head = malloc(512);
    if (!c->head)
        return;                 // nothing to send; the connection just closes
    int n = snprintf(c->head, 512,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        code, reason(code), ctype, (unsigned)len, extra_headers ? extra_headers : "");
    if (n < 0 || n >= 512) {
        free(c->head);
        c->head = NULL;
        return;
    }
    c->hlen = (size_t)n;
    if (len && copy_body) {
        c->body_heap = malloc(len);
        if (!c->body_heap) {
            c->hlen = 0;
            return;
        }
        memcpy(c->body_heap, body, len);
        body = c->body_heap;
    }
    c->body = body;
    c->blen = len;
}

void http_respond_stream(conn_t *c, int code, const char *ctype, size_t len,
                         http_fill_fn fill, void *ctx, const char *extra_headers)
{
    http_respond(c, code, ctype, NULL, len, false, extra_headers);
    c->fill = fill;
    c->fill_ctx = ctx;
}

void  http_set_user(conn_t *c, void *p) { c->user = p; }
void *http_user(const conn_t *c)        { return c->user; }

void http_respond_text(conn_t *c, int code, const char *msg)
{
    http_respond(c, code, "text/plain; charset=utf-8", msg, strlen(msg), false, NULL);
}

void http_respond_deferred(conn_t *c, int code, const char *ctype,
                           const void *body, size_t len)
{
    http_respond(c, code, ctype, body, len, true, NULL);
    if (c->st == ST_HELD) {
        c->st = ST_SEND;
    } else if (c->st == ST_BODY) {
        // Answered early (error): drain the rest so the client reads it.
        c->sink = NULL;
        c->discard_left = c->body_len - c->body_got;
        c->st = c->discard_left ? ST_DISCARD : ST_SEND;
    }
    conn_push(c);
}

// ---------------------------------------------------------------- request

const char *http_method(const conn_t *c)   { return c->method; }
const char *http_path(const conn_t *c)     { return c->path; }
long http_content_length(const conn_t *c)  { return c->content_length; }

void http_set_on_close(conn_t *c, http_conn_fn fn) { c->on_close = fn; }

void http_stream_body(conn_t *c, size_t len, http_sink_fn sink, http_conn_fn done)
{
    c->want_body = true;
    c->sink = sink;
    c->body_len = len;
    c->body_got = 0;
    c->body_done = done;
}

void http_body_consumed(conn_t *c, size_t n)
{
    while (n) {
        u16_t k = n > 0xFFFF ? 0xFFFF : (u16_t)n;
        tcp_recved(c->pcb, k);
        n -= k;
    }
}

void http_receive_body(conn_t *c, uint8_t *dst, size_t len, http_conn_fn done)
{
    c->want_body = true;
    c->body_dst = dst;
    c->body_len = len;
    c->body_got = 0;
    c->body_done = done;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void url_decode(char *out, size_t n, const char *s, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        char ch = s[i];
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%' && i + 2 < len && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            ch = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        }
        out[o++] = ch;
    }
    out[o] = 0;
}

bool http_query_param(const conn_t *c, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *q = c->query; q && *q; ) {
        const char *amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        if (seg > kl && q[kl] == '=' && strncmp(q, key, kl) == 0) {
            url_decode(out, n, q + kl + 1, seg - kl - 1);
            return true;
        }
        q = amp ? amp + 1 : NULL;
    }
    return false;
}

// Split the head in place. head_len includes the final blank line.
static bool parse_head(conn_t *c, size_t head_len)
{
    char *eol = strstr(c->req, "\r\n");
    *eol = 0;
    char *method = c->req;
    char *sp1 = strchr(method, ' ');
    if (!sp1)
        return false;
    *sp1 = 0;
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2)
        return false;
    *sp2 = 0;
    char *query = strchr(target, '?');
    if (query)
        *query++ = 0;
    c->method = method;
    c->path = target;
    c->query = query ? query : "";

    c->content_length = -1;
    char *h = eol + 2;
    char *end = c->req + head_len - 2;          // the blank line's CRLF
    while (h < end) {
        char *e = strstr(h, "\r\n");
        if (!e)
            break;
        *e = 0;
        char *colon = strchr(h, ':');
        if (colon) {
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            if (strcasecmp(h, "Content-Length") == 0) {
                char *ep;
                long n = strtol(v, &ep, 10);
                if (ep == v || n < 0)
                    return false;
                c->content_length = n;
            } else if (strcasecmp(h, "Expect") == 0 && strcasecmp(v, "100-continue") == 0) {
                c->expect_continue = true;
            }
        }
        h = e + 2;
    }
    return true;
}

// Consume request-body bytes according to the current state.
static void feed(conn_t *c, const uint8_t *d, size_t n)
{
    if (n == 0)
        return;
    if (c->st == ST_BODY) {
        size_t take = c->body_len - c->body_got;
        if (take > n)
            take = n;
        if (c->sink) {
            c->sink(c, d, take);
            c->held_now += take;
        } else {
            memcpy(c->body_dst + c->body_got, d, take);
        }
        c->body_got += take;
        if (c->body_got == c->body_len) {
            c->st = ST_HELD;
            c->body_done(c);
            if (c->responded)
                c->st = ST_SEND;
        }
    } else if (c->st == ST_DISCARD) {
        size_t take = c->discard_left < n ? c->discard_left : n;
        c->discard_left -= take;
        if (c->discard_left == 0)
            c->st = ST_SEND;
    }
}

static void feed_pbuf(conn_t *c, const struct pbuf *p, size_t off)
{
    for (const struct pbuf *q = p; q; q = q->next) {
        if (off >= q->len) {
            off -= q->len;
            continue;
        }
        feed(c, (const uint8_t *)q->payload + off, q->len - off);
        off = 0;
    }
}

// Head is complete: dispatch, then set up the body phase.
static void start_request(conn_t *c, size_t head_len)
{
    if (!parse_head(c, head_len)) {
        http_respond_text(c, 400, "bad request\n");
        c->st = ST_SEND;
        return;
    }
    handler(c);

    if (c->want_body && !c->responded) {
        c->st = ST_BODY;
        if (c->expect_continue)
            tcp_write(c->pcb, "HTTP/1.1 100 Continue\r\n\r\n", 25, TCP_WRITE_FLAG_COPY);
        return;
    }
    if (!c->responded)
        http_respond_text(c, 500, "no response\n");
    if (c->content_length > 0 && !c->expect_continue) {
        c->st = ST_DISCARD;     // read it all so the client sees our reply
        c->discard_left = (size_t)c->content_length;
    } else {
        c->st = ST_SEND;
    }
}

// ---------------------------------------------------------------- callbacks

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = arg;
    (void)err;

    if (!p) {                                   // peer finished sending
        if (!c) {
            tcp_close(pcb);
            return ERR_OK;
        }
        if (c->st == ST_SEND || c->st == ST_HELD)
            return ERR_OK;                      // still owe a reply
        return conn_close(c);
    }
    if (!c || c->closing) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }
    c->idle = 0;
    c->held_now = 0;
    u16_t tot = p->tot_len;

    if (c->st == ST_HEAD) {
        uint16_t room = REQ_MAX - c->len;
        uint16_t n = p->tot_len < room ? p->tot_len : room;
        pbuf_copy_partial(p, c->req + c->len, n, 0);
        c->len += n;
        c->req[c->len] = 0;
        char *end = strstr(c->req, "\r\n\r\n");
        if (!end) {
            tcp_recved(pcb, tot);
            pbuf_free(p);
            if (c->len < REQ_MAX)
                return ERR_OK;                  // need more
            http_respond_text(c, 431, "request head too large\n");
            c->st = ST_SEND;
            return conn_push(c);
        }
        size_t head_len = (size_t)(end + 4 - c->req);
        size_t extra = c->len - head_len;       // body bytes already copied
        start_request(c, head_len);
        feed(c, (const uint8_t *)c->req + head_len, extra);
        feed_pbuf(c, p, n);                     // the part not copied
    } else {
        feed_pbuf(c, p, 0);
    }
    pbuf_free(p);
    // Acknowledge everything except bytes a sink still holds; the consumer
    // acknowledges those through http_body_consumed().
    if (tot > c->held_now)
        tcp_recved(pcb, (u16_t)(tot - c->held_now));
    return conn_push(c);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    conn_t *c = arg;
    (void)pcb; (void)len;
    if (!c || c->closing)
        return ERR_OK;
    c->idle = 0;
    return conn_push(c);
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
    conn_t *c = arg;
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (c->closing)
        return conn_close(c);
    if (c->st != ST_HELD && ++c->idle >= IDLE_POLLS)
        return conn_abort(c);
    return conn_push(c);
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    if (arg)                    // the pcb is already gone
        conn_free(arg);
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb)
        return ERR_VAL;
    conn_t *c = calloc(1, sizeof *c);
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    c->pcb = pcb;
    c->content_length = -1;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, POLL_TICKS);
    return ERR_OK;
}

void http_init(http_handler_t h)
{
    handler = h;
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        printf("http: tcp_new failed\n");
        return;
    }
    err_t e = tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT);
    if (e != ERR_OK) {
        printf("http: bind failed %d\n", e);
        tcp_close(pcb);
        return;
    }
    struct tcp_pcb *lpcb = tcp_listen_with_backlog(pcb, 4);
    if (!lpcb) {
        printf("http: listen failed\n");
        tcp_close(pcb);
        return;
    }
    tcp_accept(lpcb, on_accept);
}
