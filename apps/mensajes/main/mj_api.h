/*
 * P4OS - Mensajes: the Telegram client, in the app's network thread.
 *
 * Everything that talks to the server is here, on top of mj_mtp: signing
 * in (the number, the code, the two-step password with SRP), the chat
 * list, the history of the open chat, sending, editing, deleting,
 * forwarding, reactions, marking read, searching, the files (photos,
 * avatars, documents, voice notes) and the updates that arrive by
 * themselves. What it learns goes into the store (mj_store.h); the screen
 * asks for things with the calls below, from any thread: each one is a job
 * queued for the network thread, which does them in order.
 *
 * Kept between runs:
 *   preferences  msj_apiid, sec_msj_hash (the api_hash), msj_dc (the home
 *                data centre), sec_msj_ak1..5 (the auth keys), msj_dcok
 *                (the data centres where we are signed in), msj_self. The
 *                ones with secrets start with sec_: a backup without a
 *                passphrase leaves them out (docs/BACKUP.md).
 *   files        <data>/mensajes/cache (photos and avatars, up to a ceiling,
 *                the oldest go first), <card>/Mensajes (documents and voice
 *                notes downloaded), <data>/mensajes/servidor.txt (a server
 *                of one's own, for the tests).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mj_store.h"

typedef struct {
    char data_dir[160];         /* <data>/mensajes                            */
    char dl_dir[160];           /* where documents go: <card>/Mensajes        */
    char lang[8];               /* the person's language, for initConnection  */
    char device[40], system[24], version[16];
    size_t cache_max;           /* bytes of cache before the oldest files go  */
    /* a message that arrived (the network thread, store locked): for the
     * phone-style notification and the badge */
    void (*on_incoming)(const mj_msg_t *m);
} mj_api_cfg_t;

bool mj_api_start(const mj_api_cfg_t *cfg);
/* Asks the thread to stop and waits for it (up to wait_ms). */
void mj_api_stop(uint32_t wait_ms);
bool mj_api_running(void);

/* ---- signing in ---- */
void mj_api_set_app(int32_t api_id, const char *api_hash);
bool mj_api_have_app(void);
void mj_api_login_phone(const char *phone);
void mj_api_login_code(const char *code);
void mj_api_login_password(const char *password);
void mj_api_login_restart(void);           /* back to the number             */
void mj_api_logout(void);

/* ---- chats ---- */
void mj_api_more_dialogs(void);
void mj_api_reload_dialogs(void);
void mj_api_open_chat(mj_pkey_t k);
void mj_api_close_chat(void);
void mj_api_more_history(void);
void mj_api_mark_read(void);

/* ---- messages ---- */
void mj_api_send_text(mj_pkey_t k, const char *text, int32_t reply_to);
void mj_api_send_photo(mj_pkey_t k, const char *path, const char *caption, int32_t reply_to);
void mj_api_edit(mj_pkey_t k, int32_t id, const char *text);
void mj_api_delete(mj_pkey_t k, int32_t id, bool for_everyone);
void mj_api_forward(mj_pkey_t from, int32_t id, mj_pkey_t to);
void mj_api_react(mj_pkey_t k, int32_t id, const char *emoji);   /* "" takes ours away */
void mj_api_typing(mj_pkey_t k);
void mj_api_search(const char *q);

/* ---- files ---- */
enum { MJ_FETCH_AVATAR = 1, MJ_FETCH_PHOTO, MJ_FETCH_THUMB, MJ_FETCH_DOC };

/* Where a file is or will be: avatar of a peer, a photo, a document's
 * thumbnail, a document (to the card). Pure: no I/O. */
void mj_api_path(int kind, const mj_peer_t *p, const mj_media_t *md, int32_t msg_id, char *out, size_t n);
/* Asks for it to be downloaded (once; again later if it failed). The store
 * says when (mj_S.media_done). */
void mj_api_fetch(int kind, const mj_peer_t *p, const mj_media_t *md, int32_t msg_id);
bool mj_file_exists(const char *path);

/* the counts for the settings page */
void mj_api_cache_info(uint32_t *files, uint32_t *bytes);
void mj_api_cache_clear(void);
