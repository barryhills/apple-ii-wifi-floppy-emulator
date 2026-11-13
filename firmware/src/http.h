#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Small HTTP/1.1 server engine on lwIP's raw TCP API, port 80.
// One request per connection ("Connection: close").
//
// All functions run in lwIP context: from the handler and callbacks, or
// from elsewhere with the lwIP lock held (cyw43_arch_lwip_begin/end).

typedef struct conn conn_t;
typedef void (*http_handler_t)(conn_t *c);
typedef void (*http_conn_fn)(conn_t *c);

// handler is called once per request when the head is complete. It must
// either respond, or call http_receive_body().
void http_init(http_handler_t handler);

const char *http_method(const conn_t *c);
const char *http_path(const conn_t *c);
long        http_content_length(const conn_t *c);   // -1 if absent
bool        http_query_param(const conn_t *c, const char *key, char *out, size_t n);

// Queue the response. If copy_body is false, body must stay valid until the
// connection closes (static data or flash).
void http_respond(conn_t *c, int code, const char *ctype, const void *body,
                  size_t len, bool copy_body, const char *extra_headers);
void http_respond_text(conn_t *c, int code, const char *msg);

// Stream exactly len body bytes into dst (answers "Expect: 100-continue").
// done() runs when all have arrived; it may respond at once, or leave the
// connection held and answer later with http_respond_deferred().
void http_receive_body(conn_t *c, uint8_t *dst, size_t len, http_conn_fn done);

// Answer a held connection from outside its callbacks (lwIP lock held).
// Copies the body. c must not be used afterwards.
void http_respond_deferred(conn_t *c, int code, const char *ctype,
                           const void *body, size_t len);

// fn runs once when the connection is freed, for any reason.
void http_set_on_close(conn_t *c, http_conn_fn fn);
