/*
 * AmoledOS - Minimal HTTP client, shared between the board and the simulator.
 *
 * It is the same file on both platforms: lwIP gives the board the same POSIX
 * sockets macOS gives the simulator. All that changes is how the thread is
 * launched and how the lock is taken, and that is the eight lines below.
 *
 * Why by hand and not esp_http_client: the simulator does not have it, and for
 * a GET it is a hundred lines. The cost of maintaining it is lower than that
 * of having two different implementations and discovering the differences on
 * the board.
 *
 * Since the Remoto app came along it also sends POSTs with headers and a body
 * of their own (Home Assistant's REST API). It is the same path: the GET is a
 * POST with no body and no extra headers, not a separate function.
 *
 * HTTP/1.0 is requested on purpose. With 1.1 the server answers with
 * Transfer-Encoding chunked (measured against open-meteo) and it has to be
 * taken apart; with 1.0 it closes the connection on finishing and reading to
 * EOF is enough. Chunked and Content-Length are supported anyway, in case some
 * server uses them.
 *
 * --- TLS (2026-09-03) ------------------------------------------------------
 *
 * From now on it also speaks https://, and the API did NOT change: all that
 * can be seen from outside is that it stopped rejecting the scheme. For the
 * microSD's apps TLS is invisible —zero changes in the .so files,
 * AOS_ABI_VERSION is still 2— and that is why the whole job is in here and not
 * in a new symbol.
 *
 * The fear was that this would break the symmetry between the board and the
 * simulator and that the four #ifdefs would become forty. It did not happen,
 * and the reason is worth writing down: mbedtls 3.x is the same API on both
 * sides (the IDF ships 3.6.7, Homebrew 3.4.1), so setup, handshake, read,
 * write and close are common code. The ONLY thing that differs is where the
 * trusted certificate store comes from —the IDF's compiled-in bundle against
 * the Mac's /etc/ssl/cert.pem— and that is ONE function, trust_attach(). The
 * rest of the file does not know which platform it runs on.
 *
 * The other piece is that plain and TLS do not branch all over the place:
 * there is a conn_t with send/recv/close, and http_work() never asks again
 * whether there is encryption. That division is TLS-against-plain, not
 * board-against-Mac.
 *
 * Since 2026-09-28 that conn_t, trust_attach() and the session cache live in
 * aos_tls.c, because the WebSocket client (aos_ws.c) needed the same thing
 * for wss://. This file keeps its timeouts and its API; nothing it does
 * changed.
 *
 * What was measured and settles the design (docs/HANDOFF-HAL-RED-MIC.md,
 * section 3.1):
 *
 *  - With CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC the handshake costs 3.5 KB of
 *    internal RAM instead of 34.6, so HTTP_SLOTS stays at 3. Without that
 *    setting, two simultaneous requests sink internal RAM below the floor
 *    where the display's DMA buffers fail, and 3 would be irresponsible.
 *  - A full handshake is 1.6-1.8 s and a resumed one 0.6. That is why there is
 *    a session cache: without it the Remoto app would pay 1.6 s on every
 *    refresh.
 *  - With no valid time the certificate cannot be verified. That is checked
 *    BEFORE spending the handshake, and afterwards the BADCERT_FUTURE bit is
 *    translated, which is the only thing distinguishing "the board is in the
 *    past" from "I do not know that CA": both give the same return code.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aos_tls.h"

/* RAM audit: the http task's stack stays in internal RAM. Moving it to PSRAM
 * was tried (E1b) and crashed the board the moment Clima asked for the
 * weather: aos_hal_time_is_valid() reads a preference from NVS, and the
 * flash driver refuses a task whose stack is in PSRAM. */

#ifdef AOS_SIM
  #include <pthread.h>
  typedef pthread_mutex_t aos_lock_t;
  #define LOCK_INIT(l)    pthread_mutex_init(&(l), NULL)
  #define LOCK(l)         pthread_mutex_lock(&(l))
  #define UNLOCK(l)       pthread_mutex_unlock(&(l))
#else
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/semphr.h"
  #include "freertos/idf_additions.h"
  #include "esp_heap_caps.h"
  typedef SemaphoreHandle_t aos_lock_t;
  #define LOCK_INIT(l)    ((l) = xSemaphoreCreateMutex())
  #define LOCK(l)         xSemaphoreTake((l), portMAX_DELAY)
  #define UNLOCK(l)       xSemaphoreGive((l))
#endif

/* It stays at 3 because the measurement allows it: with EXTERNAL_MEM_ALLOC two
 * simultaneous handshakes leave 93 KB of internal RAM free and do not
 * fragment. Without that setting the same number is 22 KB and it has to be
 * dropped to 1. */
#define HTTP_SLOTS      3       /* weather + geocoding + one spare */
#define HTTP_TIMEOUT_S  10
#define TAG             "http"

typedef struct {
    int              id;        /* 0 = free */
    aos_http_state_t state;
    int              status;
    char            *body;      /* response */
    int              len;
    int              max;
    bool             abandoned; /* the app called release while it was still running */
    bool             tls;       /* the URL was https:// */
    char             host[96];
    char             path[512];
    int              port;

    /* Request. They are copied because the caller may have them on its stack
     * and the background task uses them long after that stack is gone. */
    char             method[8];
    char            *hdrs;      /* extra lines, or NULL */
    char            *req_body;  /* body, or NULL */
    int              req_len;
    char             ctype[48];
} slot_t;

AOS_BSS_PSRAM static slot_t    s_slots[HTTP_SLOTS];
static aos_lock_t s_lock;
static bool      s_ready;
static int       s_next_id = 1;

/* ==========================================================================
 * Transport: a bare socket or a TLS tunnel, behind the same interface.
 *
 * It lives in aos_tls.c since 2026-09-28, shared with the WebSocket client
 * (wss:// for Home Assistant): the trust store, the session cache, the "no
 * valid time" check and the error translation are there, word for word what
 * used to be here. What stays is this file's own policy on top: every wait is
 * HTTP_TIMEOUT_S, and a read that times out gets one grace round on TLS and
 * none on a bare socket, as before.
 *
 * http_work() further down uses conn_send/conn_recv/conn_close and never asks
 * again whether there is encryption. That division is TLS-against-plain, not
 * board-against-Mac.
 * ========================================================================== */

typedef aos_tls_t conn_t;

#define HTTP_TIMEOUT_MS (HTTP_TIMEOUT_S * 1000)

/* The same grace as aos_tls.c's writes: one more round in case the expiry fell
 * right on a split record, then give up. */
#define TLS_REINTENTOS 1

/* Opens the connection. With tls==false it is the usual socket; with true it
 * also negotiates the tunnel. Returns 0, or the appropriate AOS_HTTP_ERR_*. */
static int conn_open(conn_t *c, const char *host, int port, bool tls)
{
    return aos_tls_open(c, host, port, tls ? AOS_TLS_ENCRYPT : 0, HTTP_TIMEOUT_MS, NULL, 0);
}

static void conn_close(conn_t *c)
{
    aos_tls_close(c);
}

/* Sends everything or fails. Returns true if it all went out. */
static bool conn_send(conn_t *c, const char *buf, int len)
{
    return aos_tls_send(c, buf, len, HTTP_TIMEOUT_MS);
}

/* Just like recv(): >0 bytes, 0 if the other side closed cleanly, <0 if it
 * broke or HTTP_TIMEOUT_S went by without a byte. */
static int conn_recv(conn_t *c, char *buf, int max)
{
    for (int reintentos = 0; ; reintentos++) {
        int r = aos_tls_recv(c, buf, max, HTTP_TIMEOUT_MS);
        if (r != AOS_TLS_TIMEOUT) {
            return r;
        }
        if (!c->tls || reintentos >= TLS_REINTENTOS) {
            return -1;
        }
    }
}

static void slot_free(slot_t *s)     /* with the lock held */
{
    free(s->body);
    free(s->hdrs);
    free(s->req_body);
    memset(s, 0, sizeof(*s));
}

/* Unpacks a chunked body in place. Returns the new length. */
static int dechunk(char *body, int len)
{
    int r = 0, w = 0;
    while (r < len) {
        int size = 0, digits = 0;
        while (r < len) {
            char c = body[r];
            int v;
            if      (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            size = size * 16 + v;
            digits++;
            r++;
        }
        if (!digits) {
            break;
        }
        while (r < len && body[r] != '\n') r++;   /* rest of the line */
        r++;
        if (size == 0 || r + size > len) {
            break;
        }
        memmove(body + w, body + r, size);
        w += size;
        r += size + 2;                            /* the closing CRLF */
    }
    body[w] = 0;
    return w;
}

/* The real work. Runs in the background thread. */
static void http_work(slot_t *s)
{
    int status = AOS_HTTP_ERR_CONNECT;
    int blen   = 0;
    conn_t c;

    int rc = conn_open(&c, s->host, s->port, s->tls);
    if (rc != 0) {
        status = rc;
        goto done;
    }

    /* The header is built in a purpose-allocated buffer and not on the stack:
     * a Home Assistant token is ~180 characters and a template's body can go
     * past a kilobyte, and this runs in a task with a 5 KB stack. */
    int hlen  = s->hdrs     ? (int)strlen(s->hdrs) : 0;
    int cap   = 320 + (int)strlen(s->path) + (int)strlen(s->host) + hlen;
    char *req = malloc(cap);
    if (!req) {
        status = AOS_HTTP_ERR_MEM;
        conn_close(&c);
        goto done;
    }

    int n = snprintf(req, cap,
                     "%s %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: AmoledOS/1.0\r\n"
                     "Accept: application/json\r\n",
                     s->method, s->path, s->host);
    if (hlen) {
        n += snprintf(req + n, cap - n, "%s", s->hdrs);
    }
    /* Content-Length always goes whenever there is a body, including a
     * zero-byte one: several servers (Home Assistant's among them) answer 411
     * if a POST arrives without it. */
    if (s->req_body) {
        n += snprintf(req + n, cap - n,
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n",
                      s->ctype[0] ? s->ctype : "application/json", s->req_len);
    }
    n += snprintf(req + n, cap - n, "\r\n");

    if (n <= 0 || n >= cap) {
        status = AOS_HTTP_ERR_PROTO;
        free(req);
        conn_close(&c);
        goto done;
    }

    /* Header and body are sent in two goes over the same connection. */
    bool sent_ok = true;
    for (int part = 0; part < 2 && sent_ok; part++) {
        const char *buf = part ? s->req_body : req;
        int         len = part ? s->req_len  : n;
        if (!buf || len <= 0) {
            continue;
        }
        sent_ok = conn_send(&c, buf, len);
    }
    free(req);
    if (!sent_ok) {
        status = AOS_HTTP_ERR_SEND;
        conn_close(&c);
        goto done;
    }

    /* Everything (headers included) is read into the same buffer and then the
     * body is moved to the front. A GET of this size does not justify two. */
    int got = 0;
    for (;;) {
        int r = conn_recv(&c, s->body + got, s->max - 1 - got);
        if (r < 0) {
            if (got == 0) {
                status = AOS_HTTP_ERR_RECV;
                conn_close(&c);
                goto done;
            }
            break;                       /* it was cut, but something arrived */
        }
        if (r == 0) {
            break;                       /* clean close */
        }
        got += r;
        if (got >= s->max - 1) {
            break;                       /* the ceiling the app asked for */
        }
    }
    conn_close(&c);
    s->body[got] = 0;

    if (got < 13 || strncmp(s->body, "HTTP/1.", 7) != 0) {
        status = AOS_HTTP_ERR_PROTO;
        goto done;
    }
    status = atoi(s->body + 9);

    char *sep = strstr(s->body, "\r\n\r\n");
    if (!sep) {
        status = AOS_HTTP_ERR_PROTO;
        goto done;
    }
    *sep = 0;
    bool chunked = (strstr(s->body, "chunked") != NULL);
    char *body = sep + 4;
    blen = got - (int)(body - s->body);
    memmove(s->body, body, blen);
    s->body[blen] = 0;
    if (chunked) {
        blen = dechunk(s->body, blen);
    }

done:
    LOCK(s_lock);
    if (s->abandoned) {
        slot_free(s);
    } else {
        s->status = status;
        s->len    = blen;
        s->state  = (status >= 200 && status < 400) ? AOS_HTTP_DONE : AOS_HTTP_FAILED;
    }
    UNLOCK(s_lock);
}

#ifdef AOS_SIM
static void *http_thread(void *arg)
{
    http_work((slot_t *)arg);
    return NULL;
}
#else
static void http_task(void *arg)
{
    http_work((slot_t *)arg);
    vTaskDeleteWithCaps(NULL);
}
#endif

/* -------------------------------------------------------------------------- */

static slot_t *find(int id)      /* with the lock held */
{
    if (id <= 0) {
        return NULL;
    }
    for (int i = 0; i < HTTP_SLOTS; i++) {
        /* An abandoned one keeps its id so the slot is not reused while its
         * thread is still alive, but nobody finds it any more. */
        if (s_slots[i].id == id && !s_slots[i].abandoned) {
            return &s_slots[i];
        }
    }
    return NULL;
}

int aos_hal_http_get(const char *url, int max_bytes)
{
    return aos_hal_http_request("GET", url, NULL, NULL, NULL, max_bytes);
}

int aos_hal_http_request(const char *method, const char *url,
                         const char *headers, const char *body,
                         const char *content_type, int max_bytes)
{
    bool tls;
    int  esquema;
    if (url && strncmp(url, "http://", 7) == 0) {
        tls = false; esquema = 7;
    } else if (url && strncmp(url, "https://", 8) == 0) {
        tls = true;  esquema = 8;
    } else {
        return -1;
    }
    if (!method || !method[0] || strlen(method) >= 8) {
        return -1;
    }
    if (max_bytes < 512) {
        max_bytes = 512;
    }

    /* The lock is created on the first request and not in a separate init. It
     * is safe because this function is called by the apps, and the apps always
     * run in the LVGL thread: there are no two first calls at once. Background
     * tasks only take it, never create it. */
    if (!s_ready) {
        LOCK_INIT(s_lock);
        s_ready = true;
    }

    /* host[:port]/path */
    const char *h = url + esquema;
    const char *slash = strchr(h, '/');
    const char *colon = strchr(h, ':');
    if (colon && slash && colon > slash) {
        colon = NULL;
    }
    int hlen = (int)((colon ? colon : (slash ? slash : h + strlen(h))) - h);
    if (hlen <= 0 || hlen >= 96) {
        return -1;
    }

    LOCK(s_lock);
    slot_t *s = NULL;
    for (int i = 0; i < HTTP_SLOTS; i++) {
        if (s_slots[i].id == 0) {
            s = &s_slots[i];
            break;
        }
    }
    if (!s) {
        UNLOCK(s_lock);
        return -2;                      /* all busy */
    }

    memset(s, 0, sizeof(*s));
    memcpy(s->host, h, hlen);
    s->host[hlen] = 0;
    s->tls  = tls;
    s->port = colon ? atoi(colon + 1) : (tls ? 443 : 80);
    snprintf(s->path, sizeof(s->path), "%s", slash ? slash : "/");
    snprintf(s->method, sizeof(s->method), "%s", method);
    if (content_type) {
        snprintf(s->ctype, sizeof(s->ctype), "%s", content_type);
    }

    /* Headers and body are copied here, with the lock held and before the task
     * exists: the caller may have them on its stack. */
    if (headers && headers[0]) {
        size_t n = strlen(headers);
        s->hdrs = malloc(n + 1);
        if (!s->hdrs) {
            memset(s, 0, sizeof(*s));
            UNLOCK(s_lock);
            return -3;
        }
        memcpy(s->hdrs, headers, n + 1);
    }
    if (body) {
        s->req_len = (int)strlen(body);
        s->req_body = malloc((size_t)s->req_len + 1);
        if (!s->req_body) {
            slot_free(s);
            UNLOCK(s_lock);
            return -3;
        }
        memcpy(s->req_body, body, (size_t)s->req_len + 1);
    }

#ifdef AOS_SIM
    s->body = malloc(max_bytes);
#else
    /* To PSRAM: it is a few kilobytes that have no reason to come out of
     * internal RAM. */
    s->body = heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->body) {
        s->body = malloc(max_bytes);
    }
#endif
    if (!s->body) {
        slot_free(s);
        UNLOCK(s_lock);
        return -3;
    }
    s->max   = max_bytes;
    s->state = AOS_HTTP_BUSY;
    s->id    = s_next_id++;
    if (s_next_id <= 0) {
        s_next_id = 1;
    }
    int id = s->id;
    UNLOCK(s_lock);

#ifdef AOS_SIM
    pthread_t th;
    if (pthread_create(&th, NULL, http_thread, s) != 0) {
        LOCK(s_lock);
        slot_free(s);
        UNLOCK(s_lock);
        return -4;
    }
    pthread_detach(th);
#else
    /* 5120 is enough for plain HTTP: the deepest point is getaddrinfo. With
     * TLS the deepest point becomes verifying the certificate chain, and more
     * is needed. Both numbers are MEASURED with uxTaskGetStackHighWaterMark()
     * on the board, not picked out of the air: plain uses 2960 bytes, and
     * encrypted 4432 in the worst case seen over the chains of open-meteo,
     * howsmyssl and badssl.
     *
     * Dropping it to 6144 to save internal RAM was tried and reverted: there
     * the worst case left 1712 bytes free, 28%. That sounds like enough and it
     * is not —what is measured is THREE certificate chains, and verification
     * is recursive: a chain deeper than these uses more, and the failure mode
     * is a stack overflow, not an error—. With 7168 the worst case observed
     * leaves 2672 (37%). The kilobyte of difference per request changes
     * nothing: with all three encrypted at once the minimum general executable
     * heap is 21,828 with 6144 and 18,572 with 7168, and the apps do not eat
     * from there —their 48 KB contiguous reservation is asked for at startup,
     * before the network exists—.
     *
     * It is only paid when the URL is https. On the P4 the stack is in PSRAM
     * (nothing here touches the flash; a preference goes through
     * aos_flash_call). On the ESP32-S3 stacks were internal RAM, and with all
     * three requests encrypted at once the internal peak was 27.9 KB.
     *
     * Priority 4, the same as LVGL, because it spends nearly all its time
     * waiting. */
    if (xTaskCreateWithCaps(http_task, "aos_http", s->tls ? 7168 : 5120, s, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        LOCK(s_lock);
        slot_free(s);
        UNLOCK(s_lock);
        return -4;
    }
#endif
    return id;
}

aos_http_state_t aos_hal_http_state(int id)
{
    if (!s_ready) {
        return AOS_HTTP_FAILED;
    }
    LOCK(s_lock);
    slot_t *s = find(id);
    aos_http_state_t st = s ? s->state : AOS_HTTP_FAILED;
    UNLOCK(s_lock);
    return st;
}

int aos_hal_http_status(int id)
{
    if (!s_ready) {
        return AOS_HTTP_ERR_MEM;
    }
    LOCK(s_lock);
    slot_t *s = find(id);
    int st = s ? s->status : AOS_HTTP_ERR_MEM;
    UNLOCK(s_lock);
    return st;
}

const char *aos_hal_http_body(int id)
{
    if (!s_ready) {
        return NULL;
    }
    LOCK(s_lock);
    slot_t *s = find(id);
    const char *b = (s && s->state != AOS_HTTP_BUSY) ? s->body : NULL;
    UNLOCK(s_lock);
    return b;
}

int aos_hal_http_len(int id)
{
    if (!s_ready) {
        return 0;
    }
    LOCK(s_lock);
    slot_t *s = find(id);
    int n = (s && s->state != AOS_HTTP_BUSY) ? s->len : 0;
    UNLOCK(s_lock);
    return n;
}

void aos_hal_http_release(int id)
{
    if (!s_ready) {
        return;
    }
    LOCK(s_lock);
    slot_t *s = find(id);
    if (s) {
        if (s->state == AOS_HTTP_BUSY) {
            /* The thread is still inside the recv: the buffer cannot be pulled
             * out from under it. It is marked and the thread itself cleans up
             * on the way out. */
            s->abandoned = true;
        } else {
            slot_free(s);
        }
    }
    UNLOCK(s_lock);
}

/* ==========================================================================
 * A connection that stays open (branch radio).
 *
 * The same transport - plain or TLS, the same trust store, the same session
 * cache - for something that is not one request and one body: an internet
 * radio sends audio for as long as it is listened to. aos_radio.c speaks the
 * HTTP and the ICY on top; all this does is lend it conn_t.
 *
 * The one difference with http_work()'s sockets is the receive timeout. There
 * it is HTTP_TIMEOUT_S, here one second, so the reader can notice it was
 * asked to stop; a timeout comes back as "nothing yet" (-2), not as an error.
 * The conn_t lives in PSRAM: an mbedtls context is ~1.5 KB, and its record
 * buffers are there already (EXTERNAL_MEM_ALLOC).
 * ========================================================================== */
#include "aos_http_stream.h"

#define STREAM_RECV_MS  1000

struct aos_http_stream {
    conn_t c;
};

void aos_http_stream_init(void)
{
    if (!s_ready) {
        LOCK_INIT(s_lock);
        s_ready = true;
    }
}

int aos_http_stream_open(aos_http_stream_t **out, const char *host, int port, bool tls)
{
    *out = NULL;
    aos_http_stream_init();
#ifdef AOS_SIM
    aos_http_stream_t *s = calloc(1, sizeof(*s));
#else
    aos_http_stream_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
#endif
    if (!s) {
        return AOS_HTTP_ERR_MEM;
    }
    int rc = conn_open(&s->c, host, port, tls);
    if (rc != 0) {
        free(s);
        return rc;
    }
    *out = s;
    return 0;
}

bool aos_http_stream_send(aos_http_stream_t *s, const char *buf, int len)
{
    return s && conn_send(&s->c, buf, len);
}

int aos_http_stream_recv(aos_http_stream_t *s, void *buf, int max)
{
    if (!s) {
        return -1;
    }
    /* AOS_TLS_TIMEOUT is -2, which is this call's "nothing yet". */
    return aos_tls_recv(&s->c, buf, max, STREAM_RECV_MS);
}

void aos_http_stream_close(aos_http_stream_t *s)
{
    if (s) {
        conn_close(&s->c);
        free(s);
    }
}
