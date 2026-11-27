/*
 * P4OS - Mensajes: what the app knows, shared by the network thread and the
 * screen.
 *
 * The network thread (mj_api.c) writes, the screen reads; both under
 * mj_store_lock(). The screen never keeps a pointer across an unlock: it
 * copies what it draws, and it redraws a part when that part's version
 * number moved (dialogs_ver, chat_ver, auth_ver...).
 *
 *   peers     users, groups and channels met in any answer, with what it
 *             takes to name them (access_hash) and their photo
 *   dialogs   the chat list, in the server's order (pinned first)
 *   chat      the open conversation, oldest message first
 *   search    the last search's results
 *   media     files that finished downloading since the screen last asked
 *
 * Everything lives in PSRAM (mj_malloc). The texts are as the server sent
 * them, UTF-8; the screen passes them through aos_text_safe() to draw.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mj_tl.h"

#define MJ_NAME 64

enum { MJ_PEER_NONE = 0, MJ_PEER_USER = 1, MJ_PEER_CHAT = 2, MJ_PEER_CHANNEL = 3 };

typedef struct {
    uint8_t type;
    int64_t id;
} mj_pkey_t;

static inline bool mj_pkey_eq(mj_pkey_t a, mj_pkey_t b) { return a.type == b.type && a.id == b.id; }

typedef struct {
    mj_pkey_t key;
    int64_t access_hash;
    char name[MJ_NAME];         /* first and last name, or the title        */
    char first[32];
    char username[36];
    char phone[20];
    int64_t photo_id;           /* 0: none                                    */
    int photo_dc;
    bool self, bot, broadcast, megagroup, deleted, have_hash;
    int32_t online_until;       /* unix time; -1 online now; 0 unknown        */
    int32_t last_seen;
} mj_peer_t;

enum {
    MJ_MEDIA_NONE = 0,
    MJ_MEDIA_PHOTO,
    MJ_MEDIA_VOICE,
    MJ_MEDIA_AUDIO,
    MJ_MEDIA_VIDEO,
    MJ_MEDIA_ROUND,
    MJ_MEDIA_GIF,
    MJ_MEDIA_STICKER,
    MJ_MEDIA_DOC,
    MJ_MEDIA_GEO,
    MJ_MEDIA_CONTACT,
    MJ_MEDIA_POLL,
    MJ_MEDIA_WEB,
    MJ_MEDIA_OTHER,
};

/* A photo or a document of a message: enough to ask for its bytes. */
typedef struct {
    uint8_t kind;               /* MJ_MEDIA_                                   */
    int64_t id, access_hash;
    uint8_t file_ref[64];
    uint8_t file_ref_len;
    int dc;
    int64_t size;
    int w, h, duration;
    char thumb[4];              /* the size to download: "x", "m" or "" (the document itself) */
    char thumb_small[4];        /* a document's own thumbnail ("m"), "" none  */
    int thumb_w, thumb_h;
    char name[80];
    char mime[40];
    char emoji[16];             /* a sticker's                                 */
    char title[64];             /* a web page's, a poll's question, a place's  */
} mj_media_t;

typedef struct {
    int32_t id;
    int32_t date, edit_date;
    mj_pkey_t chat;
    mj_pkey_t from;
    bool out, pinned, service, mentioned, pending, failed;
    int32_t reply_to;
    int64_t random_id;          /* our own while it is pending                */
    int64_t grouped_id;
    char *text;                 /* UTF-8, never NULL                           */
    char fwd_from[MJ_NAME];
    mj_media_t *media;          /* NULL: text only                            */
    char reactions[96];         /* "👍 3   ❤️ 1", ready to draw                */
    char my_reaction[16];
    int views;
    uint8_t action;             /* a service message's MJ_ACT_                 */
    char action_arg[MJ_NAME];   /* its title, or who it names                  */
} mj_msg_t;

/* Service messages, in words only on the screen (the network thread does
 * not translate: _() follows the app in front). */
enum {
    MJ_ACT_NONE = 0, MJ_ACT_CREATE, MJ_ACT_TITLE, MJ_ACT_PHOTO, MJ_ACT_ADD, MJ_ACT_LEAVE,
    MJ_ACT_JOIN, MJ_ACT_PIN, MJ_ACT_CALL, MJ_ACT_OTHER,
};

typedef struct {
    mj_pkey_t key;
    int32_t top_id, date;
    int unread, unread_mentions;
    int32_t read_in_max, read_out_max;
    bool pinned, muted;
    char preview[120];          /* the last message's text, one line          */
    uint8_t preview_kind;       /* and its MJ_MEDIA_, MJ_ACT_ (the screen words them) */
    uint8_t preview_action;
    char who[40];               /* who wrote it, in a group: "Ana"            */
    bool preview_out;
    int32_t pts;                /* a channel's own                            */
} mj_dialog_t;

enum {
    MJ_AUTH_STARTING = 0,
    MJ_AUTH_NEED_API,           /* no api_id/api_hash yet                     */
    MJ_AUTH_PHONE,              /* ask for the number                          */
    MJ_AUTH_BUSY,               /* waiting for the server                     */
    MJ_AUTH_CODE,               /* the code was sent                           */
    MJ_AUTH_PASSWORD,           /* the two-step password                       */
    MJ_AUTH_SIGNUP,             /* the number has no account: not handled     */
    MJ_AUTH_READY,
};

#define MJ_MEDIA_DONE_MAX 32

typedef struct {
    /* signing in */
    int auth;
    char auth_err[96];          /* the last error to show, "" none            */
    char code_how[24];          /* "app", "sms", "call", "email"...           */
    int code_len;
    char pw_hint[64];
    char phone[24];
    uint32_t auth_ver;

    /* the network */
    int net;                    /* MJ_NET_ of the home data centre            */
    int home_dc;
    uint32_t net_ver;

    /* me */
    mj_pkey_t self;

    /* everything met */
    mj_peer_t *peers;
    int npeers, peers_cap;
    uint32_t peers_ver;

    /* the chat list */
    mj_dialog_t *dialogs;
    int ndialogs, dialogs_cap;
    bool dialogs_loading, dialogs_end;
    uint32_t dialogs_ver;
    int unread_total;

    /* the open chat */
    mj_pkey_t chat;
    mj_msg_t **msgs;            /* oldest first                                */
    int nmsgs, msgs_cap;
    bool chat_loading, chat_start;   /* start: nothing older                  */
    uint32_t chat_ver;
    char typing[MJ_NAME];       /* "Ana" while she types                      */
    uint32_t typing_until_ms;

    /* search */
    mj_msg_t **found;
    int nfound;
    mj_pkey_t *found_peers;
    int nfound_peers;
    bool searching;
    uint32_t search_ver;

    /* files downloaded */
    char media_done[MJ_MEDIA_DONE_MAX][96];
    int nmedia_done;
    uint32_t media_ver;
    char last_saved[160];       /* the last document saved to the card        */
    int download_pct;           /* the big one in progress, -1 none           */

    /* a word for the screen, as a code the screen words (MJ_T_) */
    int toast;
    char toast_arg[64];
    uint32_t toast_ver;
} mj_store_t;

extern mj_store_t mj_S;

void mj_store_init(void);
void mj_store_free(void);
void mj_store_lock(void);
void mj_store_unlock(void);

/* ---- for the network thread (store locked) ---- */
mj_peer_t *mj_store_peer(mj_pkey_t k);                  /* NULL: never met    */
mj_peer_t *mj_store_put_user(const tl_obj_t *u);
mj_peer_t *mj_store_put_chat(const tl_obj_t *c);
void       mj_store_put_users(const tl_vec_t *v);
void       mj_store_put_chats(const tl_vec_t *v);
mj_pkey_t  mj_peer_of(const tl_obj_t *peer);            /* peerUser/Chat/Channel */

mj_msg_t  *mj_msg_from_tl(const tl_obj_t *m);           /* NULL for messageEmpty */
void       mj_msg_free(mj_msg_t *m);
void       mj_msg_set_reactions(mj_msg_t *m, const tl_obj_t *reactions);
/* A message as one line without words of ours: the text, or an emoji
 * for what it carries ("📷 the caption"). For the phone's notifications,
 * which are made while the app is hidden. */
void       mj_msg_line(const mj_msg_t *m, char *out, size_t n);

mj_dialog_t *mj_store_dialog(mj_pkey_t k);
mj_dialog_t *mj_store_dialog_add(mj_pkey_t k);
void         mj_store_dialog_top(mj_dialog_t *d, const mj_msg_t *m);
void         mj_store_dialog_raise(mj_pkey_t k);       /* to the top after the pinned */
void         mj_store_unread_count(void);

/* the open chat: insert keeps the order and replaces one with the same id */
void       mj_chat_clear(void);
void       mj_chat_insert(mj_msg_t *m);
mj_msg_t  *mj_chat_find(int32_t id);
mj_msg_t  *mj_chat_find_random(int64_t random_id);
void       mj_chat_remove(int32_t id);

enum {
    MJ_T_NONE = 0, MJ_T_SEND_FAILED, MJ_T_EDIT_FAILED, MJ_T_DELETE_FAILED, MJ_T_FORWARDED, MJ_T_FORWARD_FAILED,
    MJ_T_SAVED, MJ_T_DOWNLOAD_FAILED, MJ_T_PHOTO_FAILED, MJ_T_PHOTO_TOO_BIG, MJ_T_FLOOD, MJ_T_LOGGED_OUT,
    MJ_T_REACT_FAILED, MJ_T_ERROR,
};
void       mj_store_toast(int code, const char *arg);
void       mj_store_media_done(const char *path);

/* the name to show for a peer ("Ana Pérez"; "?" never met) */
const char *mj_peer_name(mj_pkey_t k);
