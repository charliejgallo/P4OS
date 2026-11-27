/*
 * P4OS - Mensajes: the portal's page (apps/mensajes/web/mensajes.js), over
 * the live channel (docs/PORTAL-PAGES.md, "Live data").
 *
 * While a page looks (it asked something in the last few seconds), the app
 * puts three JSON values:
 *
 *   state  {auth, net, me, unread, have_app}
 *   chats  the list: [{k:"u:123", name, text, time, unread, pinned, out,
 *          read, photo}] (photo: the avatar's path on the card, for <img>)
 *   chat   the open chat: {k, name, status, msgs: [{id, out, from, text,
 *          date, media, photo, reactions, edited}]}
 *
 * and takes its lines (1 KB each at most):
 *
 *   open u:123          opens that chat, here and on the board's screen
 *   send u:123 <text>   sends
 *   app <id> <hash>     the api_id and api_hash, typed in the browser
 *
 * The messages never touch the card on their way to the browser; the
 * pictures do, as the cache's files they already are.
 */
#include "mj_ui.h"

#define WEB_LOOK_MS 5000

static uint32_t s_dialogs_ver, s_chat_ver, s_state_ms, s_media_ver;
static bool s_put;

static void jstr(mj_buf_t *b, const char *s)
{
    mj_buf_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char e[8];
        if (*p == '"' || *p == '\\') {
            e[0] = '\\';
            e[1] = (char)*p;
            mj_buf_put(b, e, 2);
        } else if (*p < 0x20) {
            snprintf(e, sizeof e, "\\u%04x", *p);
            mj_buf_put(b, e, 6);
        } else {
            mj_buf_put(b, p, 1);
        }
    }
    mj_buf_put(b, "\"", 1);
}

static void jraw(mj_buf_t *b, const char *s)
{
    mj_buf_put(b, s, strlen(s));
}

static void jint(mj_buf_t *b, long long v)
{
    char t[24];
    snprintf(t, sizeof t, "%lld", v);
    jraw(b, t);
}

static void key_str(mj_pkey_t k, char *out, size_t n)
{
    snprintf(out, n, "%c:%lld", k.type == MJ_PEER_USER ? 'u' : k.type == MJ_PEER_CHAT ? 'c' : 'h', (long long)k.id);
}

static bool key_parse(const char *s, mj_pkey_t *k)
{
    if (!s[0] || s[1] != ':') return false;
    k->type = s[0] == 'u' ? MJ_PEER_USER : s[0] == 'c' ? MJ_PEER_CHAT : s[0] == 'h' ? MJ_PEER_CHANNEL : 0;
    k->id = mj_atoll(s + 2);
    return k->type && k->id;
}

/* a card path as the portal names it: "/data/mensajes/cache/a1.jpg" */
static void card_path(const char *abs, char *out, size_t n)
{
    const char *root = aos_hal_path_sd_root();
    size_t rl = root ? strlen(root) : 0;
    if (rl && !strncmp(abs, root, rl)) abs += rl;
    snprintf(out, n, "%s", abs);
}

static void put(const char *key, mj_buf_t *b)
{
    if (!b->oom && b->n) aos_hal_live_put(MJ_APP_ID, key, "application/json", b->p, b->n);
    mj_buf_free(b);
    s_put = true;
}

static void put_state(void)
{
    mj_buf_t b = {0};
    mj_store_lock();
    jraw(&b, "{\"auth\":");
    jint(&b, mj_S.auth);
    jraw(&b, ",\"net\":");
    jint(&b, mj_S.net);
    jraw(&b, ",\"unread\":");
    jint(&b, mj_S.unread_total);
    jraw(&b, ",\"have_app\":");
    jraw(&b, mj_api_have_app() ? "true" : "false");
    jraw(&b, ",\"me\":");
    jstr(&b, mj_S.self.id ? mj_peer_name(mj_S.self) : "");
    jraw(&b, ",\"open\":");
    char k[32];
    key_str(U.open, k, sizeof k);
    jstr(&b, U.open.type ? k : "");
    jraw(&b, "}");
    mj_store_unlock();
    put("state", &b);
}

static void put_chats(void)
{
    mj_buf_t b = {0};
    jraw(&b, "[");
    mj_store_lock();
    for (int i = 0; i < mj_S.ndialogs && i < 80; i++) {
        mj_dialog_t *d = &mj_S.dialogs[i];
        mj_peer_t *p = mj_store_peer(d->key);
        char k[32], line[160], photo[200] = "";
        key_str(d->key, k, sizeof k);
        snprintf(line, sizeof line, "%s%s%s", d->who, d->who[0] ? ": " : "", d->preview);
        if (p && p->photo_id) {
            char abs[176];
            mj_api_path(MJ_FETCH_AVATAR, p, NULL, 0, abs, sizeof abs);
            if (mj_file_exists(abs)) card_path(abs, photo, sizeof photo);
        }
        if (i) jraw(&b, ",");
        jraw(&b, "{\"k\":");
        jstr(&b, k);
        jraw(&b, ",\"name\":");
        jstr(&b, p && p->self ? _("Mensajes guardados") : mj_peer_name(d->key));
        jraw(&b, ",\"text\":");
        jstr(&b, line);
        jraw(&b, ",\"kind\":");
        jint(&b, d->preview_kind);
        jraw(&b, ",\"time\":");
        jint(&b, d->date);
        jraw(&b, ",\"unread\":");
        jint(&b, d->unread);
        jraw(&b, d->pinned ? ",\"pinned\":true" : ",\"pinned\":false");
        jraw(&b, d->preview_out ? ",\"out\":true" : ",\"out\":false");
        jraw(&b, d->preview_out && d->read_out_max >= d->top_id ? ",\"read\":true" : ",\"read\":false");
        jraw(&b, ",\"photo\":");
        jstr(&b, photo);
        jraw(&b, "}");
    }
    mj_store_unlock();
    jraw(&b, "]");
    put("chats", &b);
}

static void put_chat(void)
{
    mj_buf_t b = {0};
    mj_store_lock();
    char k[32], st[96];
    key_str(mj_S.chat, k, sizeof k);
    mj_dialog_t *d = mj_store_dialog(mj_S.chat);
    int32_t read_out = d ? d->read_out_max : 0;
    jraw(&b, "{\"k\":");
    jstr(&b, mj_S.chat.type ? k : "");
    jraw(&b, ",\"name\":");
    jstr(&b, mj_S.chat.type ? mj_peer_name(mj_S.chat) : "");
    mj_store_unlock();
    mj_status_line(U.open, st, sizeof st);
    mj_store_lock();
    jraw(&b, ",\"status\":");
    jstr(&b, st);
    jraw(&b, ",\"msgs\":[");
    int from = mj_S.nmsgs > 60 ? mj_S.nmsgs - 60 : 0;
    for (int i = from; i < mj_S.nmsgs; i++) {
        mj_msg_t *m = mj_S.msgs[i];
        char media[120] = "", photo[200] = "";
        if (m->media) {
            mj_media_words(m, media, sizeof media);
            if (m->media->kind == MJ_MEDIA_PHOTO && !m->pending) {
                char abs[176];
                mj_api_path(MJ_FETCH_PHOTO, NULL, m->media, m->id, abs, sizeof abs);
                if (mj_file_exists(abs)) card_path(abs, photo, sizeof photo);
            }
        }
        if (m->service) {
            mj_action_words(m, mj_peer_name(m->from), media, sizeof media);
        }
        if (i > from) jraw(&b, ",");
        jraw(&b, "{\"id\":");
        jint(&b, m->id);
        jraw(&b, m->out ? ",\"out\":true" : ",\"out\":false");
        jraw(&b, m->service ? ",\"service\":true" : "");
        jraw(&b, m->out && m->id <= read_out ? ",\"read\":true" : "");
        jraw(&b, m->pending ? ",\"pending\":true" : "");
        jraw(&b, ",\"from\":");
        jstr(&b, mj_peer_name(m->from));
        jraw(&b, ",\"text\":");
        jstr(&b, m->text);
        jraw(&b, ",\"date\":");
        jint(&b, m->date);
        jraw(&b, ",\"media\":");
        jstr(&b, media);
        jraw(&b, ",\"photo\":");
        jstr(&b, photo);
        jraw(&b, ",\"reactions\":");
        jstr(&b, m->reactions);
        jraw(&b, m->edit_date ? ",\"edited\":true" : "");
        jraw(&b, m->fwd_from[0] ? ",\"fwd\":" : "");
        if (m->fwd_from[0]) jstr(&b, m->fwd_from);
        jraw(&b, "}");
    }
    mj_store_unlock();
    jraw(&b, "]}");
    put("chat", &b);
}

static void take(void)
{
    char msg[1100];
    int n;
    while ((n = aos_hal_live_take(MJ_APP_ID, msg, sizeof msg - 1)) > 0) {
        msg[n] = 0;
        mj_pkey_t k;
        if (!strncmp(msg, "open ", 5) && key_parse(msg + 5, &k)) {
            mj_ui_open_chat(k);
        } else if (!strncmp(msg, "send ", 5)) {
            char *sp = strchr(msg + 5, ' ');
            if (sp && key_parse(msg + 5, &k) && sp[1]) {
                *sp = 0;
                mj_api_send_text(k, sp + 1, 0);
                if (mj_pkey_eq(k, U.open)) U.want_bottom = true;
            }
        } else if (!strncmp(msg, "app ", 4)) {
            int id = atoi(msg + 4);
            const char *h = strchr(msg + 4, ' ');
            if (id > 0 && h && strlen(h + 1) == 32) {
                mj_api_set_app(id, h + 1);
                aos_ui_toast(_("Se guardaron el api_id y el api_hash"), 2500);
            }
            memset(msg, 0, sizeof msg);
        }
    }
}

void mj_web_tick(void)
{
    take();
    uint32_t idle = aos_hal_live_idle_ms(MJ_APP_ID);
    if (idle > WEB_LOOK_MS) return;
    uint32_t now = mj_now_ms();
    mj_store_lock();
    uint32_t vd = mj_S.dialogs_ver + mj_S.peers_ver, vc = mj_S.chat_ver, vm = mj_S.media_ver;
    mj_store_unlock();
    if ((uint32_t)(now - s_state_ms) > 1000) {
        s_state_ms = now;
        put_state();
    }
    if (vd != s_dialogs_ver || vm != s_media_ver) {
        s_dialogs_ver = vd;
        put_chats();
    }
    if (vc != s_chat_ver || vm != s_media_ver) {
        s_chat_ver = vc;
        put_chat();
    }
    s_media_ver = vm;
}

void mj_web_stop(void)
{
    if (s_put) aos_hal_live_clear(MJ_APP_ID);
    s_put = false;
    s_dialogs_ver = s_chat_ver = s_media_ver = 0;
    s_state_ms = 0;
}
