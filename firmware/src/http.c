// Minimal HTTP/1.0 server on lwIP's raw TCP API.
//
// One request per connection ("Connection: close"). The request head is
// collected into a fixed buffer; the response is built in a malloc'd buffer
// and fed to tcp_write() as send-buffer space allows, continuing from the
// sent and poll callbacks. All callbacks run in the cyw43 async context with
// the lwIP lock held.
//
// Milestone 1 routes: GET / (HTML page) and GET /status (JSON). Request
// bodies are ignored; streaming PUT uploads arrive with milestone 2.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/tcp.h"

#include "http.h"
#include "status.h"

#define HTTP_PORT   80
#define REQ_MAX     1024
#define IDLE_POLLS  20      // tcp_poll interval 2 x 500 ms coarse ticks -> 20 s

typedef struct {
    struct tcp_pcb *pcb;
    char   *out;            // response (NULL until the request head is in)
    size_t  out_len, out_off;
    uint16_t len;           // bytes in req
    uint8_t idle;
    bool    closing;        // tcp_close() failed for lack of memory; retry
    char    req[REQ_MAX + 1];
} conn_t;

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
static err_t on_poll(void *arg, struct tcp_pcb *pcb);
static void  on_err(void *arg, err_t err);

static void conn_free(conn_t *c)
{
    free(c->out);
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
    c->closing = true;          // retry from on_poll
    tcp_arg(pcb, c);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 1);
    return ERR_OK;
}

// Queue as much of the response as fits; close once all of it is queued.
// Returns ERR_ABRT if the connection was aborted (caller must pass it on).
static err_t conn_push(conn_t *c)
{
    while (c->out_off < c->out_len) {
        size_t n = c->out_len - c->out_off;
        u16_t room = tcp_sndbuf(c->pcb);
        if (room == 0)
            break;
        if (n > room)
            n = room;
        err_t e = tcp_write(c->pcb, c->out + c->out_off, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM)
            break;              // resume from on_sent / on_poll
        if (e != ERR_OK)
            return conn_abort(c);
        c->out_off += n;
    }
    tcp_output(c->pcb);
    if (c->out_off == c->out_len)
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
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default:  return "";
    }
}

// Build the full response (head + body) in c->out.
static void respond(conn_t *c, int code, const char *ctype,
                    const char *body, size_t blen, const char *extra_hdr)
{
    char head[256];
    int hlen = snprintf(head, sizeof head,
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        code, reason(code), ctype, (unsigned)blen, extra_hdr ? extra_hdr : "");
    c->out = malloc((size_t)hlen + blen);
    if (!c->out) {
        c->out_len = 0;         // nothing to send; conn_push just closes
        return;
    }
    memcpy(c->out, head, (size_t)hlen);
    memcpy(c->out + hlen, body, blen);
    c->out_len = (size_t)hlen + blen;
    c->out_off = 0;
}

static void respond_text(conn_t *c, int code, const char *msg)
{
    respond(c, code, "text/plain; charset=utf-8", msg, strlen(msg),
            code == 405 ? "Allow: GET\r\n" : NULL);
}

static const char index_html[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>a2drive</title><style>"
":root{color-scheme:light dark}"
"body{font:15px -apple-system,system-ui,sans-serif;margin:2em auto;max-width:36em;padding:0 1em}"
"h1{font-size:1.3em}table{border-collapse:collapse}"
"td{padding:.25em 1em .25em 0;vertical-align:top}td:first-child{opacity:.6}"
"#err{color:#c33}"
"</style></head><body>"
"<h1>a2drive &mdash; WiFi Disk II</h1>"
"<table id=t></table><p id=err></p>"
"<script>"
"async function tick(){try{"
"const s=await (await fetch('/status',{cache:'no-store'})).json();"
"const t=document.getElementById('t');t.replaceChildren();"
"for(const[k,v]of Object.entries(s)){const r=t.insertRow();"
"r.insertCell().textContent=k;r.insertCell().textContent=v===null?'\\u2014':String(v)}"
"document.getElementById('err').textContent=''"
"}catch(e){document.getElementById('err').textContent='no response: '+e}}"
"tick();setInterval(tick,2000);"
"</script></body></html>";

static void handle_request(conn_t *c)
{
    // Request line: METHOD SP TARGET SP VERSION
    char *method = c->req;
    char *sp1 = strchr(method, ' ');
    if (!sp1) { respond_text(c, 400, "bad request line\n"); return; }
    *sp1 = 0;
    char *target = sp1 + 1;
    char *sp2 = strpbrk(target, " \r");
    if (!sp2) { respond_text(c, 400, "bad request line\n"); return; }
    *sp2 = 0;
    char *query = strchr(target, '?');
    if (query)
        *query++ = 0;

    bool is_get = strcmp(method, "GET") == 0;

    if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
        if (!is_get) { respond_text(c, 405, "GET only\n"); return; }
        respond(c, 200, "text/html; charset=utf-8",
                index_html, sizeof index_html - 1, NULL);
    } else if (strcmp(target, "/status") == 0) {
        if (!is_get) { respond_text(c, 405, "GET only\n"); return; }
        char body[STATUS_JSON_MAX];
        size_t n = status_json(body, sizeof body);
        respond(c, 200, "application/json", body, n, NULL);
    } else {
        respond_text(c, 404, "not found\n");
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = arg;
    (void)err;

    if (!p) {                   // peer closed its side
        if (c)
            return conn_close(c);
        tcp_close(pcb);
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);
    if (!c || c->out || c->closing) {   // not reading (body ignored for now)
        pbuf_free(p);
        return ERR_OK;
    }

    uint16_t room = REQ_MAX - c->len;
    uint16_t n = p->tot_len < room ? p->tot_len : room;
    pbuf_copy_partial(p, c->req + c->len, n, 0);
    c->len += n;
    c->req[c->len] = 0;
    pbuf_free(p);
    c->idle = 0;

    if (strstr(c->req, "\r\n\r\n"))
        handle_request(c);
    else if (c->len == REQ_MAX)
        respond_text(c, 431, "request head too large\n");
    else
        return ERR_OK;          // need more

    return conn_push(c);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    conn_t *c = arg;
    (void)pcb; (void)len;
    if (!c || c->closing || !c->out)
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
    if (++c->idle >= IDLE_POLLS)
        return conn_abort(c);
    if (c->out)
        return conn_push(c);
    return ERR_OK;
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
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 2);
    return ERR_OK;
}

void http_init(void)
{
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
