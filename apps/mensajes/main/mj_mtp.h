/*
 * P4OS - Mensajes: MTProto 2.0, the connections to Telegram's data centres.
 *
 * One mj_mtp_t holds a connection per data centre in use (the home one,
 * always, and the one of a file while it downloads: the HAL has four TCP
 * slots for the whole system, so a second one is closed once idle). It all
 * runs in ONE thread, the app's network thread, which calls mj_mtp_poll()
 * in a loop; nothing here is thread-safe and nothing here touches LVGL.
 *
 *   transport  TCP, "intermediate" framing (0xeeeeeeee, then a length and
 *              the packet). A negative 4-byte packet is the server's
 *              transport error (-404: no such auth key).
 *   auth key   created the first time a data centre is used
 *              (core.telegram.org/mtproto/auth_key): req_pq_multi, pq,
 *              RSA_PAD with the server's key, Diffie-Hellman, checked
 *              against the known prime. On the board 1-2 s. The key goes
 *              to the caller (on_key) to be kept.
 *   messages   sealed with AES-256-IGE and msg_key from SHA-256; msg_id
 *              from the server's clock (the offset learnt in the
 *              handshake and from bad_msg_notification), seqno, acks sent
 *              within a second or with the next request, containers and
 *              gzip_packed read, the salt changed when the server says.
 *   requests   each one waits for its rpc_result and its callback gets it.
 *              Retried by themselves: after a new salt or a clock fix, after
 *              a dropped connection (all that had no answer), after
 *              FLOOD_WAIT_X up to a minute, after a 500. The first request of
 *              each session goes wrapped in invokeWithLayer(initConnection).
 *   updates    anything that is not an answer and not a service message
 *              (updates, updateShort...) goes to on_updates, from the home
 *              data centre.
 *
 * Data centres are 1..5, at Telegram's addresses, or a server of the
 * caller's (mj_mtp_cfg_t.host: data centre N is host:port+N-1), the way the
 * tests reach tools/fake_telegram.py.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mj_tl.h"

#define MJ_DC_MAX 5

typedef struct mj_mtp mj_mtp_t;

typedef struct {
    int code;                   /* 0 ok; the rpc_error's code; -1 the request was given up */
    const char *msg;            /* the rpc_error's text ("PHONE_MIGRATE_2"), or why it was given up */
    tl_val_t val;               /* the answer, when code == 0 (in arena)     */
    mj_arena_t *arena;          /* freed when the callback returns           */
    int dc;
} mj_rpc_t;

typedef void (*mj_rpc_cb_t)(void *ctx, const mj_rpc_t *r);

typedef struct {
    /* what initConnection says of us */
    int32_t api_id;
    const char *device_model, *system_version, *app_version, *lang_code;
    /* a server of our own instead of Telegram's (tests): data centre N at
     * host:port+N-1, with this RSA key (256 bytes, big-endian, e 65537) */
    const char *host;
    int port;
    const uint8_t *rsa_n;
    /* the caller's side */
    void (*on_updates)(void *ud, int dc, tl_obj_t *u, mj_arena_t *a);
    void (*on_new_session)(void *ud, int dc);
    void (*on_key)(void *ud, int dc, const uint8_t *key);   /* a key made (NULL: forgotten) */
    void (*on_state)(void *ud, int dc, int state);          /* MJ_NET_*      */
    void *ud;
    volatile bool *stop;        /* checked during the long waits             */
} mj_mtp_cfg_t;

enum { MJ_NET_DOWN, MJ_NET_CONNECTING, MJ_NET_KEY, MJ_NET_UP };

mj_mtp_t *mj_mtp_new(const mj_mtp_cfg_t *cfg);
void      mj_mtp_free(mj_mtp_t *m);
/* The api_id for initConnection, when it is known only after starting. */
void      mj_mtp_set_api_id(mj_mtp_t *m, int32_t api_id);
/* A key kept from before (256 bytes), so no handshake is needed. */
void      mj_mtp_set_key(mj_mtp_t *m, int dc, const uint8_t *key);
bool      mj_mtp_has_key(mj_mtp_t *m, int dc);
/* Forgets the key of a data centre (the server no longer knows it). */
void      mj_mtp_drop_key(mj_mtp_t *m, int dc);

/* Queues a request: serialised now (q may go away), sent from poll(). The
 * callback runs from poll() too, exactly once. false if q cannot be
 * written. */
bool mj_mtp_call(mj_mtp_t *m, int dc, const tl_obj_t *q, mj_rpc_cb_t cb, void *ctx);
/* The same with the body already in TL (a file part, written straight). */
bool mj_mtp_call_raw(mj_mtp_t *m, int dc, const uint8_t *body, size_t n, mj_rpc_cb_t cb, void *ctx);
/* Requests still without an answer on a data centre (0: idle). */
int  mj_mtp_pending(mj_mtp_t *m, int dc);
/* Cancels every request with this ctx: their callbacks run with -1. */
void mj_mtp_cancel(mj_mtp_t *m, void *ctx);

/* One turn: connects what has requests, makes keys, sends, reads for up to
 * wait_ms, delivers answers and updates, pings, closes idle connections. */
void mj_mtp_poll(mj_mtp_t *m, int wait_ms);
/* Keeps the home data centre's connection up even with nothing to ask
 * (for the updates). */
void mj_mtp_set_home(mj_mtp_t *m, int dc);
int  mj_mtp_state(mj_mtp_t *m, int dc);
/* The server's clock (unix time), as learnt. */
int64_t mj_mtp_server_time(mj_mtp_t *m);
/* Closes every connection (they reopen on the next request). */
void mj_mtp_disconnect(mj_mtp_t *m);
/* The number in an error like "FLOOD_WAIT_7" or "PHONE_MIGRATE_2": -1 none. */
int  mj_err_num(const char *msg, const char *prefix);
