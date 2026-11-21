/*
 * P4OS - Backups (docs/plan/RESPALDO.md, stage 1).
 *
 * What a board holds and cannot fetch again, in one file on the card
 * (/backups/<name>.p4bak): its preferences - the system's and the apps', the
 * networks, the accounts -, the list of the apps the Tienda installed with
 * their extras, and the small files of the card that say how it is arranged
 * (menu.txt, modules.txt). The apps themselves are not copied: the Tienda
 * brings them back from that list.
 *
 * Two ways to make one:
 *
 *  - WITHOUT a passphrase the file is plain, and leaves out whatever looks
 *    like a credential (a password, a token, a key, a code). The filter is
 *    by the preference's name, so it is a best effort and the screens say
 *    so.
 *  - WITH a passphrase everything goes in, sealed: AES-256-GCM under a key
 *    derived from the passphrase (PBKDF2-HMAC-SHA256). Nothing of a sealed
 *    backup can be read without it, and there is no way around a forgotten
 *    one.
 *
 * What belongs to this very board and to this very moment is in neither:
 * the counters of failed boots, the crash dump's marks, the pairing flag of
 * the Bluetooth (its keys are not preferences), the portal's sessions.
 *
 * Restoring writes the preferences of the file over the ones there are and
 * leaves the rest alone; before it does, it makes a backup of what there is
 * (the same kind, under the same passphrase), so a restore can be undone.
 * A backup made by a newer P4OS than the one running is refused. Most
 * preferences are read at startup: the caller offers the restart.
 *
 * The work is done in a thread of this module: the calls that start it
 * return at once and aos_backup_status() says how it went.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_BACKUP_DIR      "backups"       /* on the card */
#define AOS_BACKUP_EXT      ".p4bak"
#define AOS_BACKUP_NAME_MAX 64

typedef struct {
    char     name[AOS_BACKUP_NAME_MAX];     /* the file's, without its folder */
    uint32_t made;          /* epoch seconds; 0 when the board had no time */
    int      seq;           /* which one of that second: the list's order among equals */
    char     fw[28];        /* the P4OS that made it */
    char     board[40];     /* the board's name then */
    bool     sealed;        /* it asks for its passphrase */
    bool     before;        /* made by itself, before a restore */
    int      prefs;         /* preferences in it */
    int      left_out;      /* credentials it does not carry (a plain one) */
    int      apps;          /* apps in its list */
    uint32_t size;
} aos_backup_info_t;

/* How a job failed, for the screen to say in the person's language. */
typedef enum {
    AOS_BACKUP_E_NONE = 0,
    AOS_BACKUP_E_OTHER,
    AOS_BACKUP_E_CARD,          /* no card, or it could not be written */
    AOS_BACKUP_E_NOT_FOUND,     /* no such backup */
    AOS_BACKUP_E_NOT_ONE,       /* the file is not a backup, or is damaged */
    AOS_BACKUP_E_PASSPHRASE,    /* sealed, and that is not its passphrase */
    AOS_BACKUP_E_NEWER,         /* made by a newer P4OS than this one */
    AOS_BACKUP_E_MEMORY,
    AOS_BACKUP_E_BUSY,
} aos_backup_err_t;

typedef struct {
    bool     busy;
    char     job[10];           /* "make" or "restore" */
    uint32_t jobs_done;         /* counts up */
    bool     last_ok;
    int      last_kind;         /* aos_backup_err_t */
    char     last_job[10];
    char     last[120];         /* a line for the log, in English */
    char     name[AOS_BACKUP_NAME_MAX];     /* the file the last job made or read */
    int      written;           /* preferences a restore wrote */
    bool     restart;           /* a restore went in: the restart is pending */
} aos_backup_status_t;

/* Makes a backup now, in the background. 'passphrase' NULL or "" is the
 * plain kind. false when one is already being made or restored. */
bool aos_backup_make(const char *passphrase);

/* Restores one, by its file's name, in the background. */
bool aos_backup_restore(const char *name, const char *passphrase);

void aos_backup_status(aos_backup_status_t *out);

/* The backups on the card, the newest first: how many, at most 'max'. */
int  aos_backup_list(aos_backup_info_t *out, int max);
bool aos_backup_info(const char *name, aos_backup_info_t *out);
bool aos_backup_delete(const char *name);

/* The apps a backup lists that are not on the card now: for each, its
 * package and the extras it had ("a.pak,b.pak", or ""), to hand to
 * aos_store_install(). How many, at most 'max'. */
typedef struct {
    char file[32];
    char packs[200];
} aos_backup_app_t;
int  aos_backup_missing_apps(const char *name, aos_backup_app_t *out, int max);

/* The same, without the thread: for the tests and for whoever is already in
 * one. 'why' gets a line for the log. They return an aos_backup_err_t. */
int aos_backup_make_now(const char *passphrase, bool before, char *name_out, size_t name_len, char *why, size_t why_len);
int aos_backup_restore_now(const char *name, const char *passphrase, int *written, char *why, size_t why_len);

#ifdef __cplusplus
}
#endif
