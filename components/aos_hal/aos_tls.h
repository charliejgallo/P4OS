/*
 * P4OS - The HAL's connection: a bare TCP socket or a TLS tunnel over it,
 * behind one type. Internal to the HAL, the same file on the board and in the
 * simulator (aos_tls.c).
 *
 * Users: aos_http.c (requests and aos_http_stream_*, the radio) and aos_ws.c
 * (Home Assistant over ws:// and wss://). Apps never see it: they have
 * aos_hal_http_* and aos_hal_tcp_*.
 *
 * Every call takes its own timeout, so one connection can do a 10 s HTTP
 * request, a 1 s radio read or a 100 ms WebSocket poll without anybody
 * touching the socket underneath.
 *
 * The struct is public so a caller can put it where it wants: on its stack
 * (aos_http.c's request task, as before) or in PSRAM (aos_tls_new()). An
 * mbedtls context is ~2 KB; its record buffers are allocated by mbedtls
 * itself and, on the board, land in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

/* aos_tls_open() flags */
#define AOS_TLS_ENCRYPT  0x01u   /* negotiate TLS; without it, a bare socket */
#define AOS_TLS_NODELAY  0x02u   /* TCP_NODELAY: small messages that each wait for an answer */

/* aos_tls_recv(): nothing arrived within the timeout. Not an error. */
#define AOS_TLS_TIMEOUT  (-2)

typedef struct {
    int                      fd;
    bool                     tls;
    int                      rcv_ms;    /* what SO_RCVTIMEO / SO_SNDTIMEO are set to now */
    int                      snd_ms;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         ca;        /* only used by the simulator; see trust_attach */
} aos_tls_t;

/* Connects (and with AOS_TLS_ENCRYPT verifies and negotiates) within
 * timeout_ms for the TCP connect and again for the handshake. 0, or one of
 * the AOS_HTTP_ERR_* of aos_hal.h: DNS, CONNECT, SIN_HORA (no valid time, or
 * the certificate is from the future, which is the same thing), TLS.
 * 'why', if not NULL, gets one line a person can read ("certificate for
 * 127.0.0.1 is not signed by a trusted authority"). On failure nothing is
 * left open. */
int  aos_tls_open(aos_tls_t *c, const char *host, int port, unsigned flags,
                  int timeout_ms, char *why, size_t why_n);

/* Everything or nothing: true if all 'len' bytes went out. Gives up once a
 * write has waited timeout_ms (plus one grace round on TLS). */
bool aos_tls_send(aos_tls_t *c, const void *buf, int len, int timeout_ms);

/* >0 bytes, 0 the other side closed cleanly (EOF or close_notify),
 * AOS_TLS_TIMEOUT nothing within timeout_ms, -1 broken. Bytes mbedtls has
 * already decrypted come back without waiting. */
int  aos_tls_recv(aos_tls_t *c, void *buf, int max, int timeout_ms);

/* Sends close_notify if it was TLS, frees everything, closes the socket.
 * Safe on a connection that failed to open or was already closed. */
void aos_tls_close(aos_tls_t *c);

/* A zeroed aos_tls_t in PSRAM on the board (plain calloc in the simulator),
 * for connections that outlive a function. aos_tls_free() closes it too. */
aos_tls_t *aos_tls_new(void);
void       aos_tls_free(aos_tls_t *c);
