/*
 * P4OS - The Tienda's service (docs/plan/TIENDA.md, phase 2).
 *
 * It reads a catalogue published as a file of a release, and installs,
 * updates and removes the apps it lists on the card: the package, the art
 * packs, the receipt. It has no screen: the Tienda app and the portal ask it
 * for things and read its state. One job at a time, in a thread of its own;
 * every call here returns at once.
 *
 * What it checks and refuses is what tools/store_cli.py does, step by step
 * (that file's header is the list); tests/store_fw_host runs the two against
 * each other.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The official catalogue: the latest PUBLISHED release's, so a draft reaches
 * nobody. Another address is a preference, "store_url" (a developer's, until
 * phase 4 brings sources). */
#define AOS_STORE_URL_DEFAULT \
    "https://github.com/charliejgallo/P4OS/releases/latest/download/p4os-store.ndjson"

typedef enum {
    AOS_STORE_ABSENT = 0,   /* in the catalogue, not on the card */
    AOS_STORE_INSTALLED,    /* the catalogue's package, by its receipt */
    AOS_STORE_UPDATE,       /* a receipt of another package: the catalogue's is newer */
    AOS_STORE_BY_HAND,      /* its .so is on the card with no receipt */
} aos_store_state_t;

typedef struct {
    char     file[32];      /* the package: its .so's name without ".so" */
    char     id[40];        /* the app's */
    char     ver[28];       /* the catalogue's version */
    char     have[28];      /* the installed one, "" when none */
    char     fw_min[16];
    aos_store_state_t state;
    bool     fits;          /* this firmware can run it (ABI and fw_min) */
    uint32_t line_off;      /* its line in aos_store_catalog()'s text */
    uint32_t line_len;
} aos_store_app_t;

typedef struct {
    bool     busy;
    char     job[12];       /* "refresh", "install", "remove", "adopt", or "" */
    char     file[32];      /* the package it is about */
    char     step[40];      /* "downloading", "checking", "packs", "installing"... */
    uint32_t got, total;    /* of the download in hand; total 0 until known */
    int      queued;        /* jobs waiting behind it */
    uint32_t jobs_done;     /* counts up: something ended since the last look */
    bool     last_ok;
    char     last[160];     /* how the last job ended, one line for a person */
    char     release[28];   /* the catalogue's */
    char     source[40];
    uint32_t apps;          /* in the catalogue */
} aos_store_status_t;

/* Reads the catalogue kept on the card from the last refresh, and the
 * receipts. Called by itself by anything below; safe to call again. */
void aos_store_init(void);

/* The catalogue's address: the preference, or the default. */
const char *aos_store_url(void);
/* Another address (https://, or http:// for a developer's own machine), or
 * NULL/"" to go back to the official one. Kept across restarts. */
bool aos_store_set_url(const char *url);

/* Each queues a job and returns: false when the queue is full or the
 * arguments are no good. 'packs' is "need" (what the app cannot start
 * without; NULL means this), "all", "none", or the extras' file names
 * separated by commas. An extra installed before stays through an update. */
bool aos_store_refresh(void);
bool aos_store_install(const char *file, const char *packs);
bool aos_store_remove(const char *file);
/* A receipt for every app copied by hand whose .so is the catalogue's. */
bool aos_store_adopt(void);

/* Stops the job in hand at its next step and drops the queue. What was
 * downloaded stays for the next try. */
void aos_store_cancel(void);

void aos_store_status(aos_store_status_t *out);
int  aos_store_count(void);
bool aos_store_app(int index, aos_store_app_t *out);
/* A copy of the catalogue's text (NDJSON: the source's line, then an app a
 * line), NUL-terminated, to free(); NULL when there is none yet. */
char *aos_store_catalog(size_t *len);

#ifdef __cplusplus
}
#endif
