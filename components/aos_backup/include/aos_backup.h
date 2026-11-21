/*
 * P4OS - Backups (docs/plan/RESPALDO.md, stages 1 to 3).
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

/* Who asked for a backup. */
typedef enum {
    AOS_BACKUP_BY_HAND = 0,
    AOS_BACKUP_BEFORE_RESTORE,      /* what there was, kept before a restore wrote over it */
    AOS_BACKUP_DAILY,               /* the day's copy, made when something changed */
    AOS_BACKUP_BEFORE_FW,           /* before a firmware was installed */
} aos_backup_why_t;

typedef struct {
    char     name[AOS_BACKUP_NAME_MAX];     /* the file's, without its folder */
    uint32_t made;          /* epoch seconds; 0 when the board had no time */
    int      seq;           /* which one of that second: the list's order among equals */
    char     fw[28];        /* the P4OS that made it */
    char     board[40];     /* the board's name then */
    bool     sealed;        /* it asks for its passphrase */
    bool     before;        /* made by itself, before a restore */
    int      why;           /* aos_backup_why_t */
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
    AOS_BACKUP_E_DRIVE,         /* no pendrive, or it could not be written */
} aos_backup_err_t;

typedef struct {
    bool     busy;
    char     job[12];           /* "make", "restore", "auto", "key", "to_drive" or "from_drive" */
    uint32_t jobs_done;         /* counts up */
    bool     last_ok;
    int      last_kind;         /* aos_backup_err_t */
    char     last_job[12];
    char     last[120];         /* a line for the log, in English */
    char     name[AOS_BACKUP_NAME_MAX];     /* the file the last job made or read */
    int      written;           /* preferences a restore wrote */
    bool     restart;           /* a restore went in: the restart is pending */
    uint32_t files, kbytes;     /* a copy to or from a pendrive: how far it is, or how much it was */
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

/* ---- the copies it makes by itself (stage 2) ----
 *
 * One a day on the card, when a preference changed since the last one, and
 * one before a firmware is installed, on the same condition. The card keeps
 * the last five of them, and the last five made before a restore. They are
 * plain, without what looks like a credential, until the person sets a
 * passphrase for them at the board; from then on they carry everything,
 * sealed. The board keeps the key PBKDF2 gave and its salt, not the
 * passphrase: a restore asks for it as for any sealed backup.
 *
 * A board without a clock makes none: a day is told by its date. */
bool aos_backup_auto_on(void);                  /* on unless turned off */
void aos_backup_auto_set(bool on);
bool aos_backup_auto_sealed(void);
/* Sets the passphrase of the automatic copies, in the background (PBKDF2
 * takes seconds), and makes the first sealed one. NULL or "" takes it off. */
bool aos_backup_auto_passphrase(const char *passphrase);
uint32_t aos_backup_auto_day(void);             /* the last day looked at, as 20261121; 0 for never */

/* From the system's tick, five times a second: cheap. Starts the day's copy
 * when it is due. */
void aos_backup_tick(void);

/* Before a firmware is written: the copy, when something changed since the
 * last one, made and waited for (half a second; never more than ten). */
void aos_backup_before_firmware(void);

/* A board that looks new (it knows no network and never answered this) with
 * a backup on its card: the newest one, to offer. aos_backup_offer_done()
 * when the person answered, whatever the answer. */
bool aos_backup_offer(aos_backup_info_t *out);
void aos_backup_offer_done(void);

/* ---- to a pendrive and back (stage 3) ----
 *
 * What a backup does not carry is what the apps wrote on the card. A
 * pendrive on the USB host takes it file by file, into p4os-backup/ on the
 * drive: the backups (p4os-backup/backups) and the chosen folders of the
 * card (p4os-backup/card/<folder>). A file the other side already has, of
 * the same size and not older, is not copied again, so a second run copies
 * what changed. The way back puts the backups and every folder of the
 * drive on the card: a new card, then a restore, and the board is as it
 * was. */
#define AOS_BACKUP_FOLDERS_MAX 48
typedef struct {
    char name[40];
    bool on;                /* it goes to the pendrive */
} aos_backup_folder_t;
/* The folders at the card's root that hold data (not the ones the Tienda
 * fetches again), by name, and whether each is chosen: all but the big
 * ones that come from a computer, until a person says otherwise. */
int  aos_backup_folders(aos_backup_folder_t *out, int max);
void aos_backup_folder_set(const char *name, bool on);
bool aos_backup_drive(void);            /* a pendrive is mounted */
bool aos_backup_to_drive(void);         /* in the background; aos_backup_status() says how far */
bool aos_backup_from_drive(void);
void aos_backup_cancel(void);           /* stops the copy in hand after the file it is on */

/* The same, without the thread: for the tests and for whoever is already in
 * one. 'why' gets a line for the log. They return an aos_backup_err_t. */
int aos_backup_make_now(const char *passphrase, bool before, char *name_out, size_t name_len, char *why, size_t why_len);
int aos_backup_restore_now(const char *name, const char *passphrase, int *written, char *why, size_t why_len);
/* The automatic copy, now: 'reason' is AOS_BACKUP_DAILY or _BEFORE_FW;
 * 'made' says whether one was needed. */
int aos_backup_auto_now(int reason, bool *made, char *why, size_t why_len);
bool aos_backup_auto_passphrase_now(const char *passphrase);
int aos_backup_copy_now(bool to_drive, char *why, size_t why_len);

#ifdef __cplusplus
}
#endif
