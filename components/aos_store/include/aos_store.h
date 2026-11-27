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
 * nobody. In developer mode another address can take its place
 * (aos_store_set_url): a test server, a pre-release. */
#define AOS_STORE_URL_DEFAULT \
    "https://github.com/charliejgallo/P4OS/releases/latest/download/p4os-store.ndjson"

/* The official source and the ones the person added. */
#define AOS_STORE_SOURCES_MAX 6

typedef enum {
    AOS_STORE_ABSENT = 0,   /* in the catalogue, not on the card */
    AOS_STORE_INSTALLED,    /* the catalogue's package, by its receipt */
    AOS_STORE_UPDATE,       /* a receipt of another package: the catalogue's is newer */
    AOS_STORE_BY_HAND,      /* its .so is on the card with no receipt */
    AOS_STORE_SET_ASIDE,    /* it made the board fall while loading: its .so is now
                             * <file>.so.off (aos_dynapp.c, "Quarantine") and
                             * installing it again is the way back */
} aos_store_state_t;

/* Where what is on the card came from, as far as the store can tell. */
typedef enum {
    AOS_STORE_LISTED = 0,   /* a source lists it: 'src' says which */
    AOS_STORE_LOOSE,        /* it has a receipt and no source lists it: a package
                             * handed in (.p4app), or an app of a source taken off */
    AOS_STORE_HAND,         /* a .so on the card that nobody lists and the store
                             * did not install: copied by hand */
} aos_store_origin_t;

typedef struct {
    char     file[32];      /* the package: its .so's name without ".so" */
    char     id[40];        /* the app's */
    char     ver[28];       /* the catalogue's version */
    char     have[28];      /* the installed one, "" when none */
    char     fw_min[16];
    char     cat[12];       /* its category in the catalogue ("juegos", "red"...), "" when it has none */
    aos_store_state_t state;
    bool     fits;          /* this firmware can run it (ABI and fw_min) */
    int      src;           /* which source lists it: 0 is the official one, -1 when none does */
    aos_store_origin_t origin;
    bool     fresh;         /* new in its source: first listed in the newest release that
                             * brought apps (the catalogue's "news" and the app's "first") */
    bool     system;        /* a part of the system and not an app: files that go where
                             * the system reads them (aos_store.c, "System parts") */
    uint32_t line_off;      /* its line in aos_store_catalog()'s text */
    uint32_t line_len;
} aos_store_app_t;

/* How a job failed, for the screen to say in the person's language; the
 * text beside it (aos_store_status_t.last) is a line for the log. */
typedef enum {
    AOS_STORE_E_NONE = 0,
    AOS_STORE_E_OTHER,
    AOS_STORE_E_NETWORK,    /* could not download: no network, cut, a server's refusal */
    AOS_STORE_E_NOT_THERE,  /* the server has no such file (404) */
    AOS_STORE_E_CLOCK,      /* the board has no time yet: a certificate cannot be checked */
    AOS_STORE_E_SERVER,     /* the server's certificate does not verify */
    AOS_STORE_E_CARD,       /* could not write the card (full, gone) */
    AOS_STORE_E_PACKAGE,    /* not a package, damaged, or not the one published */
    AOS_STORE_E_RULES,      /* it would write where a package may not, or over another's */
    AOS_STORE_E_FIRMWARE,   /* it needs a newer P4OS */
    AOS_STORE_E_CATALOG,    /* the catalogue does not read, or is of a schema to come */
    AOS_STORE_E_SOURCE,     /* that source cannot be used (http, there already, too many) */
    AOS_STORE_E_CANCELLED,
    AOS_STORE_E_MEMORY,
    AOS_STORE_E_NOT_LISTED, /* no catalogue lists it */
    AOS_STORE_E_IN_STORE,   /* a package handed in that is the store's own app */
    AOS_STORE_E_NOT_STORE,  /* not installed by the store: nothing to remove */
    AOS_STORE_E_FLASH,      /* the firmware could not be written to the other slot */
    AOS_STORE_E_UP_TO_DATE, /* the firmware offered is not newer than the one running */
} aos_store_err_t;

typedef struct {
    bool     busy;
    char     job[12];       /* "refresh", "install", "remove", "adopt", "replace", "firmware"..., or "" */
    char     file[32];      /* the package it is about */
    char     step[40];      /* "downloading", "checking", "packs", "installing"... */
    uint32_t got, total;    /* of the download in hand; total 0 until known */
    int      queued;        /* jobs waiting behind it */
    uint32_t jobs_done;     /* counts up: something ended since the last look */
    bool     last_ok;
    char     last[160];     /* how the last job ended: a line for the log, in English */
    int      last_kind;     /* aos_store_err_t: how it failed, for the screen */
    char     last_job[12];  /* which job that was, and about which package */
    char     last_file[32];
    char     release[28];   /* the catalogue's */
    char     source[40];
    uint32_t apps;          /* in the catalogue */
} aos_store_status_t;

/* Reads the catalogue kept on the card from the last refresh, and the
 * receipts. Called by itself by anything below; safe to call again. */
void aos_store_init(void);

/* The first source's address: the official one, or a developer's. */
const char *aos_store_url(void);
/* Another address in the official one's place, in developer mode only; NULL
 * or "" goes back to the official one. Kept across restarts. */
bool aos_store_set_url(const char *url);

/* Developer mode: http:// addresses (on a trusted network only) and another
 * first source. Off unless someone turns it on AT THE BOARD: the portal has
 * no call for it. Turning it off goes back to the official catalogue; the
 * test one that was set is remembered and comes back with the mode. */
bool aos_store_dev(void);
void aos_store_set_dev(bool on);

/* --------------------------------------------------------------------------
 * Sources (docs/plan/TIENDA.md, "Fuentes de terceros")
 *
 * A source is the address of a catalogue. The official one is always the
 * first; the person adds others, AT THE BOARD ONLY - these calls are for
 * the Tienda app and are not in the portal - after reading who the source
 * says it is and what adding it means: an app runs with every permission
 * of the system, and nobody at P4OS looked at these.
 *
 * What a source may not do: list an app whose file or id another source
 * before it already lists (the official one is first, so it cannot be
 * replaced); such an app is left out, and counted in 'skipped'.
 *
 * The list is kept in the board's preferences, not on the card: a card from
 * somewhere else brings no sources with it.
 * -------------------------------------------------------------------------- */
typedef struct {
    char url[200];          /* its catalogue's address */
    char name[40];          /* who it says it is; "" until its catalogue was read */
    char home[160];         /* its own page */
    char base[200];         /* where its files are: a "url" of its catalogue goes after this */
    char release[28];
    int  apps;              /* offered from it */
    int  skipped;           /* left out: another source lists them */
    bool loaded;
} aos_store_source_t;

int  aos_store_sources(void);
bool aos_store_source(int index, aos_store_source_t *out);

/* What a person typed as an address: "user/repo" (or github.com/user/repo)
 * is that repository's latest release; an https:// address is itself. */
bool aos_store_source_url(const char *text, char *out, size_t n);

/* A look at a source before adding it: a job, like the rest. When it ends
 * well aos_store_probe_result() has who the source says it is - its
 * "source" object as JSON: name, by, url, about - and how many apps it
 * lists. */
typedef struct {
    bool ok;
    char url[200];
    char source_json[900];
    int  apps;
} aos_store_probe_t;

bool aos_store_probe(const char *url);
void aos_store_probe_result(aos_store_probe_t *out);
/* Adds the source just looked at, and only that one. */
bool aos_store_source_add(const char *url);
/* Takes a source off (never the first); with_apps removes what was
 * installed from it, else those apps stay and are no longer updated. */
bool aos_store_source_remove(int index, bool with_apps);

/* The portal cannot add a source, but it can ask: the address is kept (one
 * at a time, until the next restart) for the Tienda to show at the board,
 * where a person looks at it, reads the warning and decides. false when it
 * is not an address a source may have. */
bool aos_store_propose(const char *text);
bool aos_store_proposal(char *out, size_t n);
void aos_store_proposal_clear(void);

/* --------------------------------------------------------------------------
 * The system's own firmware (docs/plan/TIENDA.md, "El firmware desde GitHub")
 *
 * The OFFICIAL source's catalogue - the first one, and no other - may say
 * which firmware its release brings, an image for each kind of board. When
 * it is newer than the one running, a person can ask for it: it is
 * downloaded to the card, checked whole against the size and the SHA-256
 * the catalogue gives BEFORE anything is written, and then written to the
 * other slot (aos_hal_ota_*). Nothing here restarts the board: whoever
 * asked does, and the new image boots on trial like any other
 * (aos_hal_ota_pending_verify). Never by itself: there is no call that
 * installs without being asked.
 * -------------------------------------------------------------------------- */
typedef struct {
    char     ver[28];       /* the release the catalogue offers for this board */
    char     profile[12];   /* which kind of board this firmware is for: "rev1_3", "rev3_x" */
    uint32_t size;
    bool     newer;         /* than the one running */
    bool     ready;         /* written to the other slot: it runs after a restart */
} aos_store_fw_t;

/* false, with 'out' cleared but for 'profile', when the catalogue offers no
 * firmware for this board. */
bool aos_store_fw(aos_store_fw_t *out);
/* Queues the download and the writing; aos_store_status() says how far it
 * is ("firmware": "downloading", then "writing"). */
bool aos_store_fw_install(void);

/* A package (.p4app) that is on the card or on a pendrive, by its path from
 * the card's root: installed by the same steps as one of a catalogue. It
 * may not be an app that a source offers or installed. */
bool aos_store_sideload(const char *card_path);

/* Each queues a job and returns: false when the queue is full or the
 * arguments are no good. 'packs' is "need" (what the app cannot start
 * without; NULL means this), "all", "none", or the extras' file names
 * separated by commas. An extra installed before stays through an update. */
bool aos_store_refresh(void);
bool aos_store_install(const char *file, const char *packs);
bool aos_store_remove(const char *file);
/* A receipt for every app copied by hand whose .so is the catalogue's. */
bool aos_store_adopt(void);
/* The ones that are not: every app copied by hand that a source lists is
 * installed over from that source, one after another, in one job. What they
 * saved and the packs the card already has stay. */
bool aos_store_replace_by_hand(void);

/* Stops the job in hand at its next step and drops the queue. What was
 * downloaded stays for the next try. */
void aos_store_cancel(void);

void aos_store_status(aos_store_status_t *out);
int  aos_store_count(void);
bool aos_store_app(int index, aos_store_app_t *out);
/* A picture of an app's page, by its whole address (its source's "base" and
 * the "url" of one of its "shots"). They are kept on the card (apps/.store/shots) once
 * downloaded, two at a time at most, apart from the queue of jobs.
 *    1   it is on the card: 'path' has its full path, to decode
 *    0   on its way; ask again
 *   -1   it could not be had (no network, not there): not tried again until
 *        the next refresh of the catalogue */
int aos_store_shot(const char *url, char *path, size_t path_len);

/* A copy of the catalogue's text, NUL-terminated, to free(); NULL when there
 * is none yet. NDJSON: for each source in turn, its own line (the one with
 * "source") and then a line for each app offered from it. After them, a
 * line made here for each app on the card that no source lists, so that the
 * list of what is installed is whole: "local" says which kind it is - "pkg"
 * for one with a receipt (and "from", its source's name, when that source
 * was taken off), "hand" for a .so the store knows nothing else about. */
char *aos_store_catalog(size_t *len);

#ifdef __cplusplus
}
#endif
