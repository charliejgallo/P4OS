/*
 * P4OS - Mensajes: the Telegram client (mj_api.h says what it does).
 *
 * One thread, one loop: take the jobs the screen queued, turn each into
 * requests, let mj_mtp send them and read, and in the callbacks put what
 * came back into the store. Callbacks run inside mj_mtp_poll(), in this
 * same thread, so nothing here needs a lock of its own except the store's
 * (taken only while writing it: this thread is the only writer) and the
 * job queue's.
 */
#include "mj_api.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mj_crypto.h"
#include "mj_mtp.h"
#include "mj_port.h"

#define START_DC     2          /* where a client without a home asks first  */
#define CHUNK        (128 * 1024)
#define PART         (512 * 1024)
#define PHOTO_MAX    (10 * 1024 * 1024)
#define FETCH_LIVE   4          /* files downloading at once                   */
#define HISTORY_PAGE 40
#define DIALOG_PAGE  50

/* ------------------------------------------------------------------ jobs */

enum {
    J_SET_APP = 1, J_PHONE, J_CODE, J_PASSWORD, J_RESTART, J_LOGOUT, J_MORE_DIALOGS, J_RELOAD_DIALOGS,
    J_OPEN, J_CLOSE, J_MORE_HISTORY, J_MARK_READ, J_SEND, J_PHOTO, J_EDIT, J_DELETE, J_FORWARD, J_REACT,
    J_TYPING, J_SEARCH, J_FETCH, J_CACHE_CLEAR,
};

typedef struct job {
    struct job *next;
    int type;
    mj_pkey_t k, k2;
    int32_t id, id2, i;
    bool b;
    char *s, *s2;
    mj_media_t md;
    mj_peer_t peer;
} job_t;

typedef struct fetch {
    struct fetch *next;
    int kind;
    char path[176];
    int dc;
    mj_arena_t a;
    tl_obj_t *loc;
    int64_t offset, size;
    FILE *f;
    int tries;
    bool live;                  /* a request is out                           */
    bool big;                   /* shows its progress                         */
} fetch_t;

typedef struct {
    mj_pkey_t k;
    int32_t reply_to;
    int64_t random_id, file_id;
    uint8_t *data;
    size_t n;
    int part, parts;
    char *caption;
    char name[64];
} upload_t;

static struct {
    mj_api_cfg_t cfg;
    volatile bool stop, running, done;
    void *jlock;
    job_t *jhead, *jtail;
    mj_mtp_t *mtp;
    int home;
    int32_t api_id;
    char api_hash[48];
    uint32_t dcok;              /* bit n: signed in at data centre n          */
    uint32_t dc_importing;
    bool logged;
    /* signing in */
    char phone[24], code_hash[64];
    int64_t srp_id;
    uint8_t srp_B[256], salt1[64], salt2[64], srp_p[256];
    uint32_t s1n, s2n;
    int srp_g;
    bool pw_ready;
    char *pw_wait;              /* a password typed before getPassword came  */
    /* updates */
    int32_t pts, qts, date, seq;
    bool diff_busy;
    uint32_t diff_at_ms;
    /* the open chat */
    mj_pkey_t chat;
    uint32_t chat_gen;
    bool history_busy;
    /* the chat list */
    int32_t dl_offset_date, dl_offset_id;
    mj_pkey_t dl_offset_peer;
    bool dialogs_busy;
    /* files */
    fetch_t *fq;
    int fetch_n;
    uint32_t cache_check_ms;
    upload_t *up;
    int64_t pending_seq;
    uint32_t typing_ms, state_check_ms;
    char srv_host[64];
    int srv_port;
    uint8_t srv_rsa[256];
    bool have_srv;
} A;

static void push(job_t *j)
{
    if (!A.jlock) return;
    mj_mutex_lock(A.jlock);
    j->next = NULL;
    if (A.jtail) A.jtail->next = j;
    else A.jhead = j;
    A.jtail = j;
    mj_mutex_unlock(A.jlock);
}

static job_t *pop(void)
{
    mj_mutex_lock(A.jlock);
    job_t *j = A.jhead;
    if (j) {
        A.jhead = j->next;
        if (!A.jhead) A.jtail = NULL;
    }
    mj_mutex_unlock(A.jlock);
    return j;
}

static job_t *job(int type)
{
    job_t *j = (job_t *)mj_calloc(sizeof *j);
    if (j) j->type = type;
    return j;
}

static void job_free(job_t *j)
{
    mj_free(j->s);
    mj_free(j->s2);
    mj_free(j);
}

static void scpy(char *d, size_t n, const char *s)
{
    snprintf(d, n, "%s", s ? s : "");
}

/* --------------------------------------------------------- the persisted */

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[b[i] >> 4];
        out[2 * i + 1] = H[b[i] & 15];
    }
    out[2 * n] = 0;
}

static bool unhex(const char *s, uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        b[i] = (uint8_t)v;
    }
    return true;
}

static void save_key(void *ud, int dc, const uint8_t *key)
{
    (void)ud;
    char name[16];
    snprintf(name, sizeof name, "sec_msj_ak%d", dc);
    if (!key) {
        mj_pref_erase(name);
        A.dcok &= ~(1u << dc);
        mj_pref_set_i32("msj_dcok", (int32_t)A.dcok);
        return;
    }
    char *h = (char *)mj_malloc(513);
    if (!h) return;
    hex(key, 256, h);
    mj_pref_set_str(name, h);
    memset(h, 0, 513);
    mj_free(h);
}

static void load_keys(void)
{
    char *h = (char *)mj_malloc(600);
    uint8_t key[256];
    if (!h) return;
    for (int dc = 1; dc <= MJ_DC_MAX; dc++) {
        char name[16];
        snprintf(name, sizeof name, "sec_msj_ak%d", dc);
        if (mj_pref_get_str(name, h, 600) && strlen(h) == 512 && unhex(h, key, 256)) mj_mtp_set_key(A.mtp, dc, key);
    }
    memset(key, 0, sizeof key);
    memset(h, 0, 600);
    mj_free(h);
}

static void set_home(int dc)
{
    A.home = dc;
    mj_pref_set_i32("msj_dc", dc);
    mj_mtp_set_home(A.mtp, dc);
    mj_store_lock();
    mj_S.home_dc = dc;
    mj_S.net_ver++;
    mj_store_unlock();
}

static void set_auth(int st, const char *err)
{
    mj_store_lock();
    mj_S.auth = st;
    scpy(mj_S.auth_err, sizeof mj_S.auth_err, err);
    mj_S.auth_ver++;
    mj_store_unlock();
}

static void load_server(void)
{
    /* <data>/mensajes/servidor.txt: a server of one's own, for the tests
     * ("host 127.0.0.1", "port 9443", "rsa <512 hex digits>") */
    char path[200];
    snprintf(path, sizeof path, "%s/servidor.txt", A.cfg.data_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[700];
    bool rsa = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "host ", 5)) scpy(A.srv_host, sizeof A.srv_host, line + 5);
        else if (!strncmp(line, "port ", 5)) A.srv_port = atoi(line + 5);
        else if (!strncmp(line, "rsa ", 4) && strlen(line + 4) == 512) rsa = unhex(line + 4, A.srv_rsa, 256);
    }
    fclose(f);
    A.have_srv = A.srv_host[0] && A.srv_port > 0 && rsa;
    if (A.have_srv) mj_log("api: using the server at %s:%d (servidor.txt)", A.srv_host, A.srv_port);
    else mj_log("api: servidor.txt is incomplete, ignored");
}

/* --------------------------------------------------------------- helpers */

static bool call_dc(int dc, tl_obj_t *q, mj_rpc_cb_t cb, void *ctx)
{
    if (!q) return false;
    bool ok = mj_mtp_call(A.mtp, dc, q, cb, ctx);
    if (!ok) mj_log("api: could not write %s", tl_name(q->id));
    return ok;
}

static bool call(tl_obj_t *q, mj_rpc_cb_t cb, void *ctx)
{
    return call_dc(A.home, q, cb, ctx);
}

static tl_obj_t *obj(const mj_rpc_t *r)
{
    return r->code == 0 && r->val.kind == TLK_OBJ ? r->val.u.o : NULL;
}

static tl_obj_t *input_peer(mj_arena_t *a, mj_pkey_t k)
{
    if (k.type == MJ_PEER_USER && k.id == mj_S.self.id) return tl_new(a, TLC_inputPeerSelf);
    mj_peer_t *p = mj_store_peer(k);
    int64_t h = p ? p->access_hash : 0;
    tl_obj_t *o = NULL;
    switch (k.type) {
    case MJ_PEER_USER:
        o = tl_new(a, TLC_inputPeerUser);
        tl_set_l(o, TLN_user_id, k.id);
        tl_set_l(o, TLN_access_hash, h);
        break;
    case MJ_PEER_CHAT:
        o = tl_new(a, TLC_inputPeerChat);
        tl_set_l(o, TLN_chat_id, k.id);
        break;
    case MJ_PEER_CHANNEL:
        o = tl_new(a, TLC_inputPeerChannel);
        tl_set_l(o, TLN_channel_id, k.id);
        tl_set_l(o, TLN_access_hash, h);
        break;
    default:
        o = tl_new(a, TLC_inputPeerEmpty);
    }
    return o;
}

static tl_obj_t *input_channel(mj_arena_t *a, mj_pkey_t k)
{
    mj_peer_t *p = mj_store_peer(k);
    tl_obj_t *o = tl_new(a, TLC_inputChannel);
    tl_set_l(o, TLN_channel_id, k.id);
    tl_set_l(o, TLN_access_hash, p ? p->access_hash : 0);
    return o;
}

static bool is_auth_error(const char *m)
{
    return !strcmp(m, "AUTH_KEY_UNREGISTERED") || !strcmp(m, "SESSION_REVOKED") || !strcmp(m, "SESSION_EXPIRED") ||
           !strcmp(m, "AUTH_KEY_INVALID") || !strcmp(m, "USER_DEACTIVATED") || !strcmp(m, "AUTH_KEY_PERM_EMPTY") ||
           !strcmp(m, "USER_DEACTIVATED_BAN");
}

static void signed_out(bool drop_keys);

/* What every callback of a signed-in request checks first: the session was
 * closed elsewhere (back to the number), or the account lives elsewhere.
 * true: handled, the callback does nothing more. */
static bool common_errors(const mj_rpc_t *r)
{
    if (r->code == 401 && is_auth_error(r->msg) && r->dc == A.home && A.logged) {
        mj_log("api: %s: the session is closed, back to signing in", r->msg);
        signed_out(strcmp(r->msg, "AUTH_KEY_UNREGISTERED") != 0);
        return true;
    }
    if (r->code == 303) {
        int dc = mj_err_num(r->msg, "USER_MIGRATE_");
        if (dc < 0) dc = mj_err_num(r->msg, "NETWORK_MIGRATE_");
        if (dc >= 1 && dc <= MJ_DC_MAX && dc != A.home) {
            mj_log("api: %s: the home is now dc%d", r->msg, dc);
            set_home(dc);
            return true;
        }
    }
    if (r->code == 420) {
        mj_store_lock();
        mj_store_toast(MJ_T_FLOOD, r->msg);
        mj_store_unlock();
        return true;
    }
    return false;
}

/* ------------------------------------------------------ updates' state */

static void state_path(char *p, size_t n)
{
    snprintf(p, n, "%s/state.txt", A.cfg.data_dir);
}

static void state_save(void)
{
    char p[200];
    state_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d\n", (int)A.pts, (int)A.qts, (int)A.date, (int)A.seq);
    fclose(f);
}

static void set_state(const tl_obj_t *st)
{
    if (!st) return;
    A.pts = tl_i(st, TLN_pts);
    A.qts = tl_i(st, TLN_qts);
    A.date = tl_i(st, TLN_date);
    A.seq = tl_i(st, TLN_seq);
    state_save();
}

static void get_difference(void);

/* pts as core.telegram.org/api/updates says: 1 apply, 0 already had it,
 * -1 a gap (getDifference). */
static int check_pts(int32_t pts, int32_t count)
{
    if (!A.pts || !pts) {
        if (pts) A.pts = pts;
        return 1;
    }
    if (A.pts + count == pts) {
        A.pts = pts;
        return 1;
    }
    if (A.pts + count > pts) return 0;
    mj_log("api: a gap in pts (%d + %d < %d)", (int)A.pts, (int)count, (int)pts);
    if (!A.diff_at_ms) A.diff_at_ms = mj_now_ms() + 500;
    return -1;
}

/* ----------------------------------------------------------- messages */

static void store_messages(const tl_vec_t *v, bool to_chat, bool tops, uint32_t gen);

/* A message that arrived now (an update or the difference): the list, the
 * open chat, the notification. */
static void got_new(const tl_obj_t *t)
{
    mj_msg_t *m = mj_msg_from_tl(t);
    if (!m) return;
    mj_store_lock();
    bool known = mj_store_dialog(m->chat) != NULL;
    mj_dialog_t *d = mj_store_dialog_add(m->chat);
    bool is_open = mj_pkey_eq(m->chat, A.chat);
    if (d) {
        bool newer = m->id > d->top_id;
        mj_store_dialog_top(d, m);
        if (newer && !m->out && !m->service && !is_open) d->unread++;
        if (newer && !m->out && !m->service && is_open && mj_S.typing[0]) mj_S.typing[0] = 0;
        if (!known || newer) mj_store_dialog_raise(m->chat);
        mj_store_unread_count();
        mj_S.dialogs_ver++;
    }
    if (!m->out && !m->service && A.cfg.on_incoming) A.cfg.on_incoming(m);
    if (is_open) {
        /* a pending one of ours with this id is the same message */
        mj_chat_insert(m);
        m = NULL;
    }
    mj_store_unlock();
    mj_msg_free(m);
}

static void got_edit(const tl_obj_t *t)
{
    mj_msg_t *m = mj_msg_from_tl(t);
    if (!m) return;
    mj_store_lock();
    mj_dialog_t *d = mj_store_dialog(m->chat);
    if (d && d->top_id == m->id) {
        mj_store_dialog_top(d, m);
        mj_S.dialogs_ver++;
    }
    if (mj_pkey_eq(m->chat, A.chat) && mj_chat_find(m->id)) {
        mj_chat_insert(m);
        m = NULL;
    }
    mj_store_unlock();
    mj_msg_free(m);
}

static void got_message_id(int32_t id, int64_t random_id)
{
    mj_store_lock();
    mj_msg_t *m = mj_chat_find_random(random_id);
    if (m) {
        /* out of the list and in again with its real id */
        mj_msg_t *c = (mj_msg_t *)mj_malloc(sizeof *c);
        if (c) {
            *c = *m;
            m->text = NULL;
            m->media = NULL;
            c->id = id;
            c->pending = false;
            mj_chat_remove(m->id);
            if (!mj_chat_find(id)) mj_chat_insert(c);
            else mj_msg_free(c);
        }
    }
    mj_store_unlock();
}

static void got_delete(const tl_vec_t *ids, mj_pkey_t channel)
{
    mj_store_lock();
    bool here = channel.type ? mj_pkey_eq(channel, A.chat) : A.chat.type != MJ_PEER_CHANNEL;
    if (here)
        for (uint32_t i = 0; i < tl_vn(ids); i++) mj_chat_remove((int32_t)tl_vl(ids, i));
    mj_store_unlock();
}

static void got_read_in(mj_pkey_t k, int32_t max_id, int still)
{
    mj_store_lock();
    mj_dialog_t *d = mj_store_dialog(k);
    if (d) {
        d->read_in_max = max_id;
        d->unread = still;
        mj_store_unread_count();
        mj_S.dialogs_ver++;
    }
    mj_store_unlock();
}

static void got_read_out(mj_pkey_t k, int32_t max_id)
{
    mj_store_lock();
    mj_dialog_t *d = mj_store_dialog(k);
    if (d && max_id > d->read_out_max) {
        d->read_out_max = max_id;
        mj_S.dialogs_ver++;
        if (mj_pkey_eq(k, A.chat)) mj_S.chat_ver++;
    }
    mj_store_unlock();
}

static void got_typing(mj_pkey_t chat, mj_pkey_t who, const tl_obj_t *action)
{
    if (!mj_pkey_eq(chat, A.chat)) return;
    mj_store_lock();
    if (action && action->id == TLC_sendMessageCancelAction) {
        mj_S.typing[0] = 0;
    } else {
        mj_peer_t *p = mj_store_peer(who);
        scpy(mj_S.typing, sizeof mj_S.typing, p ? (p->first[0] ? p->first : p->name) : "?");
        mj_S.typing_until_ms = mj_now_ms() + 6000;
    }
    mj_S.chat_ver++;
    mj_store_unlock();
}

static void got_reactions(mj_pkey_t k, int32_t id, const tl_obj_t *r)
{
    if (!mj_pkey_eq(k, A.chat)) return;
    mj_store_lock();
    mj_msg_t *m = mj_chat_find(id);
    if (m) {
        mj_msg_set_reactions(m, r);
        mj_S.chat_ver++;
    }
    mj_store_unlock();
}

/* One update. checked: its pts goes through check_pts (false inside a
 * difference, whose state already counts them). */
static void apply_update(const tl_obj_t *u, bool checked)
{
    if (!u) return;
    switch (u->id) {
    case TLC_updateNewMessage:
        if (checked && check_pts(tl_i(u, TLN_pts), tl_i(u, TLN_pts_count)) != 1) return;
        got_new(tl_o(u, TLN_message));
        break;
    case TLC_updateNewChannelMessage:
        got_new(tl_o(u, TLN_message));
        break;
    case TLC_updateEditMessage:
        if (checked && check_pts(tl_i(u, TLN_pts), tl_i(u, TLN_pts_count)) != 1) return;
        got_edit(tl_o(u, TLN_message));
        break;
    case TLC_updateEditChannelMessage:
        got_edit(tl_o(u, TLN_message));
        break;
    case TLC_updateMessageID:
        got_message_id(tl_i(u, TLN_id), tl_l(u, TLN_random_id));
        break;
    case TLC_updateDeleteMessages: {
        if (checked && check_pts(tl_i(u, TLN_pts), tl_i(u, TLN_pts_count)) != 1) return;
        mj_pkey_t none = {0, 0};
        got_delete(tl_v(u, TLN_messages), none);
        break;
    }
    case TLC_updateDeleteChannelMessages: {
        mj_pkey_t k = {MJ_PEER_CHANNEL, tl_l(u, TLN_channel_id)};
        got_delete(tl_v(u, TLN_messages), k);
        break;
    }
    case TLC_updateReadHistoryInbox:
        if (checked && check_pts(tl_i(u, TLN_pts), tl_i(u, TLN_pts_count)) != 1) return;
        got_read_in(mj_peer_of(tl_o(u, TLN_peer)), tl_i(u, TLN_max_id), tl_i(u, TLN_still_unread_count));
        break;
    case TLC_updateReadHistoryOutbox:
        if (checked && check_pts(tl_i(u, TLN_pts), tl_i(u, TLN_pts_count)) != 1) return;
        got_read_out(mj_peer_of(tl_o(u, TLN_peer)), tl_i(u, TLN_max_id));
        break;
    case TLC_updateReadChannelInbox: {
        mj_pkey_t k = {MJ_PEER_CHANNEL, tl_l(u, TLN_channel_id)};
        got_read_in(k, tl_i(u, TLN_max_id), tl_i(u, TLN_still_unread_count));
        break;
    }
    case TLC_updateReadChannelOutbox: {
        mj_pkey_t k = {MJ_PEER_CHANNEL, tl_l(u, TLN_channel_id)};
        got_read_out(k, tl_i(u, TLN_max_id));
        break;
    }
    case TLC_updateUserTyping: {
        mj_pkey_t k = {MJ_PEER_USER, tl_l(u, TLN_user_id)};
        got_typing(k, k, tl_o(u, TLN_action));
        break;
    }
    case TLC_updateChatUserTyping: {
        mj_pkey_t k = {MJ_PEER_CHAT, tl_l(u, TLN_chat_id)};
        got_typing(k, mj_peer_of(tl_o(u, TLN_from_id)), tl_o(u, TLN_action));
        break;
    }
    case TLC_updateChannelUserTyping: {
        mj_pkey_t k = {MJ_PEER_CHANNEL, tl_l(u, TLN_channel_id)};
        got_typing(k, mj_peer_of(tl_o(u, TLN_from_id)), tl_o(u, TLN_action));
        break;
    }
    case TLC_updateMessageReactions:
        got_reactions(mj_peer_of(tl_o(u, TLN_peer)), tl_i(u, TLN_msg_id), tl_o(u, TLN_reactions));
        break;
    case TLC_updateUserStatus: {
        mj_pkey_t k = {MJ_PEER_USER, tl_l(u, TLN_user_id)};
        mj_store_lock();
        mj_peer_t *p = mj_store_peer(k);
        tl_obj_t *st = tl_o(u, TLN_status);
        if (p && st) {
            p->online_until = st->id == TLC_userStatusOnline ? tl_i(st, TLN_expires) : 0;
            if (st->id == TLC_userStatusOffline) p->last_seen = tl_i(st, TLN_was_online);
            mj_S.peers_ver++;
        }
        mj_store_unlock();
        break;
    }
    case TLC_updateDialogPinned: {
        tl_obj_t *dp = tl_o(u, TLN_peer);
        if (dp && dp->id == TLC_dialogPeer) {
            mj_store_lock();
            mj_dialog_t *d = mj_store_dialog(mj_peer_of(tl_o(dp, TLN_peer)));
            if (d) {
                d->pinned = tl_t(u, TLN_pinned);
                mj_S.dialogs_ver++;
            }
            mj_store_unlock();
        }
        break;
    }
    case TLC_updatePinnedDialogs:
        mj_api_reload_dialogs();
        break;
    case TLC_updateChannelTooLong:
        break;
    default:
        break;
    }
}

static void put_peers(const tl_obj_t *o)
{
    mj_store_lock();
    mj_store_put_users(tl_v(o, TLN_users));
    mj_store_put_chats(tl_v(o, TLN_chats));
    mj_store_unlock();
}

/* updateShortMessage and updateShortChatMessage carry a message in pieces:
 * it is made whole as a message, the same way the rest go. */
static void short_message(mj_arena_t *a, const tl_obj_t *o)
{
    if (check_pts(tl_i(o, TLN_pts), tl_i(o, TLN_pts_count)) != 1) return;
    tl_obj_t *m = tl_new(a, TLC_message);
    tl_set_i(m, TLN_id, tl_i(o, TLN_id));
    tl_set_i(m, TLN_date, tl_i(o, TLN_date));
    tl_set_s(m, TLN_message, tl_s(o, TLN_message));
    if (tl_t(o, TLN_out)) tl_set_t(m, TLN_out);
    if (tl_t(o, TLN_mentioned)) tl_set_t(m, TLN_mentioned);
    if (tl_o(o, TLN_reply_to)) tl_set_o(m, TLN_reply_to, tl_o(o, TLN_reply_to));
    if (tl_o(o, TLN_fwd_from)) tl_set_o(m, TLN_fwd_from, tl_o(o, TLN_fwd_from));
    tl_obj_t *peer, *from;
    if (o->id == TLC_updateShortMessage) {
        peer = tl_new(a, TLC_peerUser);
        tl_set_l(peer, TLN_user_id, tl_l(o, TLN_user_id));
        from = tl_t(o, TLN_out) ? NULL : peer;
    } else {
        peer = tl_new(a, TLC_peerChat);
        tl_set_l(peer, TLN_chat_id, tl_l(o, TLN_chat_id));
        from = tl_new(a, TLC_peerUser);
        tl_set_l(from, TLN_user_id, tl_l(o, TLN_from_id));
    }
    tl_set_o(m, TLN_peer_id, peer);
    if (from) tl_set_o(m, TLN_from_id, from);
    got_new(m);
}

static void handle_updates(void *ud, int dc, tl_obj_t *o, mj_arena_t *a)
{
    (void)ud;
    (void)dc;
    if (!A.logged || !o) return;
    switch (o->id) {
    case TLC_updatesTooLong:
        get_difference();
        break;
    case TLC_updateShortMessage:
    case TLC_updateShortChatMessage:
        short_message(a, o);
        break;
    case TLC_updateShort:
        apply_update(tl_o(o, TLN_update), true);
        if (tl_i(o, TLN_date) > A.date) A.date = tl_i(o, TLN_date);
        break;
    case TLC_updates:
    case TLC_updatesCombined: {
        int32_t seq = tl_i(o, TLN_seq);
        int32_t start = o->id == TLC_updatesCombined ? tl_i(o, TLN_seq_start) : seq;
        if (seq && A.seq && start > A.seq + 1) {
            mj_log("api: a gap in seq (%d, then %d)", (int)A.seq, (int)start);
            get_difference();
            return;
        }
        if (seq && A.seq && start <= A.seq && seq <= A.seq) return;   /* already had them */
        put_peers(o);
        tl_vec_t *v = tl_v(o, TLN_updates);
        /* the ids of our own messages first, so the pending ones are found */
        for (uint32_t i = 0; i < tl_vn(v); i++)
            if (tl_vo(v, i) && tl_vo(v, i)->id == TLC_updateMessageID) apply_update(tl_vo(v, i), true);
        for (uint32_t i = 0; i < tl_vn(v); i++)
            if (tl_vo(v, i) && tl_vo(v, i)->id != TLC_updateMessageID) apply_update(tl_vo(v, i), true);
        if (seq) A.seq = seq;
        if (tl_i(o, TLN_date) > A.date) A.date = tl_i(o, TLN_date);
        break;
    }
    case TLC_updateShortSentMessage:
        check_pts(tl_i(o, TLN_pts), tl_i(o, TLN_pts_count));
        break;
    }
}

/* ------------------------------------------------------------ difference */

static void diff_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    A.diff_busy = false;
    if (common_errors(r)) return;
    tl_obj_t *o = obj(r);
    if (!o) return;
    switch (o->id) {
    case TLC_updates_differenceEmpty:
        A.date = tl_i(o, TLN_date);
        A.seq = tl_i(o, TLN_seq);
        state_save();
        break;
    case TLC_updates_difference:
    case TLC_updates_differenceSlice: {
        put_peers(o);
        tl_vec_t *nm = tl_v(o, TLN_new_messages);
        for (uint32_t i = 0; i < tl_vn(nm); i++) got_new(tl_vo(nm, i));
        tl_vec_t *ou = tl_v(o, TLN_other_updates);
        for (uint32_t i = 0; i < tl_vn(ou); i++) apply_update(tl_vo(ou, i), false);
        bool slice = o->id == TLC_updates_differenceSlice;
        set_state(tl_o(o, slice ? TLN_intermediate_state : TLN_state));
        mj_log("api: difference: %u new messages, %u other updates%s", (unsigned)tl_vn(nm), (unsigned)tl_vn(ou),
               slice ? ", more" : "");
        if (slice) get_difference();
        break;
    }
    case TLC_updates_differenceTooLong:
        A.pts = tl_i(o, TLN_pts);
        state_save();
        mj_api_reload_dialogs();
        break;
    }
}

static void get_difference(void)
{
    A.diff_at_ms = 0;
    if (A.diff_busy || !A.logged || !A.pts) return;
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_updates_getDifference);
    tl_set_i(q, TLN_pts, A.pts);
    tl_set_i(q, TLN_date, A.date);
    tl_set_i(q, TLN_qts, A.qts);
    A.diff_busy = call(q, diff_cb, NULL);
    mj_arena_free(&a);
}

static void state_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    if (common_errors(r)) return;
    tl_obj_t *o = obj(r);
    if (o && o->id == TLC_updates_state) set_state(o);
}

static void new_session(void *ud, int dc)
{
    (void)ud;
    /* the server may have dropped updates meanwhile */
    if (dc == A.home && A.logged && A.pts) {
        if (!A.diff_at_ms) A.diff_at_ms = mj_now_ms() + 300;
    }
}

static void net_state(void *ud, int dc, int st)
{
    (void)ud;
    if (dc != A.home) return;
    mj_store_lock();
    mj_S.net = st;
    mj_S.net_ver++;
    mj_store_unlock();
}

/* ----------------------------------------------------------- the chat list */

static void dialogs_cb(void *ctx, const mj_rpc_t *r)
{
    bool reload = ctx != NULL;
    A.dialogs_busy = false;
    mj_store_lock();
    mj_S.dialogs_loading = false;
    mj_S.dialogs_ver++;
    mj_store_unlock();
    if (common_errors(r)) return;
    tl_obj_t *o = obj(r);
    if (!o || (o->id != TLC_messages_dialogs && o->id != TLC_messages_dialogsSlice)) {
        if (r->code) mj_log("api: dialogs: %d %s", r->code, r->msg ? r->msg : "");
        return;
    }
    put_peers(o);
    tl_vec_t *dl = tl_v(o, TLN_dialogs), *ms = tl_v(o, TLN_messages);
    int64_t now = mj_mtp_server_time(A.mtp);
    mj_store_lock();
    if (reload) mj_S.ndialogs = 0;
    for (uint32_t i = 0; i < tl_vn(dl); i++) {
        tl_obj_t *t = tl_vo(dl, i);
        if (!t || t->id != TLC_dialog) continue;
        mj_pkey_t k = mj_peer_of(tl_o(t, TLN_peer));
        mj_dialog_t *d = mj_store_dialog_add(k);
        if (!d) break;
        d->unread = tl_i(t, TLN_unread_count);
        d->unread_mentions = tl_i(t, TLN_unread_mentions_count);
        d->read_in_max = tl_i(t, TLN_read_inbox_max_id);
        d->read_out_max = tl_i(t, TLN_read_outbox_max_id);
        d->pinned = tl_t(t, TLN_pinned);
        d->pts = tl_i(t, TLN_pts);
        tl_obj_t *ns = tl_o(t, TLN_notify_settings);
        d->muted = ns && tl_i(ns, TLN_mute_until) > now;
        int32_t top = tl_i(t, TLN_top_message);
        for (uint32_t j = 0; j < tl_vn(ms); j++) {
            tl_obj_t *mt = tl_vo(ms, j);
            if (!mt || tl_i(mt, TLN_id) != top || !mj_pkey_eq(mj_peer_of(tl_o(mt, TLN_peer_id)), k)) continue;
            mj_msg_t *m = mj_msg_from_tl(mt);
            if (m) {
                d->top_id = 0;
                mj_store_dialog_top(d, m);
                mj_msg_free(m);
            }
            break;
        }
        A.dl_offset_date = d->date;
        A.dl_offset_id = d->top_id;
        A.dl_offset_peer = k;
    }
    mj_S.dialogs_end = tl_vn(dl) < DIALOG_PAGE;
    mj_store_unread_count();
    mj_S.dialogs_ver++;
    mj_store_unlock();
    mj_log("api: %u chats (%d in all)", (unsigned)tl_vn(dl), mj_S.ndialogs);
}

static void load_dialogs(bool reload)
{
    if (A.dialogs_busy || !A.logged) return;
    if (!reload && mj_S.dialogs_end) return;
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_messages_getDialogs);
    if (reload) {
        tl_set_i(q, TLN_offset_date, 0);
        tl_set_i(q, TLN_offset_id, 0);
        tl_set_o(q, TLN_offset_peer, tl_new(&a, TLC_inputPeerEmpty));
    } else {
        tl_set_i(q, TLN_offset_date, A.dl_offset_date);
        tl_set_i(q, TLN_offset_id, A.dl_offset_id);
        tl_set_o(q, TLN_offset_peer, input_peer(&a, A.dl_offset_peer));
    }
    tl_set_i(q, TLN_limit, DIALOG_PAGE);
    tl_set_l(q, TLN_hash, 0);
    A.dialogs_busy = call(q, dialogs_cb, reload ? (void *)1 : NULL);
    mj_arena_free(&a);
    if (A.dialogs_busy) {
        mj_store_lock();
        mj_S.dialogs_loading = true;
        mj_S.dialogs_ver++;
        mj_store_unlock();
    }
}

/* ------------------------------------------------------------- history */

static void updates_cb(void *ctx, const mj_rpc_t *r);

static void mark_read(void)
{
    if (!A.chat.type) return;
    mj_dialog_t *d = mj_store_dialog(A.chat);
    if (!d || (d->unread == 0 && d->read_in_max >= d->top_id)) return;
    int32_t max = d->top_id;
    mj_arena_t a = {0};
    tl_obj_t *q;
    if (A.chat.type == MJ_PEER_CHANNEL) {
        q = tl_new(&a, TLC_channels_readHistory);
        tl_set_o(q, TLN_channel, input_channel(&a, A.chat));
    } else {
        q = tl_new(&a, TLC_messages_readHistory);
        tl_set_o(q, TLN_peer, input_peer(&a, A.chat));
    }
    tl_set_i(q, TLN_max_id, max);
    call(q, updates_cb, (void *)(intptr_t)MJ_T_NONE);
    mj_arena_free(&a);
    mj_store_lock();
    d->unread = 0;
    d->read_in_max = max;
    mj_store_unread_count();
    mj_S.dialogs_ver++;
    mj_store_unlock();
}

static void store_messages(const tl_vec_t *v, bool to_chat, bool tops, uint32_t gen)
{
    mj_store_lock();
    for (uint32_t i = 0; i < tl_vn(v); i++) {
        mj_msg_t *m = mj_msg_from_tl(tl_vo(v, i));
        if (!m) continue;
        if (tops) {
            mj_dialog_t *d = mj_store_dialog(m->chat);
            if (d) mj_store_dialog_top(d, m);
        }
        if (to_chat && gen == A.chat_gen && mj_pkey_eq(m->chat, A.chat)) mj_chat_insert(m);
        else mj_msg_free(m);
    }
    mj_store_unlock();
}

static void history_cb(void *ctx, const mj_rpc_t *r)
{
    uint32_t gen = (uint32_t)(uintptr_t)ctx;
    A.history_busy = false;
    mj_store_lock();
    if (gen == A.chat_gen) mj_S.chat_loading = false;
    mj_S.chat_ver++;
    mj_store_unlock();
    if (common_errors(r)) return;
    tl_obj_t *o = obj(r);
    if (!o || gen != A.chat_gen) return;
    put_peers(o);
    tl_vec_t *v = tl_v(o, TLN_messages);
    store_messages(v, true, false, gen);
    mj_store_lock();
    if (tl_vn(v) < HISTORY_PAGE) mj_S.chat_start = true;
    mj_S.chat_ver++;
    mj_store_unlock();
    mark_read();
}

static void load_history(bool older)
{
    if (!A.chat.type || A.history_busy) return;
    if (older && mj_S.chat_start) return;
    int32_t off = 0;
    if (older) {
        for (int i = 0; i < mj_S.nmsgs; i++)
            if (!mj_S.msgs[i]->pending) {
                off = mj_S.msgs[i]->id;
                break;
            }
        if (!off) return;
    }
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_messages_getHistory);
    tl_set_o(q, TLN_peer, input_peer(&a, A.chat));
    tl_set_i(q, TLN_offset_id, off);
    tl_set_i(q, TLN_offset_date, 0);
    tl_set_i(q, TLN_add_offset, 0);
    tl_set_i(q, TLN_limit, HISTORY_PAGE);
    tl_set_i(q, TLN_max_id, 0);
    tl_set_i(q, TLN_min_id, 0);
    tl_set_l(q, TLN_hash, 0);
    A.history_busy = call(q, history_cb, (void *)(uintptr_t)A.chat_gen);
    mj_arena_free(&a);
    mj_store_lock();
    mj_S.chat_loading = A.history_busy;
    mj_S.chat_ver++;
    mj_store_unlock();
}

/* --------------------------------------------------------------- sending */

static mj_msg_t *pending_msg(mj_pkey_t k, const char *text, int32_t reply_to, int64_t random_id)
{
    mj_msg_t *m = (mj_msg_t *)mj_calloc(sizeof *m);
    if (!m) return NULL;
    m->text = mj_strdup(text ? text : "");
    m->id = (int32_t)(-(++A.pending_seq));
    m->date = (int32_t)mj_mtp_server_time(A.mtp);
    m->chat = k;
    m->from = mj_S.self;
    m->out = true;
    m->pending = true;
    m->reply_to = reply_to;
    m->random_id = random_id;
    return m;
}

static void show_pending(mj_msg_t *m)
{
    mj_store_lock();
    mj_dialog_t *d = mj_store_dialog_add(m->chat);
    if (d) {
        mj_store_dialog_top(d, m);
        mj_store_dialog_raise(m->chat);
        mj_S.dialogs_ver++;
    }
    if (mj_pkey_eq(m->chat, A.chat)) mj_chat_insert(m);
    else mj_msg_free(m);
    mj_store_unlock();
}

static void sent_cb(void *ctx, const mj_rpc_t *r)
{
    int64_t *rid = (int64_t *)ctx;
    int64_t random_id = *rid;
    mj_free(rid);
    tl_obj_t *o = obj(r);
    if (!o) {
        if (!common_errors(r)) mj_log("api: send: %d %s", r->code, r->msg ? r->msg : "");
        mj_store_lock();
        mj_msg_t *m = mj_chat_find_random(random_id);
        if (m) {
            m->failed = true;
            mj_S.chat_ver++;
        }
        mj_store_toast(MJ_T_SEND_FAILED, r->msg);
        mj_store_unlock();
        return;
    }
    if (o->id == TLC_updateShortSentMessage) {
        check_pts(tl_i(o, TLN_pts), tl_i(o, TLN_pts_count));
        got_message_id(tl_i(o, TLN_id), random_id);
        mj_store_lock();
        mj_msg_t *m = mj_chat_find(tl_i(o, TLN_id));
        if (m) {
            m->date = tl_i(o, TLN_date);
            mj_S.chat_ver++;
        }
        mj_dialog_t *d = mj_store_dialog(A.chat);
        if (d && m) {
            mj_store_dialog_top(d, m);
            mj_S.dialogs_ver++;
        }
        mj_store_unlock();
    } else {
        handle_updates(NULL, r->dc, o, r->arena);
    }
}

static void send_text(mj_pkey_t k, const char *text, int32_t reply_to)
{
    if (!text || !*text) return;
    int64_t random_id;
    mj_random(&random_id, 8);
    mj_msg_t *pm = pending_msg(k, text, reply_to, random_id);
    if (pm) show_pending(pm);
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_messages_sendMessage);
    tl_set_o(q, TLN_peer, input_peer(&a, k));
    tl_set_s(q, TLN_message, text);
    tl_set_l(q, TLN_random_id, random_id);
    if (reply_to > 0) {
        tl_obj_t *rt = tl_new(&a, TLC_inputReplyToMessage);
        tl_set_i(rt, TLN_reply_to_msg_id, reply_to);
        tl_set_o(q, TLN_reply_to, rt);
    }
    int64_t *ctx = (int64_t *)mj_malloc(sizeof *ctx);
    if (ctx) {
        *ctx = random_id;
        if (!call(q, sent_cb, ctx)) mj_free(ctx);
    }
    mj_arena_free(&a);
}

/* A photo: its parts, one after the other, then sendMedia. */
static void upload_next(void);

static void upload_end(bool ok)
{
    upload_t *u = A.up;
    if (!u) return;
    if (!ok) {
        mj_store_lock();
        mj_msg_t *m = mj_chat_find_random(u->random_id);
        if (m) {
            m->failed = true;
            mj_S.chat_ver++;
        }
        mj_store_toast(MJ_T_PHOTO_FAILED, NULL);
        mj_store_unlock();
    }
    mj_free(u->data);
    mj_free(u->caption);
    mj_free(u);
    A.up = NULL;
}

static void part_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    if (!A.up) return;
    if (r->code || r->val.kind != TLK_BOOL || !r->val.u.i) {
        mj_log("api: a photo part failed: %d %s", r->code, r->msg ? r->msg : "");
        upload_end(false);
        return;
    }
    A.up->part++;
    upload_next();
}

static void photo_sent_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    upload_t *u = A.up;
    tl_obj_t *o = obj(r);
    if (!o) {
        mj_log("api: sendMedia: %d %s", r->code, r->msg ? r->msg : "");
        upload_end(false);
        return;
    }
    handle_updates(NULL, r->dc, o, r->arena);
    (void)u;
    upload_end(true);
}

static void upload_next(void)
{
    upload_t *u = A.up;
    mj_arena_t a = {0};
    if (u->part < u->parts) {
        size_t off = (size_t)u->part * PART;
        size_t n = u->n - off < PART ? u->n - off : PART;
        tl_obj_t *q = tl_new(&a, TLC_upload_saveFilePart);
        tl_set_l(q, TLN_file_id, u->file_id);
        tl_set_i(q, TLN_file_part, u->part);
        tl_set_b(q, TLN_bytes, u->data + off, (uint32_t)n);
        if (!call(q, part_cb, NULL)) upload_end(false);
        mj_arena_free(&a);
        return;
    }
    tl_obj_t *file = tl_new(&a, TLC_inputFile);
    tl_set_l(file, TLN_id, u->file_id);
    tl_set_i(file, TLN_parts, u->parts);
    tl_set_s(file, TLN_name, u->name);
    tl_set_s(file, TLN_md5_checksum, "");
    tl_obj_t *media = tl_new(&a, TLC_inputMediaUploadedPhoto);
    tl_set_o(media, TLN_file, file);
    tl_obj_t *q = tl_new(&a, TLC_messages_sendMedia);
    tl_set_o(q, TLN_peer, input_peer(&a, u->k));
    tl_set_o(q, TLN_media, media);
    tl_set_s(q, TLN_message, u->caption ? u->caption : "");
    tl_set_l(q, TLN_random_id, u->random_id);
    if (u->reply_to > 0) {
        tl_obj_t *rt = tl_new(&a, TLC_inputReplyToMessage);
        tl_set_i(rt, TLN_reply_to_msg_id, u->reply_to);
        tl_set_o(q, TLN_reply_to, rt);
    }
    if (!call(q, photo_sent_cb, NULL)) upload_end(false);
    mj_arena_free(&a);
}

static void send_photo(mj_pkey_t k, const char *path, const char *caption, int32_t reply_to)
{
    if (A.up) {
        mj_store_lock();
        mj_store_toast(MJ_T_PHOTO_FAILED, NULL);
        mj_store_unlock();
        return;
    }
    struct stat st;
    if (stat(path, &st) || st.st_size <= 0) {
        mj_store_lock();
        mj_store_toast(MJ_T_PHOTO_FAILED, NULL);
        mj_store_unlock();
        return;
    }
    if (st.st_size > PHOTO_MAX) {
        mj_store_lock();
        mj_store_toast(MJ_T_PHOTO_TOO_BIG, NULL);
        mj_store_unlock();
        return;
    }
    upload_t *u = (upload_t *)mj_calloc(sizeof *u);
    if (!u) return;
    u->n = (size_t)st.st_size;
    u->data = (uint8_t *)mj_malloc(u->n);
    FILE *f = u->data ? fopen(path, "rb") : NULL;
    if (!f || fread(u->data, 1, u->n, f) != u->n) {
        if (f) fclose(f);
        mj_free(u->data);
        mj_free(u);
        mj_store_lock();
        mj_store_toast(MJ_T_PHOTO_FAILED, NULL);
        mj_store_unlock();
        return;
    }
    fclose(f);
    u->k = k;
    u->reply_to = reply_to;
    u->caption = caption ? mj_strdup(caption) : NULL;
    mj_random(&u->random_id, 8);
    mj_random(&u->file_id, 8);
    u->parts = (int)((u->n + PART - 1) / PART);
    const char *base = strrchr(path, '/');
    scpy(u->name, sizeof u->name, base ? base + 1 : path);
    A.up = u;
    mj_msg_t *pm = pending_msg(k, caption, reply_to, u->random_id);
    if (pm) {
        pm->media = (mj_media_t *)mj_calloc(sizeof *pm->media);
        if (pm->media) {
            pm->media->kind = MJ_MEDIA_PHOTO;
            scpy(pm->media->name, sizeof pm->media->name, path);   /* the screen shows the local file */
        }
        show_pending(pm);
    }
    mj_log("api: sending a photo of %u KB in %d parts", (unsigned)(u->n / 1024), u->parts);
    upload_next();
}

/* edit, delete, forward, react */

static void updates_cb(void *ctx, const mj_rpc_t *r)
{
    int code = (int)(intptr_t)ctx;
    tl_obj_t *o = obj(r);
    if (!o) {
        if (!common_errors(r)) {
            mj_log("api: %d %s", r->code, r->msg ? r->msg : "");
            if (code != MJ_T_NONE) {
                mj_store_lock();
                mj_store_toast(code, r->msg);
                mj_store_unlock();
            }
        }
        return;
    }
    if (o->id == TLC_messages_affectedMessages) {
        check_pts(tl_i(o, TLN_pts), tl_i(o, TLN_pts_count));
        return;
    }
    handle_updates(NULL, r->dc, o, r->arena);
    if (code == MJ_T_FORWARD_FAILED) {
        mj_store_lock();
        mj_store_toast(MJ_T_FORWARDED, NULL);
        mj_store_unlock();
    }
}

static void do_edit(mj_pkey_t k, int32_t id, const char *text)
{
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_messages_editMessage);
    tl_set_o(q, TLN_peer, input_peer(&a, k));
    tl_set_i(q, TLN_id, id);
    tl_set_s(q, TLN_message, text);
    call(q, updates_cb, (void *)(intptr_t)MJ_T_EDIT_FAILED);
    mj_arena_free(&a);
    /* shown at once; the update confirms it */
    mj_store_lock();
    mj_msg_t *m = mj_chat_find(id);
    if (m && mj_pkey_eq(k, A.chat)) {
        char *t = mj_strdup(text);
        if (t) {
            mj_free(m->text);
            m->text = t;
            m->edit_date = (int32_t)mj_mtp_server_time(A.mtp);
            mj_S.chat_ver++;
        }
    }
    mj_store_unlock();
}

static void do_delete(mj_pkey_t k, int32_t id, bool revoke)
{
    mj_arena_t a = {0};
    tl_vec_t *ids = tl_vec_new(&a, 1);
    tl_vset_i(ids, 0, id);
    tl_obj_t *q;
    if (k.type == MJ_PEER_CHANNEL) {
        q = tl_new(&a, TLC_channels_deleteMessages);
        tl_set_o(q, TLN_channel, input_channel(&a, k));
    } else {
        q = tl_new(&a, TLC_messages_deleteMessages);
        if (revoke) tl_set_t(q, TLN_revoke);
    }
    tl_set_v(q, TLN_id, ids);
    call(q, updates_cb, (void *)(intptr_t)MJ_T_DELETE_FAILED);
    mj_arena_free(&a);
    mj_store_lock();
    if (mj_pkey_eq(k, A.chat)) mj_chat_remove(id);
    mj_store_unlock();
}

static void do_forward(mj_pkey_t from, int32_t id, mj_pkey_t to)
{
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_messages_forwardMessages);
    tl_set_o(q, TLN_from_peer, input_peer(&a, from));
    tl_set_o(q, TLN_to_peer, input_peer(&a, to));
    tl_vec_t *ids = tl_vec_new(&a, 1), *rnd = tl_vec_new(&a, 1);
    int64_t r;
    mj_random(&r, 8);
    tl_vset_i(ids, 0, id);
    tl_vset_l(rnd, 0, r);
    tl_set_v(q, TLN_id, ids);
    tl_set_v(q, TLN_random_id, rnd);
    call(q, updates_cb, (void *)(intptr_t)MJ_T_FORWARD_FAILED);
    mj_arena_free(&a);
}

static void do_react(mj_pkey_t k, int32_t id, const char *emoji)
{
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_messages_sendReaction);
    tl_set_o(q, TLN_peer, input_peer(&a, k));
    tl_set_i(q, TLN_msg_id, id);
    bool add = emoji && *emoji;
    tl_vec_t *v = tl_vec_new(&a, add ? 1 : 0);
    if (add) {
        tl_obj_t *re = tl_new(&a, TLC_reactionEmoji);
        tl_set_s(re, TLN_emoticon, emoji);
        tl_vset_o(v, 0, re);
    }
    tl_set_v(q, TLN_reaction, v);
    call(q, updates_cb, (void *)(intptr_t)MJ_T_REACT_FAILED);
    mj_arena_free(&a);
}

static void do_typing(mj_pkey_t k)
{
    uint32_t now = mj_now_ms();
    if (A.typing_ms && (uint32_t)(now - A.typing_ms) < 5000) return;
    A.typing_ms = now;
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_messages_setTyping);
    tl_set_o(q, TLN_peer, input_peer(&a, k));
    tl_set_o(q, TLN_action, tl_new(&a, TLC_sendMessageTypingAction));
    call(q, NULL, NULL);
    mj_arena_free(&a);
}

/* ---------------------------------------------------------------- search */

static int s_search_parts;

static void search_end(void)
{
    if (--s_search_parts > 0) return;
    mj_store_lock();
    mj_S.searching = false;
    mj_S.search_ver++;
    mj_store_unlock();
}

static void search_msgs_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    tl_obj_t *o = obj(r);
    if (o) {
        put_peers(o);
        tl_vec_t *v = tl_v(o, TLN_messages);
        mj_store_lock();
        for (uint32_t i = 0; i < tl_vn(v) && mj_S.nfound < 40; i++) {
            mj_msg_t *m = mj_msg_from_tl(tl_vo(v, i));
            if (m) mj_S.found[mj_S.nfound++] = m;
        }
        mj_store_unlock();
    }
    search_end();
}

static void search_peers_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    tl_obj_t *o = obj(r);
    if (o && o->id == TLC_contacts_found) {
        put_peers(o);
        mj_store_lock();
        for (int pass = 0; pass < 2; pass++) {
            tl_vec_t *v = tl_v(o, pass ? TLN_results : TLN_my_results);
            for (uint32_t i = 0; i < tl_vn(v) && mj_S.nfound_peers < 20; i++)
                mj_S.found_peers[mj_S.nfound_peers++] = mj_peer_of(tl_vo(v, i));
        }
        mj_store_unlock();
    }
    search_end();
}

static void do_search(const char *q)
{
    mj_store_lock();
    for (int i = 0; i < mj_S.nfound; i++) mj_msg_free(mj_S.found[i]);
    mj_S.nfound = mj_S.nfound_peers = 0;
    if (!mj_S.found) mj_S.found = (mj_msg_t **)mj_calloc(40 * sizeof(mj_msg_t *));
    if (!mj_S.found_peers) mj_S.found_peers = (mj_pkey_t *)mj_calloc(20 * sizeof(mj_pkey_t));
    mj_S.searching = q && *q && mj_S.found && mj_S.found_peers;
    mj_S.search_ver++;
    mj_store_unlock();
    if (!mj_S.searching) return;
    mj_arena_t a = {0};
    tl_obj_t *g = tl_new(&a, TLC_messages_searchGlobal);
    tl_set_s(g, TLN_q, q);
    tl_set_o(g, TLN_filter, tl_new(&a, TLC_inputMessagesFilterEmpty));
    tl_set_i(g, TLN_min_date, 0);
    tl_set_i(g, TLN_max_date, 0);
    tl_set_i(g, TLN_offset_rate, 0);
    tl_set_o(g, TLN_offset_peer, tl_new(&a, TLC_inputPeerEmpty));
    tl_set_i(g, TLN_offset_id, 0);
    tl_set_i(g, TLN_limit, 30);
    s_search_parts = 0;
    if (call(g, search_msgs_cb, NULL)) s_search_parts++;
    tl_obj_t *c = tl_new(&a, TLC_contacts_search);
    tl_set_s(c, TLN_q, q);
    tl_set_i(c, TLN_limit, 10);
    if (call(c, search_peers_cb, NULL)) s_search_parts++;
    mj_arena_free(&a);
    if (!s_search_parts) search_end();
}

/* ----------------------------------------------------------------- files */

bool mj_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static void safe_name(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        bool bad = c == '/' || c == '\\' || c == ':' || c < 32 || (j == 0 && c == '.');
        out[j] = bad ? '_' : (char)c;
        j++;
    }
    out[j] = 0;
}

void mj_api_path(int kind, const mj_peer_t *p, const mj_media_t *md, int32_t msg_id, char *out, size_t n)
{
    const char *dd = A.cfg.data_dir;
    switch (kind) {
    case MJ_FETCH_AVATAR:
        snprintf(out, n, "%s/cache/a%lld.jpg", dd, (long long)(p ? p->photo_id : 0));
        break;
    case MJ_FETCH_PHOTO:
        snprintf(out, n, "%s/cache/p%lld%s.jpg", dd, (long long)md->id, md->thumb);
        break;
    case MJ_FETCH_THUMB:
        snprintf(out, n, "%s/cache/t%lld%s.jpg", dd, (long long)md->id, md->thumb_small);
        break;
    default: {
        char nm[96];
        if (md->name[0]) safe_name(nm, sizeof nm, md->name);
        else if (md->kind == MJ_MEDIA_VOICE) snprintf(nm, sizeof nm, "voz-%d.ogg", (int)msg_id);
        else if (md->kind == MJ_MEDIA_VIDEO || md->kind == MJ_MEDIA_ROUND || md->kind == MJ_MEDIA_GIF)
            snprintf(nm, sizeof nm, "video-%d.mp4", (int)msg_id);
        else if (md->kind == MJ_MEDIA_STICKER) snprintf(nm, sizeof nm, "sticker-%d.webp", (int)msg_id);
        else snprintf(nm, sizeof nm, "archivo-%d", (int)msg_id);
        snprintf(out, n, "%s/%s", A.cfg.dl_dir, nm);
        break;
    }
    }
}

static void fetch_free(fetch_t *f)
{
    if (f->f) fclose(f->f);
    mj_arena_free(&f->a);
    mj_free(f);
}

static void fetch_done(fetch_t *f, bool ok)
{
    fetch_t **l = &A.fq;
    while (*l && *l != f) l = &(*l)->next;
    if (*l) *l = f->next;
    A.fetch_n--;
    char part[192];
    snprintf(part, sizeof part, "%s.part", f->path);
    if (f->f) {
        fclose(f->f);
        f->f = NULL;
    }
    if (ok && rename(part, f->path) == 0) {
        mj_store_lock();
        mj_store_media_done(f->path);
        if (f->kind == MJ_FETCH_DOC) {
            scpy(mj_S.last_saved, sizeof mj_S.last_saved, f->path);
            mj_store_toast(MJ_T_SAVED, f->path);
        }
        if (f->big) mj_S.download_pct = -1;
        mj_store_unlock();
    } else {
        unlink(part);
        if (f->kind == MJ_FETCH_DOC || f->kind == MJ_FETCH_PHOTO) {
            mj_store_lock();
            if (f->big) mj_S.download_pct = -1;
            mj_store_toast(MJ_T_DOWNLOAD_FAILED, NULL);
            mj_store_unlock();
        }
    }
    fetch_free(f);
}

static void fetch_cb(void *ctx, const mj_rpc_t *r)
{
    fetch_t *f = (fetch_t *)ctx;
    f->live = false;
    if (r->code) {
        int dc = mj_err_num(r->msg, "FILE_MIGRATE_");
        if (r->code == 303 && dc >= 1 && dc <= MJ_DC_MAX && f->tries < 3) {
            f->dc = dc;
            f->tries++;
            return;                     /* asked again from the loop */
        }
        if (r->code == 401 && f->dc != A.home && f->tries < 3) {
            /* not signed in there (any more) */
            A.dcok &= ~(1u << f->dc);
            f->tries++;
            return;
        }
        mj_log("api: download %s: %d %s", f->path, r->code, r->msg ? r->msg : "");
        fetch_done(f, false);
        return;
    }
    tl_obj_t *o = obj(r);
    uint32_t n = 0;
    const uint8_t *b = o && o->id == TLC_upload_file ? tl_b(o, TLN_bytes, &n) : NULL;
    if (!o || o->id != TLC_upload_file) {
        fetch_done(f, false);
        return;
    }
    if (!f->f) {
        char part[192];
        snprintf(part, sizeof part, "%s.part", f->path);
        f->f = fopen(part, "wb");
        if (!f->f) {
            mj_log("api: cannot write %s", part);
            fetch_done(f, false);
            return;
        }
    }
    if (n && fwrite(b, 1, n, f->f) != n) {
        fetch_done(f, false);
        return;
    }
    f->offset += n;
    if (f->big && f->size > 0) {
        mj_store_lock();
        mj_S.download_pct = (int)(f->offset * 100 / f->size);
        mj_S.media_ver++;
        mj_store_unlock();
    }
    if (n < CHUNK || (f->size > 0 && f->offset >= f->size)) fetch_done(f, true);
    /* else the loop asks for the next chunk */
}

static void import_cb(void *ctx, const mj_rpc_t *r)
{
    int dc = (int)(intptr_t)ctx;
    A.dc_importing &= ~(1u << dc);
    tl_obj_t *o = obj(r);
    if (o && o->id == TLC_auth_authorization) {
        A.dcok |= 1u << dc;
        mj_pref_set_i32("msj_dcok", (int32_t)A.dcok);
        mj_log("api: signed in at dc%d too", dc);
    } else {
        mj_log("api: importAuthorization at dc%d: %d %s", dc, r->code, r->msg ? r->msg : "");
    }
}

static void export_cb(void *ctx, const mj_rpc_t *r)
{
    int dc = (int)(intptr_t)ctx;
    tl_obj_t *o = obj(r);
    if (!o || o->id != TLC_auth_exportedAuthorization) {
        A.dc_importing &= ~(1u << dc);
        mj_log("api: exportAuthorization for dc%d: %d %s", dc, r->code, r->msg ? r->msg : "");
        return;
    }
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_auth_importAuthorization);
    tl_set_l(q, TLN_id, tl_l(o, TLN_id));
    uint32_t n;
    const uint8_t *b = tl_b(o, TLN_bytes, &n);
    tl_set_b(q, TLN_bytes, b, n);
    if (!call_dc(dc, q, import_cb, (void *)(intptr_t)dc)) A.dc_importing &= ~(1u << dc);
    mj_arena_free(&a);
}

static bool dc_ready(int dc)
{
    if (dc == A.home || (A.dcok & (1u << dc))) return true;
    if (A.dc_importing & (1u << dc)) return false;
    A.dc_importing |= 1u << dc;
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_auth_exportAuthorization);
    tl_set_i(q, TLN_dc_id, dc);
    if (!call(q, export_cb, (void *)(intptr_t)dc)) A.dc_importing &= ~(1u << dc);
    mj_arena_free(&a);
    return false;
}

static void fetch_pump(void)
{
    int live = 0;
    for (fetch_t *f = A.fq; f; f = f->next) live += f->live;
    for (fetch_t *f = A.fq; f && live < FETCH_LIVE; f = f->next) {
        if (f->live) continue;
        if (!dc_ready(f->dc)) continue;
        mj_arena_t a = {0};
        tl_obj_t *q = tl_new(&a, TLC_upload_getFile);
        tl_set_o(q, TLN_location, f->loc);
        tl_set_l(q, TLN_offset, f->offset);
        tl_set_i(q, TLN_limit, CHUNK);
        f->live = call_dc(f->dc, q, fetch_cb, f);
        mj_arena_free(&a);
        if (!f->live) {
            fetch_done(f, false);
            return;                     /* the list changed */
        }
        live++;
    }
}

static void fetch_add(const job_t *j)
{
    char path[176];
    mj_api_path(j->i, &j->peer, &j->md, j->id, path, sizeof path);
    if (mj_file_exists(path)) {
        mj_store_lock();
        mj_store_media_done(path);
        mj_store_unlock();
        return;
    }
    for (fetch_t *f = A.fq; f; f = f->next)
        if (!strcmp(f->path, path)) return;
    fetch_t *f = (fetch_t *)mj_calloc(sizeof *f);
    if (!f) return;
    f->kind = j->i;
    scpy(f->path, sizeof f->path, path);
    mj_arena_t *a = &f->a;
    if (j->i == MJ_FETCH_AVATAR) {
        f->dc = j->peer.photo_dc;
        f->loc = tl_new(a, TLC_inputPeerPhotoFileLocation);
        tl_set_o(f->loc, TLN_peer, input_peer(a, j->peer.key));
        tl_set_l(f->loc, TLN_photo_id, j->peer.photo_id);
    } else if (j->i == MJ_FETCH_PHOTO) {
        f->dc = j->md.dc;
        f->loc = tl_new(a, TLC_inputPhotoFileLocation);
        tl_set_l(f->loc, TLN_id, j->md.id);
        tl_set_l(f->loc, TLN_access_hash, j->md.access_hash);
        tl_set_b(f->loc, TLN_file_reference, mj_arena_strdup(a, (const char *)j->md.file_ref, j->md.file_ref_len),
                 j->md.file_ref_len);
        tl_set_s(f->loc, TLN_thumb_size, mj_arena_strdup(a, j->md.thumb, strlen(j->md.thumb)));
    } else {
        f->dc = j->md.dc;
        f->loc = tl_new(a, TLC_inputDocumentFileLocation);
        tl_set_l(f->loc, TLN_id, j->md.id);
        tl_set_l(f->loc, TLN_access_hash, j->md.access_hash);
        tl_set_b(f->loc, TLN_file_reference, mj_arena_strdup(a, (const char *)j->md.file_ref, j->md.file_ref_len),
                 j->md.file_ref_len);
        const char *ts = j->i == MJ_FETCH_THUMB ? j->md.thumb_small : "";
        tl_set_s(f->loc, TLN_thumb_size, mj_arena_strdup(a, ts, strlen(ts)));
        if (j->i == MJ_FETCH_DOC) {
            f->size = j->md.size;
            f->big = true;
            mkdir(A.cfg.dl_dir, 0755);
        }
    }
    if (f->dc < 1 || f->dc > MJ_DC_MAX) f->dc = A.home;
    if (!f->loc) {
        fetch_free(f);
        return;
    }
    if (f->big) {
        mj_store_lock();
        mj_S.download_pct = 0;
        mj_S.media_ver++;
        mj_store_unlock();
    }
    /* documents to the end, the small ones first */
    fetch_t **l = &A.fq;
    if (f->big)
        while (*l) l = &(*l)->next;
    f->next = *l;
    *l = f;
    A.fetch_n++;
}

/* The cache's ceiling: past it the oldest files go, down to three quarters.
 * mode 0 checks the ceiling, 1 empties the cache, 2 only counts. */
typedef struct {
    char name[40];
    time_t t;
    off_t size;
} centry_t;

static int cmp_age(const void *a, const void *b)
{
    const centry_t *x = (const centry_t *)a, *y = (const centry_t *)b;
    return x->t < y->t ? -1 : x->t > y->t;
}

static void cache_scan(int mode, uint32_t *files, uint32_t *bytes)
{
    char dir[200];
    snprintf(dir, sizeof dir, "%s/cache", A.cfg.data_dir);
    DIR *d = opendir(dir);
    if (!d) return;
    int cap = 256, n = 0;
    centry_t *v = (centry_t *)mj_malloc((size_t)cap * sizeof *v);
    uint64_t total = 0;
    struct dirent *e;
    while (v && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof v[0].name) continue;
        char p[320];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (mode == 1) {
            unlink(p);
            continue;
        }
        if (n == cap) {
            centry_t *nv = (centry_t *)mj_realloc(v, (size_t)cap * 2 * sizeof *v);
            if (!nv) break;
            v = nv;
            cap *= 2;
        }
        scpy(v[n].name, sizeof v[n].name, e->d_name);
        v[n].t = st.st_mtime;
        v[n].size = st.st_size;
        total += (uint64_t)st.st_size;
        n++;
    }
    closedir(d);
    if (files) *files = (uint32_t)n;
    if (bytes) *bytes = (uint32_t)total;
    if (mode == 0 && v && A.cfg.cache_max && total > A.cfg.cache_max) {
        qsort(v, (size_t)n, sizeof *v, cmp_age);
        int gone = 0;
        for (int i = 0; i < n && total > A.cfg.cache_max * 3 / 4; i++) {
            char p[320];
            snprintf(p, sizeof p, "%s/%s", dir, v[i].name);
            if (unlink(p) == 0) {
                total -= (uint64_t)v[i].size;
                gone++;
            }
        }
        mj_log("api: the cache passed its ceiling: %d files out, %u KB left", gone, (unsigned)(total / 1024));
    }
    mj_free(v);
}

/* ---------------------------------------------------------- signing in */

static void after_login(const tl_obj_t *user)
{
    A.logged = true;
    mj_pref_set_i32("msj_logged", 1);
    A.dcok |= 1u << A.home;
    mj_pref_set_i32("msj_dcok", (int32_t)A.dcok);
    if (user) {
        mj_store_lock();
        mj_peer_t *p = mj_store_put_user(user);
        if (p) {
            mj_S.self = p->key;
            char id[24];
            snprintf(id, sizeof id, "%lld", (long long)p->key.id);
            mj_pref_set_str("msj_self", id);
        }
        mj_store_unlock();
    }
    set_auth(MJ_AUTH_READY, "");
    mj_arena_t a = {0};
    call(tl_new(&a, TLC_updates_getState), state_cb, NULL);
    mj_arena_free(&a);
    load_dialogs(true);
}

static void signed_out(bool drop_keys)
{
    A.logged = false;
    mj_pref_erase("msj_logged");
    mj_pref_erase("msj_self");
    A.pts = 0;
    char p[200];
    state_path(p, sizeof p);
    unlink(p);
    A.dcok = 0;
    mj_pref_set_i32("msj_dcok", 0);
    if (drop_keys)
        for (int dc = 1; dc <= MJ_DC_MAX; dc++)
            if (mj_mtp_has_key(A.mtp, dc)) {
                mj_mtp_drop_key(A.mtp, dc);
                save_key(NULL, dc, NULL);
            }
    mj_store_lock();
    mj_S.ndialogs = 0;
    mj_S.dialogs_end = false;
    mj_S.self.id = 0;
    mj_chat_clear();
    mj_S.dialogs_ver++;
    mj_store_toast(MJ_T_LOGGED_OUT, NULL);
    mj_store_unlock();
    A.chat.type = 0;
    set_auth(A.api_id ? MJ_AUTH_PHONE : MJ_AUTH_NEED_API, "");
}

static void auth_result(const tl_obj_t *o)
{
    if (o->id == TLC_auth_authorization) {
        mj_log("api: signed in");
        after_login(tl_o(o, TLN_user));
    } else if (o->id == TLC_auth_authorizationSignUpRequired) {
        set_auth(MJ_AUTH_SIGNUP, "");
    }
}

static void send_code(void);

static const char *code_how(const tl_obj_t *t)
{
    if (!t) return "";
    switch (t->id) {
    case TLC_auth_sentCodeTypeApp: return "app";
    case TLC_auth_sentCodeTypeSms:
    case TLC_auth_sentCodeTypeSmsWord:
    case TLC_auth_sentCodeTypeSmsPhrase:
    case TLC_auth_sentCodeTypeFirebaseSms: return "sms";
    case TLC_auth_sentCodeTypeCall: return "call";
    case TLC_auth_sentCodeTypeFlashCall:
    case TLC_auth_sentCodeTypeMissedCall: return "missed";
    case TLC_auth_sentCodeTypeEmailCode: return "email";
    case TLC_auth_sentCodeTypeFragmentSms: return "fragment";
    case TLC_auth_sentCodeTypeSetUpEmailRequired: return "setup_email";
    }
    return "";
}

static void code_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    if (r->code == 303) {
        int dc = mj_err_num(r->msg, "PHONE_MIGRATE_");
        if (dc < 0) dc = mj_err_num(r->msg, "NETWORK_MIGRATE_");
        if (dc >= 1 && dc <= MJ_DC_MAX) {
            mj_log("api: %s: the number lives at dc%d", r->msg, dc);
            set_home(dc);
            send_code();
            return;
        }
    }
    tl_obj_t *o = obj(r);
    if (!o) {
        set_auth(MJ_AUTH_PHONE, r->msg ? r->msg : "?");
        return;
    }
    if (o->id == TLC_auth_sentCodeSuccess) {
        auth_result(tl_o(o, TLN_authorization));
        return;
    }
    if (o->id != TLC_auth_sentCode) {
        set_auth(MJ_AUTH_PHONE, tl_name(o->id));
        return;
    }
    scpy(A.code_hash, sizeof A.code_hash, tl_s(o, TLN_phone_code_hash));
    tl_obj_t *t = tl_o(o, TLN_type);
    const char *how = code_how(t);
    mj_store_lock();
    scpy(mj_S.code_how, sizeof mj_S.code_how, how);
    mj_S.code_len = tl_i(t, TLN_length);
    scpy(mj_S.phone, sizeof mj_S.phone, A.phone);
    mj_store_unlock();
    if (!strcmp(how, "setup_email")) set_auth(MJ_AUTH_PHONE, "EMAIL_SETUP_REQUIRED");
    else set_auth(MJ_AUTH_CODE, "");
    mj_log("api: the code went by %s", how);
}

static void send_code(void)
{
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_auth_sendCode);
    tl_set_s(q, TLN_phone_number, A.phone);
    tl_set_i(q, TLN_api_id, A.api_id);
    tl_set_s(q, TLN_api_hash, A.api_hash);
    tl_set_o(q, TLN_settings, tl_new(&a, TLC_codeSettings));
    if (!call(q, code_cb, NULL)) set_auth(MJ_AUTH_PHONE, "INTERNAL");
    mj_arena_free(&a);
}

static void password_info_cb(void *ctx, const mj_rpc_t *r);

static void get_password(void)
{
    mj_arena_t a = {0};
    A.pw_ready = false;
    call(tl_new(&a, TLC_account_getPassword), password_info_cb, NULL);
    mj_arena_free(&a);
}

static void check_password(const char *pw);

static void password_info_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    tl_obj_t *o = obj(r);
    if (!o || o->id != TLC_account_password) {
        set_auth(MJ_AUTH_PASSWORD, r->msg ? r->msg : "?");
        return;
    }
    tl_obj_t *algo = tl_o(o, TLN_current_algo);
    if (!algo || algo->id != TLC_passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow) {
        set_auth(MJ_AUTH_PASSWORD, "UNKNOWN_ALGO");
        return;
    }
    uint32_t n;
    const uint8_t *b;
    b = tl_b(algo, TLN_salt1, &n);
    A.s1n = n <= sizeof A.salt1 ? n : 0;
    if (b && A.s1n) memcpy(A.salt1, b, A.s1n);
    b = tl_b(algo, TLN_salt2, &n);
    A.s2n = n <= sizeof A.salt2 ? n : 0;
    if (b && A.s2n) memcpy(A.salt2, b, A.s2n);
    A.srp_g = tl_i(algo, TLN_g);
    memset(A.srp_p, 0, 256);
    b = tl_b(algo, TLN_p, &n);
    if (b && n <= 256) memcpy(A.srp_p + 256 - n, b, n);
    memset(A.srp_B, 0, 256);
    b = tl_b(o, TLN_srp_B, &n);
    if (b && n <= 256) memcpy(A.srp_B + 256 - n, b, n);
    A.srp_id = tl_l(o, TLN_srp_id);
    A.pw_ready = true;
    mj_store_lock();
    scpy(mj_S.pw_hint, sizeof mj_S.pw_hint, tl_s(o, TLN_hint));
    mj_S.auth_ver++;
    mj_store_unlock();
    if (A.pw_wait) {
        char *pw = A.pw_wait;
        A.pw_wait = NULL;
        check_password(pw);
        memset(pw, 0, strlen(pw));
        mj_free(pw);
    } else {
        set_auth(MJ_AUTH_PASSWORD, "");
    }
}

static void signin_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    if (r->code == 401 && r->msg && !strcmp(r->msg, "SESSION_PASSWORD_NEEDED")) {
        mj_log("api: the account has a two-step password");
        get_password();
        return;
    }
    tl_obj_t *o = obj(r);
    if (!o) {
        set_auth(MJ_AUTH_CODE, r->msg ? r->msg : "?");
        return;
    }
    auth_result(o);
}

static void checkpw_cb(void *ctx, const mj_rpc_t *r)
{
    (void)ctx;
    tl_obj_t *o = obj(r);
    if (!o) {
        /* srp_B is good for one try: a fresh one for the next */
        set_auth(MJ_AUTH_PASSWORD, r->msg ? r->msg : "?");
        get_password();
        return;
    }
    auth_result(o);
}

static void check_password(const char *pw)
{
    if (!A.pw_ready) {
        mj_free(A.pw_wait);
        A.pw_wait = mj_strdup(pw);
        get_password();
        return;
    }
    uint8_t a_rand[256], Ab[256], M1[32];
    mj_random(a_rand, sizeof a_rand);
    uint32_t t0 = mj_now_ms();
    bool ok = mj_srp(pw, A.salt1, A.s1n, A.salt2, A.s2n, A.srp_g, A.srp_p, A.srp_B, a_rand, Ab, M1);
    memset(a_rand, 0, sizeof a_rand);
    mj_log("api: SRP in %u ms", (unsigned)(mj_now_ms() - t0));
    A.pw_ready = false;
    if (!ok) {
        set_auth(MJ_AUTH_PASSWORD, "SRP_FAILED");
        get_password();
        return;
    }
    mj_arena_t a = {0};
    tl_obj_t *in = tl_new(&a, TLC_inputCheckPasswordSRP);
    tl_set_l(in, TLN_srp_id, A.srp_id);
    tl_set_b(in, TLN_A, Ab, 256);
    tl_set_b(in, TLN_M1, M1, 32);
    tl_obj_t *q = tl_new(&a, TLC_auth_checkPassword);
    tl_set_o(q, TLN_password, in);
    if (!call(q, checkpw_cb, NULL)) set_auth(MJ_AUTH_PASSWORD, "INTERNAL");
    mj_arena_free(&a);
}

/* ------------------------------------------------------------ the jobs */

static void open_chat(mj_pkey_t k)
{
    A.chat = k;
    A.chat_gen++;
    A.history_busy = false;
    mj_store_lock();
    mj_chat_clear();
    mj_S.chat = k;
    mj_S.chat_ver++;
    mj_store_unlock();
    load_history(false);
}

static void run_job(job_t *j)
{
    switch (j->type) {
    case J_SET_APP:
        A.api_id = j->i;
        scpy(A.api_hash, sizeof A.api_hash, j->s);
        mj_pref_set_i32("msj_apiid", A.api_id);
        mj_pref_set_str("sec_msj_hash", A.api_hash);
        mj_mtp_set_api_id(A.mtp, A.api_id);
        if (!A.logged) set_auth(MJ_AUTH_PHONE, "");
        break;
    case J_PHONE:
        if (!A.api_id) {
            set_auth(MJ_AUTH_NEED_API, "");
            break;
        }
        scpy(A.phone, sizeof A.phone, j->s);
        set_auth(MJ_AUTH_BUSY, "");
        send_code();
        break;
    case J_CODE: {
        set_auth(MJ_AUTH_BUSY, "");
        mj_arena_t a = {0};
        tl_obj_t *q = tl_new(&a, TLC_auth_signIn);
        tl_set_s(q, TLN_phone_number, A.phone);
        tl_set_s(q, TLN_phone_code_hash, A.code_hash);
        tl_set_s(q, TLN_phone_code, j->s);
        if (!call(q, signin_cb, NULL)) set_auth(MJ_AUTH_CODE, "INTERNAL");
        mj_arena_free(&a);
        break;
    }
    case J_PASSWORD:
        set_auth(MJ_AUTH_BUSY, "");
        check_password(j->s);
        memset(j->s, 0, strlen(j->s));
        break;
    case J_RESTART:
        set_auth(A.api_id ? MJ_AUTH_PHONE : MJ_AUTH_NEED_API, "");
        break;
    case J_LOGOUT: {
        mj_arena_t a = {0};
        call(tl_new(&a, TLC_auth_logOut), NULL, NULL);
        mj_arena_free(&a);
        /* the request goes out before the keys are dropped */
        for (int i = 0; i < 20 && mj_mtp_pending(A.mtp, A.home); i++) mj_mtp_poll(A.mtp, 100);
        signed_out(true);
        break;
    }
    case J_MORE_DIALOGS:
        load_dialogs(false);
        break;
    case J_RELOAD_DIALOGS:
        load_dialogs(true);
        break;
    case J_OPEN:
        open_chat(j->k);
        break;
    case J_CLOSE:
        A.chat.type = 0;
        A.chat_gen++;
        mj_store_lock();
        mj_chat_clear();
        mj_S.chat.type = 0;
        mj_store_unlock();
        break;
    case J_MORE_HISTORY:
        load_history(true);
        break;
    case J_MARK_READ:
        mark_read();
        break;
    case J_SEND:
        send_text(j->k, j->s, j->id);
        break;
    case J_PHOTO:
        send_photo(j->k, j->s, j->s2, j->id);
        break;
    case J_EDIT:
        do_edit(j->k, j->id, j->s);
        break;
    case J_DELETE:
        do_delete(j->k, j->id, j->b);
        break;
    case J_FORWARD:
        do_forward(j->k, j->id, j->k2);
        break;
    case J_REACT:
        do_react(j->k, j->id, j->s);
        break;
    case J_TYPING:
        do_typing(j->k);
        break;
    case J_SEARCH:
        do_search(j->s);
        break;
    case J_FETCH:
        fetch_add(j);
        break;
    case J_CACHE_CLEAR:
        cache_scan(1, NULL, NULL);
        break;
    }
}

/* -------------------------------------------------------------- the loop */

static void thread_main(void *arg)
{
    (void)arg;
    char path[200];
    snprintf(path, sizeof path, "%s/cache", A.cfg.data_dir);
    mkdir(A.cfg.data_dir, 0755);
    mkdir(path, 0755);
    load_server();

    mj_mtp_cfg_t mc = {0};
    mc.api_id = A.api_id;
    mc.device_model = A.cfg.device[0] ? A.cfg.device : "P4OS";
    mc.system_version = A.cfg.system[0] ? A.cfg.system : "P4OS";
    mc.app_version = A.cfg.version[0] ? A.cfg.version : "1.0";
    mc.lang_code = A.cfg.lang[0] ? A.cfg.lang : "en";
    if (A.have_srv) {
        mc.host = A.srv_host;
        mc.port = A.srv_port;
        mc.rsa_n = A.srv_rsa;
    }
    mc.on_updates = handle_updates;
    mc.on_new_session = new_session;
    mc.on_key = save_key;
    mc.on_state = net_state;
    mc.stop = &A.stop;
    A.mtp = mj_mtp_new(&mc);
    if (!A.mtp) {
        A.done = true;
        A.running = false;
        return;
    }
    load_keys();
    int32_t v = 0;
    set_home(mj_pref_get_i32("msj_dc", &v) && v >= 1 && v <= MJ_DC_MAX ? v : START_DC);
    if (mj_pref_get_i32("msj_dcok", &v)) A.dcok = (uint32_t)v;
    char self[24];
    int32_t logged = 0;
    mj_pref_get_i32("msj_logged", &logged);
    if (logged && mj_mtp_has_key(A.mtp, A.home) && mj_pref_get_str("msj_self", self, sizeof self)) {
        mj_store_lock();
        mj_S.self.type = MJ_PEER_USER;
        mj_S.self.id = mj_atoll(self);
        mj_store_unlock();
        A.logged = true;
        set_auth(MJ_AUTH_READY, "");
        mj_arena_t a = {0};
        call(tl_new(&a, TLC_updates_getState), state_cb, NULL);
        /* who we are (the name, the photo), with the first request */
        tl_obj_t *q = tl_new(&a, TLC_users_getUsers);
        tl_vec_t *ids = tl_vec_new(&a, 1);
        tl_vset_o(ids, 0, tl_new(&a, TLC_inputUserSelf));
        tl_set_v(q, TLN_id, ids);
        call(q, NULL, NULL);
        mj_arena_free(&a);
        load_dialogs(true);
    } else {
        set_auth(A.api_id ? MJ_AUTH_PHONE : MJ_AUTH_NEED_API, "");
    }
    cache_scan(0, NULL, NULL);
    A.cache_check_ms = A.state_check_ms = mj_now_ms();
    mj_log("api: started (dc%d, %s, %u KB held)", A.home, A.logged ? "signed in" : "not signed in",
           (unsigned)(mj_mem_used() / 1024));

    while (!A.stop) {
        job_t *j;
        int n = 0;
        while (n < 8 && (j = pop())) {
            run_job(j);
            job_free(j);
            n++;
        }
        if (A.logged) fetch_pump();
        mj_mtp_poll(A.mtp, A.jhead ? 5 : 80);
        uint32_t now = mj_now_ms();
        if (A.diff_at_ms && (int32_t)(now - A.diff_at_ms) >= 0) get_difference();
        if (mj_S.typing[0] && (int32_t)(now - mj_S.typing_until_ms) >= 0) {
            mj_store_lock();
            mj_S.typing[0] = 0;
            mj_S.chat_ver++;
            mj_store_unlock();
        }
        if ((uint32_t)(now - A.cache_check_ms) > 60000) {
            A.cache_check_ms = now;
            cache_scan(0, NULL, NULL);
        }
        /* now and then a real request: a session closed from another
         * device is only said in the answer to one (AUTH_KEY_UNREGISTERED) */
        if (A.logged && (uint32_t)(now - A.state_check_ms) > 10 * 60000) {
            A.state_check_ms = now;
            mj_arena_t a = {0};
            call(tl_new(&a, TLC_updates_getState), state_cb, NULL);
            mj_arena_free(&a);
        }
    }
    if (A.logged && A.pts) state_save();
    while (A.fq) fetch_done(A.fq, false);
    if (A.up) upload_end(false);
    mj_mtp_free(A.mtp);
    A.mtp = NULL;
    job_t *j;
    while ((j = pop())) job_free(j);
    mj_free(A.pw_wait);
    A.pw_wait = NULL;
    memset(A.api_hash, 0, sizeof A.api_hash);
    A.running = false;
    A.done = true;
}

bool mj_api_start(const mj_api_cfg_t *cfg)
{
    if (A.running) return true;
    void *lock = A.jlock;
    memset(&A, 0, sizeof A);
    A.jlock = lock ? lock : mj_mutex_new();
    A.cfg = *cfg;
    int32_t id = 0;
    if (mj_pref_get_i32("msj_apiid", &id) && id > 0 && mj_pref_get_str("sec_msj_hash", A.api_hash, sizeof A.api_hash) &&
        A.api_hash[0])
        A.api_id = id;
    else A.api_hash[0] = 0;
    A.running = true;
    if (!mj_thread_start("mensajes", thread_main, NULL, 24576)) {
        A.running = false;
        return false;
    }
    return true;
}

void mj_api_stop(uint32_t wait_ms)
{
    if (!A.running) return;
    A.stop = true;
    uint32_t t0 = mj_now_ms();
    while (!A.done && (uint32_t)(mj_now_ms() - t0) < wait_ms) mj_sleep_ms(20);
    if (!A.done) mj_log("api: the network thread did not stop in %u ms", (unsigned)wait_ms);
}

bool mj_api_running(void)
{
    return A.running;
}

/* ---- the calls of the screen ---- */

static void q1(int type, const char *s)
{
    job_t *j = job(type);
    if (!j) return;
    if (s) j->s = mj_strdup(s);
    push(j);
}

void mj_api_set_app(int32_t api_id, const char *api_hash)
{
    job_t *j = job(J_SET_APP);
    if (!j) return;
    j->i = api_id;
    j->s = mj_strdup(api_hash);
    /* known at once, for the screen's next question */
    A.api_id = api_id;
    push(j);
}

bool mj_api_have_app(void) { return A.api_id > 0; }
void mj_api_login_phone(const char *phone) { q1(J_PHONE, phone); }
void mj_api_login_code(const char *code) { q1(J_CODE, code); }
void mj_api_login_password(const char *pw) { q1(J_PASSWORD, pw); }
void mj_api_login_restart(void) { q1(J_RESTART, NULL); }
void mj_api_logout(void) { q1(J_LOGOUT, NULL); }
void mj_api_more_dialogs(void) { q1(J_MORE_DIALOGS, NULL); }
void mj_api_reload_dialogs(void) { q1(J_RELOAD_DIALOGS, NULL); }
void mj_api_close_chat(void) { q1(J_CLOSE, NULL); }
void mj_api_more_history(void) { q1(J_MORE_HISTORY, NULL); }
void mj_api_mark_read(void) { q1(J_MARK_READ, NULL); }
void mj_api_search(const char *q) { q1(J_SEARCH, q ? q : ""); }
void mj_api_cache_clear(void) { q1(J_CACHE_CLEAR, NULL); }

void mj_api_open_chat(mj_pkey_t k)
{
    job_t *j = job(J_OPEN);
    if (!j) return;
    j->k = k;
    push(j);
}

void mj_api_send_text(mj_pkey_t k, const char *text, int32_t reply_to)
{
    job_t *j = job(J_SEND);
    if (!j) return;
    j->k = k;
    j->s = mj_strdup(text);
    j->id = reply_to;
    push(j);
}

void mj_api_send_photo(mj_pkey_t k, const char *path, const char *caption, int32_t reply_to)
{
    job_t *j = job(J_PHOTO);
    if (!j) return;
    j->k = k;
    j->s = mj_strdup(path);
    j->s2 = caption && *caption ? mj_strdup(caption) : NULL;
    j->id = reply_to;
    push(j);
}

void mj_api_edit(mj_pkey_t k, int32_t id, const char *text)
{
    job_t *j = job(J_EDIT);
    if (!j) return;
    j->k = k;
    j->id = id;
    j->s = mj_strdup(text);
    push(j);
}

void mj_api_delete(mj_pkey_t k, int32_t id, bool for_everyone)
{
    job_t *j = job(J_DELETE);
    if (!j) return;
    j->k = k;
    j->id = id;
    j->b = for_everyone;
    push(j);
}

void mj_api_forward(mj_pkey_t from, int32_t id, mj_pkey_t to)
{
    job_t *j = job(J_FORWARD);
    if (!j) return;
    j->k = from;
    j->id = id;
    j->k2 = to;
    push(j);
}

void mj_api_react(mj_pkey_t k, int32_t id, const char *emoji)
{
    job_t *j = job(J_REACT);
    if (!j) return;
    j->k = k;
    j->id = id;
    j->s = mj_strdup(emoji ? emoji : "");
    push(j);
}

void mj_api_typing(mj_pkey_t k)
{
    job_t *j = job(J_TYPING);
    if (!j) return;
    j->k = k;
    push(j);
}

void mj_api_fetch(int kind, const mj_peer_t *p, const mj_media_t *md, int32_t msg_id)
{
    job_t *j = job(J_FETCH);
    if (!j) return;
    j->i = kind;
    if (p) j->peer = *p;
    if (md) j->md = *md;
    j->id = msg_id;
    push(j);
}

void mj_api_cache_info(uint32_t *files, uint32_t *bytes)
{
    *files = *bytes = 0;
    cache_scan(2, files, bytes);   /* 2: only count */
}
