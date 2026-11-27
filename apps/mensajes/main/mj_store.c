/*
 * P4OS - Mensajes: the store (mj_store.h).
 */
#include "mj_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mj_port.h"

mj_store_t mj_S;
static void *s_lock;

void mj_store_init(void)
{
    memset(&mj_S, 0, sizeof mj_S);
    mj_S.download_pct = -1;
    if (!s_lock) s_lock = mj_mutex_new();
}

static void msgs_free(mj_msg_t **v, int n)
{
    for (int i = 0; i < n; i++) mj_msg_free(v[i]);
}

void mj_store_free(void)
{
    msgs_free(mj_S.msgs, mj_S.nmsgs);
    mj_free(mj_S.msgs);
    msgs_free(mj_S.found, mj_S.nfound);
    mj_free(mj_S.found);
    mj_free(mj_S.found_peers);
    mj_free(mj_S.peers);
    mj_free(mj_S.dialogs);
    memset(&mj_S, 0, sizeof mj_S);
}

void mj_store_lock(void)
{
    mj_mutex_lock(s_lock);
}

void mj_store_unlock(void)
{
    mj_mutex_unlock(s_lock);
}

static void scpy(char *d, size_t n, const char *s)
{
    if (!n) return;
    size_t l = s ? strlen(s) : 0;
    if (l >= n) {
        l = n - 1;
        /* not in the middle of a UTF-8 sequence */
        while (l > 0 && ((unsigned char)s[l] & 0xC0) == 0x80) l--;
    }
    if (l) memcpy(d, s, l);
    d[l] = 0;
}

/* ---- peers ---- */

mj_pkey_t mj_peer_of(const tl_obj_t *p)
{
    mj_pkey_t k = {MJ_PEER_NONE, 0};
    if (!p) return k;
    switch (p->id) {
    case TLC_peerUser:
        k.type = MJ_PEER_USER;
        k.id = tl_l(p, TLN_user_id);
        break;
    case TLC_peerChat:
        k.type = MJ_PEER_CHAT;
        k.id = tl_l(p, TLN_chat_id);
        break;
    case TLC_peerChannel:
        k.type = MJ_PEER_CHANNEL;
        k.id = tl_l(p, TLN_channel_id);
        break;
    }
    return k;
}

mj_peer_t *mj_store_peer(mj_pkey_t k)
{
    /* kept sorted by (type, id): a binary search */
    int lo = 0, hi = mj_S.npeers - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        mj_peer_t *p = &mj_S.peers[mid];
        if (p->key.type == k.type && p->key.id == k.id) return p;
        if (p->key.type < k.type || (p->key.type == k.type && p->key.id < k.id)) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static mj_peer_t *peer_add(mj_pkey_t k)
{
    mj_peer_t *p = mj_store_peer(k);
    if (p) return p;
    if (mj_S.npeers == mj_S.peers_cap) {
        int cap = mj_S.peers_cap ? mj_S.peers_cap * 2 : 128;
        mj_peer_t *n = (mj_peer_t *)mj_realloc(mj_S.peers, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        mj_S.peers = n;
        mj_S.peers_cap = cap;
    }
    int i = mj_S.npeers;
    while (i > 0) {
        mj_peer_t *q = &mj_S.peers[i - 1];
        if (q->key.type < k.type || (q->key.type == k.type && q->key.id < k.id)) break;
        i--;
    }
    memmove(&mj_S.peers[i + 1], &mj_S.peers[i], (size_t)(mj_S.npeers - i) * sizeof(mj_peer_t));
    mj_S.npeers++;
    p = &mj_S.peers[i];
    memset(p, 0, sizeof *p);
    p->key = k;
    return p;
}

mj_peer_t *mj_store_put_user(const tl_obj_t *u)
{
    if (!u || (u->id != TLC_user && u->id != TLC_userEmpty)) return NULL;
    mj_pkey_t k = {MJ_PEER_USER, tl_l(u, TLN_id)};
    mj_peer_t *p = peer_add(k);
    if (!p || u->id == TLC_userEmpty) return p;
    bool min = tl_t(u, TLN_min);
    /* a "min" user carries what a group showed of him: not his hash, maybe
     * not his name; what we knew stays */
    if (tl_has(u, TLN_access_hash) && (!min || !p->have_hash)) {
        p->access_hash = tl_l(u, TLN_access_hash);
        p->have_hash = !min;
    }
    const char *fn = tl_s(u, TLN_first_name), *ln = tl_s(u, TLN_last_name);
    if (!min || !p->name[0]) {
        if (*fn || *ln) snprintf(p->name, sizeof p->name, "%s%s%s", fn, (*fn && *ln) ? " " : "", ln);
        scpy(p->first, sizeof p->first, *fn ? fn : ln);
        if (tl_has(u, TLN_username)) scpy(p->username, sizeof p->username, tl_s(u, TLN_username));
        if (tl_has(u, TLN_phone)) scpy(p->phone, sizeof p->phone, tl_s(u, TLN_phone));
    }
    p->self = tl_t(u, TLN_self);
    p->bot = tl_t(u, TLN_bot);
    p->deleted = tl_t(u, TLN_deleted);
    tl_obj_t *ph = tl_o(u, TLN_photo);
    if (ph && ph->id == TLC_userProfilePhoto) {
        p->photo_id = tl_l(ph, TLN_photo_id);
        p->photo_dc = tl_i(ph, TLN_dc_id);
    } else if (ph || !min) {
        p->photo_id = 0;
    }
    tl_obj_t *st = tl_o(u, TLN_status);
    if (st) {
        if (st->id == TLC_userStatusOnline) p->online_until = tl_i(st, TLN_expires);
        else if (st->id == TLC_userStatusOffline) {
            p->online_until = 0;
            p->last_seen = tl_i(st, TLN_was_online);
        } else {
            p->online_until = 0;
        }
    }
    if (p->self) mj_S.self = k;
    mj_S.peers_ver++;
    return p;
}

mj_peer_t *mj_store_put_chat(const tl_obj_t *c)
{
    if (!c) return NULL;
    mj_pkey_t k;
    if (c->id == TLC_chat || c->id == TLC_chatForbidden || c->id == TLC_chatEmpty) k.type = MJ_PEER_CHAT;
    else if (c->id == TLC_channel || c->id == TLC_channelForbidden) k.type = MJ_PEER_CHANNEL;
    else return NULL;
    k.id = tl_l(c, TLN_id);
    mj_peer_t *p = peer_add(k);
    if (!p) return NULL;
    bool min = tl_t(c, TLN_min);
    if (tl_has(c, TLN_access_hash) && (!min || !p->have_hash)) {
        p->access_hash = tl_l(c, TLN_access_hash);
        p->have_hash = !min;
    }
    if (tl_has(c, TLN_title)) scpy(p->name, sizeof p->name, tl_s(c, TLN_title));
    if (tl_has(c, TLN_username)) scpy(p->username, sizeof p->username, tl_s(c, TLN_username));
    p->broadcast = tl_t(c, TLN_broadcast);
    p->megagroup = tl_t(c, TLN_megagroup);
    tl_obj_t *ph = tl_o(c, TLN_photo);
    if (ph && ph->id == TLC_chatPhoto) {
        p->photo_id = tl_l(ph, TLN_photo_id);
        p->photo_dc = tl_i(ph, TLN_dc_id);
    } else if (ph) {
        p->photo_id = 0;
    }
    mj_S.peers_ver++;
    return p;
}

void mj_store_put_users(const tl_vec_t *v)
{
    for (uint32_t i = 0; i < tl_vn(v); i++) mj_store_put_user(tl_vo(v, i));
}

void mj_store_put_chats(const tl_vec_t *v)
{
    for (uint32_t i = 0; i < tl_vn(v); i++) mj_store_put_chat(tl_vo(v, i));
}

const char *mj_peer_name(mj_pkey_t k)
{
    mj_peer_t *p = mj_store_peer(k);
    if (!p) return "?";
    if (p->name[0]) return p->name;
    if (p->username[0]) return p->username;
    return p->deleted ? "-" : "?";
}

/* ---- messages ---- */

static const tl_obj_t *best_size(const tl_vec_t *sizes, int maxw, int *w, int *h, char *type)
{
    /* the largest photoSize/Progressive not wider than maxw, else the smallest */
    const tl_obj_t *best = NULL, *small = NULL;
    int bw = 0, sw = 1 << 30;
    for (uint32_t i = 0; i < tl_vn(sizes); i++) {
        const tl_obj_t *s = tl_vo(sizes, i);
        if (!s || (s->id != TLC_photoSize && s->id != TLC_photoSizeProgressive)) continue;
        int ww = tl_i(s, TLN_w), hh = tl_i(s, TLN_h);
        int big = ww > hh ? ww : hh;
        if (big <= maxw && big > bw) {
            best = s;
            bw = big;
        }
        if (big < sw) {
            small = s;
            sw = big;
        }
    }
    if (!best) best = small;
    if (best) {
        *w = tl_i(best, TLN_w);
        *h = tl_i(best, TLN_h);
        scpy(type, 4, tl_s(best, TLN_type));
    }
    return best;
}

static void file_ref(mj_media_t *md, const tl_obj_t *o)
{
    uint32_t n = 0;
    const uint8_t *fr = tl_b(o, TLN_file_reference, &n);
    if (n > sizeof md->file_ref) n = 0;
    if (fr && n) memcpy(md->file_ref, fr, n);
    md->file_ref_len = (uint8_t)n;
}

static mj_media_t *media_from_tl(const tl_obj_t *mm, char *text_extra, size_t tn)
{
    if (!mm || mm->id == TLC_messageMediaEmpty) return NULL;
    mj_media_t *md = (mj_media_t *)mj_calloc(sizeof *md);
    if (!md) return NULL;
    md->kind = MJ_MEDIA_OTHER;
    switch (mm->id) {
    case TLC_messageMediaPhoto: {
        tl_obj_t *ph = tl_o(mm, TLN_photo);
        if (!ph || ph->id != TLC_photo) break;
        md->kind = MJ_MEDIA_PHOTO;
        md->id = tl_l(ph, TLN_id);
        md->access_hash = tl_l(ph, TLN_access_hash);
        md->dc = tl_i(ph, TLN_dc_id);
        file_ref(md, ph);
        best_size(tl_v(ph, TLN_sizes), 800, &md->w, &md->h, md->thumb);
        break;
    }
    case TLC_messageMediaDocument: {
        tl_obj_t *doc = tl_o(mm, TLN_document);
        if (!doc || doc->id != TLC_document) break;
        md->kind = MJ_MEDIA_DOC;
        md->id = tl_l(doc, TLN_id);
        md->access_hash = tl_l(doc, TLN_access_hash);
        md->dc = tl_i(doc, TLN_dc_id);
        md->size = tl_l(doc, TLN_size);
        file_ref(md, doc);
        scpy(md->mime, sizeof md->mime, tl_s(doc, TLN_mime_type));
        tl_vec_t *th = tl_v(doc, TLN_thumbs);
        if (th) best_size(th, 320, &md->thumb_w, &md->thumb_h, md->thumb_small);
        tl_vec_t *at = tl_v(doc, TLN_attributes);
        for (uint32_t i = 0; i < tl_vn(at); i++) {
            tl_obj_t *a = tl_vo(at, i);
            if (!a) continue;
            switch (a->id) {
            case TLC_documentAttributeFilename:
                scpy(md->name, sizeof md->name, tl_s(a, TLN_file_name));
                break;
            case TLC_documentAttributeAudio:
                md->kind = tl_t(a, TLN_voice) ? MJ_MEDIA_VOICE : MJ_MEDIA_AUDIO;
                md->duration = tl_i(a, TLN_duration);
                if (tl_has(a, TLN_title)) scpy(md->title, sizeof md->title, tl_s(a, TLN_title));
                break;
            case TLC_documentAttributeVideo:
                md->kind = tl_t(a, TLN_round_message) ? MJ_MEDIA_ROUND : MJ_MEDIA_VIDEO;
                md->duration = (int)tl_d(a, TLN_duration);
                md->w = tl_i(a, TLN_w);
                md->h = tl_i(a, TLN_h);
                break;
            case TLC_documentAttributeAnimated:
                md->kind = MJ_MEDIA_GIF;
                break;
            case TLC_documentAttributeSticker:
                md->kind = MJ_MEDIA_STICKER;
                scpy(md->emoji, sizeof md->emoji, tl_s(a, TLN_alt));
                break;
            case TLC_documentAttributeImageSize:
                md->w = tl_i(a, TLN_w);
                md->h = tl_i(a, TLN_h);
                break;
            }
        }
        if (tl_t(mm, TLN_voice)) md->kind = MJ_MEDIA_VOICE;
        break;
    }
    case TLC_messageMediaGeo:
    case TLC_messageMediaVenue:
    case TLC_messageMediaGeoLive:
        md->kind = MJ_MEDIA_GEO;
        if (tl_has(mm, TLN_title)) scpy(md->title, sizeof md->title, tl_s(mm, TLN_title));
        break;
    case TLC_messageMediaContact:
        md->kind = MJ_MEDIA_CONTACT;
        snprintf(md->title, sizeof md->title, "%s %s", tl_s(mm, TLN_first_name), tl_s(mm, TLN_last_name));
        scpy(md->name, sizeof md->name, tl_s(mm, TLN_phone_number));
        break;
    case TLC_messageMediaPoll: {
        md->kind = MJ_MEDIA_POLL;
        tl_obj_t *poll = tl_o(mm, TLN_poll);
        tl_obj_t *q = poll ? tl_o(poll, TLN_question) : NULL;
        if (q) scpy(md->title, sizeof md->title, tl_s(q, TLN_text));
        break;
    }
    case TLC_messageMediaWebPage: {
        md->kind = MJ_MEDIA_WEB;
        tl_obj_t *wp = tl_o(mm, TLN_webpage);
        if (wp && wp->id == TLC_webPage) {
            scpy(md->title, sizeof md->title, tl_s(wp, TLN_title));
            scpy(md->name, sizeof md->name, tl_s(wp, TLN_site_name));
        }
        break;
    }
    }
    (void)text_extra;
    (void)tn;
    return md;
}

void mj_msg_set_reactions(mj_msg_t *m, const tl_obj_t *r)
{
    m->reactions[0] = 0;
    m->my_reaction[0] = 0;
    if (!r) return;
    tl_vec_t *res = tl_v(r, TLN_results);
    size_t at = 0;
    for (uint32_t i = 0; i < tl_vn(res) && at + 24 < sizeof m->reactions; i++) {
        tl_obj_t *rc = tl_vo(res, i);
        tl_obj_t *re = rc ? tl_o(rc, TLN_reaction) : NULL;
        if (!re) continue;
        const char *e = re->id == TLC_reactionEmoji ? tl_s(re, TLN_emoticon) : "\xE2\xAD\x90";   /* a star for custom ones */
        int n = tl_i(rc, TLN_count);
        int w = snprintf(m->reactions + at, sizeof m->reactions - at, "%s%s %d", at ? "   " : "", e, n);
        if (w > 0) at += (size_t)w;
        if (tl_has(rc, TLN_chosen_order) && re->id == TLC_reactionEmoji) scpy(m->my_reaction, sizeof m->my_reaction, e);
    }
    if (at >= sizeof m->reactions) m->reactions[sizeof m->reactions - 1] = 0;
}

static uint8_t action_of(const tl_obj_t *a, char *arg, size_t an)
{
    if (!a) return MJ_ACT_OTHER;
    arg[0] = 0;
    switch (a->id) {
    case TLC_messageActionChatCreate:
    case TLC_messageActionChannelCreate:
        scpy(arg, an, tl_s(a, TLN_title));
        return MJ_ACT_CREATE;
    case TLC_messageActionChatEditTitle:
        scpy(arg, an, tl_s(a, TLN_title));
        return MJ_ACT_TITLE;
    case TLC_messageActionChatEditPhoto:
    case TLC_messageActionChatDeletePhoto:
        return MJ_ACT_PHOTO;
    case TLC_messageActionChatAddUser: {
        tl_vec_t *u = tl_v(a, TLN_users);
        if (tl_vn(u)) {
            mj_pkey_t k = {MJ_PEER_USER, tl_vl(u, 0)};
            scpy(arg, an, mj_peer_name(k));
        }
        return MJ_ACT_ADD;
    }
    case TLC_messageActionChatDeleteUser: {
        mj_pkey_t k = {MJ_PEER_USER, tl_l(a, TLN_user_id)};
        scpy(arg, an, mj_peer_name(k));
        return MJ_ACT_LEAVE;
    }
    case TLC_messageActionChatJoinedByLink:
    case TLC_messageActionChatJoinedByRequest:
        return MJ_ACT_JOIN;
    case TLC_messageActionPinMessage:
        return MJ_ACT_PIN;
    case TLC_messageActionPhoneCall:
    case TLC_messageActionGroupCall:
        return MJ_ACT_CALL;
    }
    return MJ_ACT_OTHER;
}

mj_msg_t *mj_msg_from_tl(const tl_obj_t *t)
{
    if (!t || (t->id != TLC_message && t->id != TLC_messageService)) return NULL;
    mj_msg_t *m = (mj_msg_t *)mj_calloc(sizeof *m);
    if (!m) return NULL;
    m->id = tl_i(t, TLN_id);
    m->date = tl_i(t, TLN_date);
    m->edit_date = tl_i(t, TLN_edit_date);
    m->chat = mj_peer_of(tl_o(t, TLN_peer_id));
    m->out = tl_t(t, TLN_out);
    m->pinned = tl_t(t, TLN_pinned);
    m->mentioned = tl_t(t, TLN_mentioned);
    tl_obj_t *from = tl_o(t, TLN_from_id);
    m->from = from ? mj_peer_of(from) : m->chat;
    if (m->out && mj_S.self.id) m->from = mj_S.self;
    tl_obj_t *rt = tl_o(t, TLN_reply_to);
    if (rt && rt->id == TLC_messageReplyHeader) m->reply_to = tl_i(rt, TLN_reply_to_msg_id);
    m->grouped_id = tl_l(t, TLN_grouped_id);
    m->views = tl_i(t, TLN_views);
    if (t->id == TLC_messageService) {
        m->service = true;
        m->action = action_of(tl_o(t, TLN_action), m->action_arg, sizeof m->action_arg);
        m->text = mj_strdup("");
    } else {
        m->text = mj_strdup(tl_s(t, TLN_message));
        m->media = media_from_tl(tl_o(t, TLN_media), NULL, 0);
        tl_obj_t *fw = tl_o(t, TLN_fwd_from);
        if (fw) {
            if (tl_has(fw, TLN_from_name)) scpy(m->fwd_from, sizeof m->fwd_from, tl_s(fw, TLN_from_name));
            else if (tl_o(fw, TLN_from_id)) scpy(m->fwd_from, sizeof m->fwd_from, mj_peer_name(mj_peer_of(tl_o(fw, TLN_from_id))));
            if (!m->fwd_from[0]) scpy(m->fwd_from, sizeof m->fwd_from, "?");
        }
        mj_msg_set_reactions(m, tl_o(t, TLN_reactions));
    }
    if (!m->text) {
        mj_free(m);
        return NULL;
    }
    return m;
}

void mj_msg_free(mj_msg_t *m)
{
    if (!m) return;
    mj_free(m->text);
    mj_free(m->media);
    mj_free(m);
}

void mj_msg_line(const mj_msg_t *m, char *out, size_t n)
{
    const char *icon = "";
    if (m->media) switch (m->media->kind) {
        case MJ_MEDIA_PHOTO: icon = "\xF0\x9F\x93\xB7"; break;          /* camera */
        case MJ_MEDIA_VOICE: icon = "\xF0\x9F\x8E\xA4"; break;          /* microphone */
        case MJ_MEDIA_AUDIO: icon = "\xF0\x9F\x8E\xB5"; break;          /* note */
        case MJ_MEDIA_VIDEO:
        case MJ_MEDIA_ROUND:
        case MJ_MEDIA_GIF: icon = "\xF0\x9F\x8E\xAC"; break;            /* clapper */
        case MJ_MEDIA_STICKER: icon = m->media->emoji[0] ? m->media->emoji : "\xF0\x9F\x99\x82"; break;
        case MJ_MEDIA_DOC: icon = "\xF0\x9F\x93\x8E"; break;            /* paperclip */
        case MJ_MEDIA_GEO: icon = "\xF0\x9F\x93\x8D"; break;            /* pin */
        case MJ_MEDIA_CONTACT: icon = "\xF0\x9F\x91\xA4"; break;
        case MJ_MEDIA_POLL: icon = "\xF0\x9F\x93\x8A"; break;
        default: break;
        }
    const char *t = m->text;
    if (!*t && m->media) t = m->media->name[0] ? m->media->name : m->media->title;
    snprintf(out, n, "%s%s%s", icon, (*icon && *t) ? " " : "", t);
    for (char *p = out; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
}

/* ---- dialogs ---- */

mj_dialog_t *mj_store_dialog(mj_pkey_t k)
{
    for (int i = 0; i < mj_S.ndialogs; i++)
        if (mj_pkey_eq(mj_S.dialogs[i].key, k)) return &mj_S.dialogs[i];
    return NULL;
}

mj_dialog_t *mj_store_dialog_add(mj_pkey_t k)
{
    mj_dialog_t *d = mj_store_dialog(k);
    if (d) return d;
    if (mj_S.ndialogs == mj_S.dialogs_cap) {
        int cap = mj_S.dialogs_cap ? mj_S.dialogs_cap * 2 : 64;
        mj_dialog_t *n = (mj_dialog_t *)mj_realloc(mj_S.dialogs, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        mj_S.dialogs = n;
        mj_S.dialogs_cap = cap;
    }
    d = &mj_S.dialogs[mj_S.ndialogs++];
    memset(d, 0, sizeof *d);
    d->key = k;
    return d;
}

void mj_store_dialog_top(mj_dialog_t *d, const mj_msg_t *m)
{
    if (!d || !m) return;
    if (m->id < d->top_id && !m->pending) return;
    d->top_id = m->id;
    d->date = m->date;
    d->preview_out = m->out;
    d->preview_kind = m->media ? m->media->kind : MJ_MEDIA_NONE;
    d->preview_action = m->action;
    const char *t = m->text;
    if (!*t && m->media) t = m->media->kind == MJ_MEDIA_STICKER ? m->media->emoji : m->media->name[0] ? m->media->name : m->media->title;
    if (m->service) t = m->action_arg;
    scpy(d->preview, sizeof d->preview, t);
    for (char *p = d->preview; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    d->who[0] = 0;
    if (d->key.type != MJ_PEER_USER && !m->out && m->from.type == MJ_PEER_USER) {
        mj_peer_t *p = mj_store_peer(m->from);
        if (p) scpy(d->who, sizeof d->who, p->first[0] ? p->first : p->name);
    }
}

void mj_store_dialog_raise(mj_pkey_t k)
{
    int i;
    for (i = 0; i < mj_S.ndialogs; i++)
        if (mj_pkey_eq(mj_S.dialogs[i].key, k)) break;
    if (i == mj_S.ndialogs || mj_S.dialogs[i].pinned) return;
    int to = 0;
    while (to < mj_S.ndialogs && mj_S.dialogs[to].pinned) to++;
    if (to >= i) return;
    mj_dialog_t d = mj_S.dialogs[i];
    memmove(&mj_S.dialogs[to + 1], &mj_S.dialogs[to], (size_t)(i - to) * sizeof d);
    mj_S.dialogs[to] = d;
}

void mj_store_unread_count(void)
{
    int n = 0;
    for (int i = 0; i < mj_S.ndialogs; i++)
        if (!mj_S.dialogs[i].muted) n += mj_S.dialogs[i].unread;
    mj_S.unread_total = n;
}

/* ---- the open chat ---- */

void mj_chat_clear(void)
{
    msgs_free(mj_S.msgs, mj_S.nmsgs);
    mj_S.nmsgs = 0;
    mj_S.chat_start = false;
    mj_S.typing[0] = 0;
    mj_S.chat_ver++;
}

static int chat_pos(int32_t id)
{
    /* pending messages (negative ids) sort after the real ones */
    int lo = 0, hi = mj_S.nmsgs;
    uint32_t key = (uint32_t)id;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if ((uint32_t)mj_S.msgs[mid]->id < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void mj_chat_insert(mj_msg_t *m)
{
    int i = chat_pos(m->id);
    if (i < mj_S.nmsgs && mj_S.msgs[i]->id == m->id) {
        mj_msg_free(mj_S.msgs[i]);
        mj_S.msgs[i] = m;
        mj_S.chat_ver++;
        return;
    }
    if (mj_S.nmsgs == mj_S.msgs_cap) {
        int cap = mj_S.msgs_cap ? mj_S.msgs_cap * 2 : 128;
        mj_msg_t **n = (mj_msg_t **)mj_realloc(mj_S.msgs, (size_t)cap * sizeof *n);
        if (!n) {
            mj_msg_free(m);
            return;
        }
        mj_S.msgs = n;
        mj_S.msgs_cap = cap;
    }
    memmove(&mj_S.msgs[i + 1], &mj_S.msgs[i], (size_t)(mj_S.nmsgs - i) * sizeof *mj_S.msgs);
    mj_S.msgs[i] = m;
    mj_S.nmsgs++;
    mj_S.chat_ver++;
}

mj_msg_t *mj_chat_find(int32_t id)
{
    int i = chat_pos(id);
    return i < mj_S.nmsgs && mj_S.msgs[i]->id == id ? mj_S.msgs[i] : NULL;
}

mj_msg_t *mj_chat_find_random(int64_t random_id)
{
    for (int i = mj_S.nmsgs - 1; i >= 0; i--)
        if (mj_S.msgs[i]->pending && mj_S.msgs[i]->random_id == random_id) return mj_S.msgs[i];
    return NULL;
}

void mj_chat_remove(int32_t id)
{
    int i = chat_pos(id);
    if (i < mj_S.nmsgs && mj_S.msgs[i]->id == id) {
        mj_msg_free(mj_S.msgs[i]);
        memmove(&mj_S.msgs[i], &mj_S.msgs[i + 1], (size_t)(mj_S.nmsgs - i - 1) * sizeof *mj_S.msgs);
        mj_S.nmsgs--;
        mj_S.chat_ver++;
    }
}

void mj_store_toast(int code, const char *arg)
{
    mj_S.toast = code;
    scpy(mj_S.toast_arg, sizeof mj_S.toast_arg, arg ? arg : "");
    mj_S.toast_ver++;
}

void mj_store_media_done(const char *path)
{
    if (mj_S.nmedia_done == MJ_MEDIA_DONE_MAX) {
        memmove(mj_S.media_done[0], mj_S.media_done[1], sizeof mj_S.media_done[0] * (MJ_MEDIA_DONE_MAX - 1));
        mj_S.nmedia_done--;
    }
    scpy(mj_S.media_done[mj_S.nmedia_done++], sizeof mj_S.media_done[0], path);
    mj_S.media_ver++;
}
