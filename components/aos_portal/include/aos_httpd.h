/*
 * P4OS - a small HTTP/1.1 server (aos_httpd.c), the same file on the board
 * and in the simulator: lwIP gives the board the POSIX sockets macOS gives the
 * Mac, like aos_http.c on the client side.
 *
 * One request per connection (Connection: close), a thread per connection,
 * at most AOS_HTTPD_CONNS at a time. A handler reads the request line, the
 * query and the headers from the request, reads the body in pieces if there
 * is one (an upload of megabytes never sits in memory), and answers with one
 * of the send calls, or begin + write for something produced as it goes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Connections served at once. 4 on the watch, where each one's 12 KB stack
 * was internal RAM; on the P4 the stacks are in PSRAM, and a page, an
 * upload and the Monitor's polling took the 4 and answered 503 to the rest
 * (2026-09-29). */
#define AOS_HTTPD_CONNS 8

typedef struct aos_httpd_req aos_httpd_req_t;
typedef void (*aos_httpd_handler_t)(aos_httpd_req_t *r);

/* Starts the server on a port with a handler for everything. */
bool aos_httpd_start(int port, aos_httpd_handler_t handler);

const char *aos_httpd_method(aos_httpd_req_t *r);   /* "GET" */
const char *aos_httpd_path(aos_httpd_req_t *r);     /* "/api/fs", decoded, no query */
/* A query parameter, %-decoded; false if absent. */
bool        aos_httpd_query(aos_httpd_req_t *r, const char *key, char *out, size_t out_len);
long        aos_httpd_query_int(aos_httpd_req_t *r, const char *key, long def);
const char *aos_httpd_header(aos_httpd_req_t *r, const char *name);    /* case-insensitive, NULL if absent */
long        aos_httpd_body_len(aos_httpd_req_t *r);                    /* Content-Length, -1 if none */
/* The board's own address the request came in on ("192.168.4.1" on the AP,
 * "192.168.7.1" over the cable): the portal's rules depend on it. */
bool        aos_httpd_local_ip(aos_httpd_req_t *r, char *out, size_t out_len);
/* Reads up to len bytes of the body: bytes read, 0 at its end, -1 on error. */
int         aos_httpd_body_read(aos_httpd_req_t *r, void *buf, int len);
/* The whole body into a malloc'd, NUL-terminated buffer (free it); NULL if
 * larger than max or on error. */
char       *aos_httpd_body_all(aos_httpd_req_t *r, size_t max);

/* Answers */
void aos_httpd_send(aos_httpd_req_t *r, int status, const char *ctype, const void *data, size_t len);
void aos_httpd_send_text(aos_httpd_req_t *r, int status, const char *text);
void aos_httpd_send_json(aos_httpd_req_t *r, int status, const char *json);
/* For bodies produced piecewise. len -1: unknown, the connection's end marks
 * it. extra_headers are whole "Name: value\r\n" lines, or NULL. */
bool aos_httpd_begin(aos_httpd_req_t *r, int status, const char *ctype, long len, const char *extra_headers);
bool aos_httpd_write(aos_httpd_req_t *r, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
