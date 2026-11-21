/*
 * P4OS - A file from the network onto the card (docs/plan/TIENDA.md).
 *
 * aos_hal_http_* keeps a whole answer in memory, which is right for a JSON
 * and wrong for a 37 MB art pack. This is the other half: one GET that
 * follows redirects from host to host, writes what arrives to a file, picks
 * up where it left off after a cut, and says at the end whether what is on
 * the card is what was published. It is what the Tienda downloads with.
 *
 * Internal to the firmware for now (the store's service and the portal use
 * it); the same file on the board and in the simulator, on aos_tls.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AOS_FETCH_NONE = 0,     /* no such download */
    AOS_FETCH_BUSY,
    AOS_FETCH_DONE,         /* the file is at its path, whole and checked */
    AOS_FETCH_FAILED,       /* 'status' and 'why' say how; what arrived stays in <path>.part */
    AOS_FETCH_CANCELLED,    /* the same: a later start with the same path goes on from it */
} aos_fetch_state_t;

/* 'status' when it failed for a reason of its own. The network's reasons are
 * aos_hal.h's AOS_HTTP_ERR_*, and a server's refusal is its HTTP code. */
#define AOS_FETCH_ERR_FILE      (-20)   /* the card: could not create, write or rename */
#define AOS_FETCH_ERR_HASH      (-21)   /* the file is not the one the hash names: deleted */
#define AOS_FETCH_ERR_SIZE      (-22)   /* nor the size: deleted */
#define AOS_FETCH_ERR_REDIRECT  (-23)   /* too many, to nowhere, or from https down to http */
#define AOS_FETCH_ERR_GAVE_UP   (-24)   /* several tries in a row without a byte more */

typedef struct {
    aos_fetch_state_t state;
    int      status;        /* the last HTTP code, or a negative reason */
    uint32_t got;           /* bytes on the card, what an earlier try left included */
    uint32_t total;         /* 0 until the server says */
    uint32_t resumed;       /* how many of them were there before this start */
    uint16_t connects;      /* connections opened, redirects included */
    uint16_t redirects;     /* of the last try */
    uint16_t retries;       /* tries after the first */
    uint32_t ms;            /* since the start; stops when it ends */
    uint32_t connect_ms;    /* of them, spent connecting and shaking hands */
    uint32_t verify_ms;     /* and reading the file back for its hash */
    char     host[96];      /* who sends the bytes: the last host of the chain */
    char     why[96];       /* one line for a person, in English: it goes to the log */
} aos_fetch_info_t;

/* Starts a download and returns at once: an id > 0, or < 0 when the address
 * is not http(s), the path is too long, both slots are taken or there is no
 * memory. 'path' is a full path of the file system ("/sdcard/apps/x.so");
 * the bytes go to <path>.part and take the name only when they are all there
 * and checked, so the file at 'path' is never half of anything.
 *
 * sha256_hex (64 hex digits) and size are what the file must be; NULL and 0
 * for "whatever comes". With a hash the file is read back from the card once
 * it is whole, so what is checked is what was written.
 *
 * A <path>.part left by an earlier try is continued with a Range request; a
 * server that does not do ranges sends the whole file and that is taken. */
int  aos_fetch_start(const char *url, const char *path, const char *sha256_hex, uint32_t size);

/* False when the id is not a download. Any thread. */
bool aos_fetch_info(int id, aos_fetch_info_t *out);

/* Asks it to stop; it does within a second or so, as CANCELLED. */
void aos_fetch_cancel(int id);

/* Gives the slot back. Always call it; on a download still going it cancels
 * it too. */
void aos_fetch_release(int id);
