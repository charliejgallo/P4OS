/*
 * P4OS - Mensajes: the open chat.
 *
 * The bubbles are kept in step with the store one by one, not drawn again
 * whole: each row (a message or a day's label) remembers a signature of
 * what it shows, and on every change of the store only the rows whose
 * signature moved are made again; the rest are moved into place. A chat of
 * a few hundred messages then costs nothing when one more arrives, and the
 * scroll stays where the person left it (at the bottom it follows; older
 * messages loaded above keep the one in view in its place).
 *
 * A long press on a bubble opens its sheet: the reactions, reply, forward,
 * copy, edit and delete. A photo opens the viewer; a document, a voice
 * note or a video downloads to the card (<card>/Mensajes): the board has
 * no Opus decoder an app can use, so voice notes are kept, not played.
 */
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "aos_text_safe.h"
#include "mj_ui.h"

#define BUBBLE_MAX_PCT 80
#define PHOTO_MAX_W    440
#define COMPOSER_H     92

struct mj_row {
    int32_t id;                 /* the message's, or the day (a label row) */
    bool day;
    uint32_t sig;
    lv_obj_t *obj;
    bool wants_media;           /* a picture or a file it waits for         */
};

/* a message as the screen needs it, copied from the store */
typedef struct {
    mj_msg_t m;                 /* text and media are our own copies        */
    mj_media_t md;
    char from_name[MJ_NAME];
    char reply_name[MJ_NAME];
    char reply_text[100];
    bool read, group, first_of_run;
} mv_t;

static mv_t *s_v;
static int s_nv;
static int32_t s_read_out;
static bool s_group, s_broadcast, s_start, s_loading;
static int s_unread;

/* ---- copying the store ---- */

static uint32_t hash_str(uint32_t h, const char *s)
{
    while (s && *s) h = h * 31 + (uint8_t)*s++;
    return h;
}

static void views_free(void)
{
    for (int i = 0; i < s_nv; i++) mj_free(s_v[i].m.text);
    s_nv = 0;
}

static void copy_views(void)
{
    views_free();
    mj_store_lock();
    if (!mj_pkey_eq(mj_S.chat, U.open)) {
        mj_store_unlock();
        return;
    }
    mj_dialog_t *d = mj_store_dialog(U.open);
    s_read_out = d ? d->read_out_max : 0;
    s_unread = d ? d->unread : 0;
    mj_peer_t *p = mj_store_peer(U.open);
    s_broadcast = p && p->broadcast;
    s_group = U.open.type != MJ_PEER_USER && !s_broadcast;
    s_start = mj_S.chat_start;
    s_loading = mj_S.chat_loading;
    mv_t *nv = (mv_t *)mj_realloc(s_v, (size_t)(mj_S.nmsgs + 1) * sizeof *nv);
    if (nv) {
        s_v = nv;
        for (int i = 0; i < mj_S.nmsgs; i++) {
            mj_msg_t *m = mj_S.msgs[i];
            mv_t *v = &s_v[s_nv];
            memset(v, 0, sizeof *v);
            v->m = *m;
            v->m.text = mj_strdup(m->text);
            if (m->media) {
                v->md = *m->media;
                v->m.media = &v->md;
            }
            snprintf(v->from_name, sizeof v->from_name, "%s", mj_peer_name(m->from));
            if (m->reply_to) {
                mj_msg_t *r = mj_chat_find(m->reply_to);
                if (r) {
                    snprintf(v->reply_name, sizeof v->reply_name, "%s", mj_peer_name(r->from));
                    mj_msg_line(r, v->reply_text, sizeof v->reply_text);
                }
            }
            v->read = m->out && m->id > 0 && m->id <= s_read_out;
            v->group = s_group;
            v->first_of_run = !s_nv || !mj_pkey_eq(s_v[s_nv - 1].m.from, m->from) || s_v[s_nv - 1].m.service;
            s_nv++;
        }
    }
    mj_store_unlock();
}

static int32_t day_of(int32_t t)
{
    time_t tt = (time_t)t;
    struct tm a;
    localtime_r(&tt, &a);
    return (a.tm_year + 1900) * 1000 + a.tm_yday;
}

static uint32_t sig_of(const mv_t *v, bool have_file)
{
    uint32_t h = (uint32_t)v->m.id * 2654435761u;
    h = hash_str(h, v->m.text);
    h = hash_str(h, v->m.reactions);
    h = hash_str(h, v->reply_text);
    h = h * 31 + (uint32_t)v->m.edit_date;
    h = h * 31 + (v->m.pending ? 1 : 0) + (v->m.failed ? 2 : 0) + (v->read ? 4 : 0) + (have_file ? 8 : 0) +
        (v->first_of_run ? 16 : 0);
    return h;
}

/* ---- the media of a bubble ---- */

static void path_for(const mv_t *v, int kind, char *out, size_t n)
{
    mj_api_path(kind, NULL, &v->md, v->m.id, out, n);
}

static bool media_ready(const mv_t *v)
{
    if (!v->m.media) return true;
    char p[176];
    switch (v->md.kind) {
    case MJ_MEDIA_PHOTO:
        if (v->m.pending) return true;
        path_for(v, MJ_FETCH_PHOTO, p, sizeof p);
        return mj_file_exists(p);
    case MJ_MEDIA_STICKER:
        if (!v->md.thumb_small[0]) return true;
        path_for(v, MJ_FETCH_THUMB, p, sizeof p);
        return mj_file_exists(p);
    case MJ_MEDIA_VOICE:
    case MJ_MEDIA_AUDIO:
    case MJ_MEDIA_DOC:
    case MJ_MEDIA_VIDEO:
    case MJ_MEDIA_ROUND:
    case MJ_MEDIA_GIF:
        path_for(v, MJ_FETCH_DOC, p, sizeof p);
        return mj_file_exists(p);
    }
    return true;
}

static void viewer_close(lv_event_t *e)
{
    (void)e;
    if (U.viewer) {
        lv_obj_t *o = U.viewer;
        U.viewer = NULL;
        lv_obj_delete_async(o);
    }
}

typedef struct {
    char path[176];
    char caption[160];
} viewer_arg_t;

static void viewer_try(lv_timer_t *t)
{
    viewer_arg_t *a = (viewer_arg_t *)lv_timer_get_user_data(t);
    if (!U.viewer) {
        mj_free(a);
        lv_timer_delete(t);
        return;
    }
    const lv_image_dsc_t *d = mj_img_get(a->path, U.W, U.H - 120, false);
    if (!d) return;
    lv_obj_t *img = lv_image_create(U.viewer);
    mj_img_attach(img, d);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sp = lv_obj_get_child(U.viewer, 0);
    if (sp) lv_obj_add_flag(sp, LV_OBJ_FLAG_HIDDEN);
    mj_free(a);
    lv_timer_delete(t);
}

static void viewer_open(const char *path, const char *caption)
{
    mj_kb_hide();
    U.viewer = mj_box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.viewer, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.viewer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.viewer, viewer_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sp = lv_spinner_create(U.viewer);
    lv_obj_set_size(sp, 64, 64);
    lv_obj_center(sp);
    if (caption && *caption) {
        lv_obj_t *c = lv_label_create(U.viewer);
        lv_obj_set_style_text_font(c, aos_font_small, 0);
        lv_obj_set_style_text_color(c, AOS_C_TEXT, 0);
        mj_label_safe(c, caption);
        lv_obj_set_width(c, U.W - 2 * AOS_UI_PAD);
        lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -24);
    }
    viewer_arg_t *a = (viewer_arg_t *)mj_malloc(sizeof *a);
    if (!a) return;
    snprintf(a->path, sizeof a->path, "%s", path);
    snprintf(a->caption, sizeof a->caption, "%s", caption ? caption : "");
    lv_timer_t *t = lv_timer_create(viewer_try, 60, a);
    viewer_try(t);
}

static int find_view(int32_t id)
{
    for (int i = 0; i < s_nv; i++)
        if (s_v[i].m.id == id) return i;
    return -1;
}

static void photo_cb(lv_event_t *e)
{
    int32_t id = (int32_t)(intptr_t)lv_event_get_user_data(e);
    int i = find_view(id);
    if (i < 0) return;
    char p[176];
    if (s_v[i].m.pending) snprintf(p, sizeof p, "%s", s_v[i].md.name);
    else path_for(&s_v[i], MJ_FETCH_PHOTO, p, sizeof p);
    if (mj_file_exists(p)) viewer_open(p, s_v[i].m.text);
}

static void download_cb(lv_event_t *e)
{
    int32_t id = (int32_t)(intptr_t)lv_event_get_user_data(e);
    int i = find_view(id);
    if (i < 0) return;
    char p[176];
    path_for(&s_v[i], MJ_FETCH_DOC, p, sizeof p);
    if (mj_file_exists(p)) {
        char t[220];
        snprintf(t, sizeof t, _("Ya está en la tarjeta: %s"), p);
        aos_ui_toast(t, 3000);
        return;
    }
    mj_api_fetch(MJ_FETCH_DOC, NULL, &s_v[i].md, id);
    aos_ui_toast(_("Bajando a la tarjeta…"), 1500);
}

static void add_photo(lv_obj_t *b, const mv_t *v, int32_t bw, bool *wants)
{
    int w = v->md.w > 0 ? v->md.w : 4, h = v->md.h > 0 ? v->md.h : 3;
    int dw = bw - 20 < PHOTO_MAX_W ? bw - 20 : PHOTO_MAX_W;
    int dh = dw * h / w;
    if (dh > 460) {
        dh = 460;
        dw = 460 * w / h;
    }
    if (dw < 120) dw = 120;
    lv_obj_t *box = mj_box(b, dw, dh);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_bg_color(box, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(box, photo_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)v->m.id);
    char p[176];
    if (v->m.pending) snprintf(p, sizeof p, "%s", v->md.name);
    else path_for(v, MJ_FETCH_PHOTO, p, sizeof p);
    const lv_image_dsc_t *d = NULL;
    if (mj_file_exists(p)) d = mj_img_get(p, dw, dh, true);
    else if (!v->m.pending) mj_api_fetch(MJ_FETCH_PHOTO, NULL, &v->md, v->m.id);
    if (d) {
        lv_obj_t *img = lv_image_create(box);
        mj_img_attach(img, d);
        lv_obj_center(img);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t *g = mj_label(box, AOS_SYM_IMAGE, &aos_sym_44, AOS_C_DIM);
        lv_obj_center(g);
        *wants = true;
    }
}

static void add_sticker(lv_obj_t *b, const mv_t *v, bool *wants)
{
    char p[176];
    const lv_image_dsc_t *d = NULL;
    if (v->md.thumb_small[0]) {
        path_for(v, MJ_FETCH_THUMB, p, sizeof p);
        if (mj_file_exists(p)) d = mj_img_get(p, 200, 200, false);
        else mj_api_fetch(MJ_FETCH_THUMB, NULL, &v->md, v->m.id);
        if (!d) *wants = true;
    }
    if (d) {
        lv_obj_t *img = lv_image_create(b);
        mj_img_attach(img, d);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        /* a sticker without a JPEG thumbnail (most are WebP): its emoji, big */
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_huge, 0);
        mj_label_safe(l, v->md.emoji[0] ? v->md.emoji : "\xF0\x9F\x99\x82");
    }
}

static void add_file(lv_obj_t *b, const mv_t *v, int32_t bw, bool *wants)
{
    lv_obj_t *r = mj_box(b, bw - 32, 84);
    const char *glyph = v->md.kind == MJ_MEDIA_VOICE    ? AOS_SYM_MICROPHONE
                        : v->md.kind == MJ_MEDIA_AUDIO  ? AOS_SYM_MUSIC
                        : v->md.kind == MJ_MEDIA_DOC    ? AOS_SYM_FILE_DOCUMENT_OUTLINE
                                                        : AOS_SYM_VIDEO;
    lv_obj_t *c = mj_box(r, 72, 72);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, v->m.out ? lv_color_white() : AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(c, v->m.out ? LV_OPA_30 : LV_OPA_COVER, 0);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *g = mj_label(c, glyph, &aos_sym_28, AOS_C_TEXT);
    lv_obj_center(g);
    char title[120], sub[120], sz[24];
    mj_size_words(v->md.size, sz, sizeof sz);
    int s = v->md.duration;
    switch (v->md.kind) {
    case MJ_MEDIA_VOICE:
        snprintf(title, sizeof title, "%s", _("Nota de voz"));
        snprintf(sub, sizeof sub, "%d:%02d · %s", s / 60, s % 60, sz);
        break;
    case MJ_MEDIA_AUDIO:
        snprintf(title, sizeof title, "%s", v->md.title[0] ? v->md.title : v->md.name[0] ? v->md.name : _("Audio"));
        snprintf(sub, sizeof sub, "%d:%02d · %s", s / 60, s % 60, sz);
        break;
    case MJ_MEDIA_DOC:
        snprintf(title, sizeof title, "%s", v->md.name[0] ? v->md.name : _("Archivo"));
        snprintf(sub, sizeof sub, "%s", sz);
        break;
    default:
        snprintf(title, sizeof title, "%s", v->md.kind == MJ_MEDIA_GIF ? "GIF" : _("Video"));
        snprintf(sub, sizeof sub, "%d:%02d · %s", s / 60, s % 60, sz);
        break;
    }
    char p[176];
    path_for(v, MJ_FETCH_DOC, p, sizeof p);
    bool have = mj_file_exists(p);
    if (have) {
        size_t l = strlen(sub);
        snprintf(sub + l, sizeof sub - l, " · %s", _("en la tarjeta"));
    }
    lv_obj_t *tl = lv_label_create(r);
    lv_obj_set_style_text_font(tl, aos_font_small, 0);
    lv_obj_set_style_text_color(tl, AOS_C_TEXT, 0);
    mj_label_safe(tl, title);
    lv_obj_set_width(tl, bw - 32 - 84 - 76);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(tl, 86, 8);
    lv_obj_t *sl = mj_label(r, sub, aos_font_caption, v->m.out ? lv_color_hex(0xD8E6FF) : AOS_C_DIM);
    lv_obj_set_width(sl, bw - 32 - 84 - 76);
    lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(sl, 86, 46);
    lv_obj_t *dl = mj_icon_btn(r, have ? AOS_SYM_CHECK : AOS_SYM_DOWNLOAD, 60, AOS_C_CARD, download_cb,
                               (void *)(intptr_t)v->m.id);
    lv_obj_align(dl, LV_ALIGN_RIGHT_MID, 0, 0);
    if (!have) *wants = true;
}

/* ---- a bubble ---- */

static void bubble_long(lv_event_t *e);

static lv_obj_t *make_day(lv_obj_t *list, int32_t t, int32_t w)
{
    lv_obj_t *row = mj_box(list, w, 64);
    char d[48];
    mj_day_label(t, d, sizeof d);
    lv_obj_t *p = mj_box(row, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_radius(p, 20, 0);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(p, 18, 0);
    lv_obj_t *l = mj_label(p, d, aos_font_caption, AOS_C_DIM);
    lv_obj_center(l);
    lv_obj_center(p);
    return row;
}

static lv_obj_t *make_service(lv_obj_t *list, const mv_t *v, int32_t w)
{
    lv_obj_t *row = mj_box(list, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 8, 0);
    char t[200];
    mj_action_words(&v->m, v->from_name, t, sizeof t);
    lv_obj_t *p = mj_box(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(p, 20, 0);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(p, 18, 0);
    lv_obj_set_style_pad_ver(p, 8, 0);
    lv_obj_set_style_max_width(p, w - 80, 0);
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, aos_font_caption, 0);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    mj_label_safe(l, t);
    lv_obj_set_style_max_width(l, w - 116, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(p);
    return row;
}

static lv_obj_t *make_bubble(lv_obj_t *list, const mv_t *v, int32_t w, bool *wants)
{
    if (v->m.service) return make_service(list, v, w);
    int32_t bw = w * BUBBLE_MAX_PCT / 100;
    lv_obj_t *row = mj_box(list, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, v->first_of_run ? 10 : 3, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_t *b = mj_box(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, bw, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_set_style_pad_row(b, 6, 0);
    lv_obj_set_style_radius(b, 24, 0);
    bool sticker = v->m.media && v->md.kind == MJ_MEDIA_STICKER;
    if (!sticker) {
        lv_obj_set_style_bg_color(b, v->m.out ? AOS_C_ACCENT : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
    if (v->m.failed) {
        lv_obj_set_style_border_color(b, AOS_C_RED, 0);
        lv_obj_set_style_border_width(b, 3, 0);
    }
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, bubble_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)v->m.id);
    lv_obj_align(b, v->m.out ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
    lv_color_t sub = v->m.out ? lv_color_hex(0xD8E6FF) : AOS_C_DIM;

    if (v->group && !v->m.out && v->first_of_run) {
        lv_obj_t *n = lv_label_create(b);
        lv_obj_set_style_text_font(n, aos_font_caption, 0);
        lv_obj_set_style_text_color(n, mj_peer_color(v->m.from.id), 0);
        mj_label_safe(n, v->from_name);
    }
    if (v->m.fwd_from[0]) {
        char f[MJ_NAME + 40];
        snprintf(f, sizeof f, _("Reenviado de %s"), v->m.fwd_from);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
        lv_obj_set_style_text_color(l, sub, 0);
        mj_label_safe(l, f);
    }
    if (v->m.reply_to && v->reply_name[0]) {
        lv_obj_t *q = mj_box(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(q, bw - 32, 0);
        lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(q, 4, 0);
        lv_obj_set_style_border_color(q, v->m.out ? lv_color_white() : AOS_C_ACCENT, 0);
        lv_obj_set_style_pad_left(q, 12, 0);
        lv_obj_set_flex_flow(q, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *n = lv_label_create(q);
        lv_obj_set_style_text_font(n, aos_font_caption, 0);
        lv_obj_set_style_text_color(n, v->m.out ? lv_color_white() : AOS_C_ACCENT, 0);
        mj_label_safe(n, v->reply_name);
        lv_obj_t *t = lv_label_create(q);
        lv_obj_set_style_text_font(t, aos_font_caption, 0);
        lv_obj_set_style_text_color(t, sub, 0);
        mj_label_safe(t, v->reply_text);
        lv_obj_set_style_max_width(t, bw - 60, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    }
    if (v->m.media) {
        switch (v->md.kind) {
        case MJ_MEDIA_PHOTO:
            add_photo(b, v, bw, wants);
            break;
        case MJ_MEDIA_STICKER:
            add_sticker(b, v, wants);
            break;
        case MJ_MEDIA_VOICE:
        case MJ_MEDIA_AUDIO:
        case MJ_MEDIA_DOC:
        case MJ_MEDIA_VIDEO:
        case MJ_MEDIA_ROUND:
        case MJ_MEDIA_GIF:
            add_file(b, v, bw, wants);
            break;
        case MJ_MEDIA_WEB:
            break;
        default: {
            char t[160];
            mj_media_words(&v->m, t, sizeof t);
            lv_obj_t *l = lv_label_create(b);
            lv_obj_set_style_text_font(l, aos_font_small, 0);
            lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
            mj_label_safe(l, t);
            lv_obj_set_style_max_width(l, bw - 32, 0);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        }
        }
    }
    if (v->m.text[0]) {
        lv_obj_t *t = lv_label_create(b);
        lv_obj_set_style_text_font(t, aos_font_body, 0);
        lv_obj_set_style_text_color(t, AOS_C_TEXT, 0);
        mj_label_safe(t, v->m.text);
        lv_obj_set_style_max_width(t, bw - 32, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    }
    if (v->m.media && v->md.kind == MJ_MEDIA_WEB && v->md.title[0]) {
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
        lv_obj_set_style_text_color(l, sub, 0);
        char t[160];
        snprintf(t, sizeof t, "\xF0\x9F\x94\x97 %s", v->md.title);
        mj_label_safe(l, t);
        lv_obj_set_style_max_width(l, bw - 32, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    if (v->m.reactions[0]) {
        lv_obj_t *r = lv_label_create(b);
        lv_obj_set_style_text_font(r, aos_font_small, 0);
        lv_obj_set_style_text_color(r, AOS_C_TEXT, 0);
        mj_label_safe(r, v->m.reactions);
    }
    /* the time, "edited", the ticks */
    char t[16], foot[64];
    mj_time_short(v->m.date, t, sizeof t);
    snprintf(foot, sizeof foot, "%s%s%s", v->m.edit_date ? _("editado") : "", v->m.edit_date ? " " : "", t);
    if (v->m.views && s_broadcast) {
        size_t l = strlen(foot);
        snprintf(foot + l, sizeof foot - l, "  \xF0\x9F\x91\x81\xEF\xB8\x8F %d", v->m.views);
    }
    lv_obj_t *fr = mj_box(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fr, 6, 0);
    lv_obj_set_style_align(fr, LV_ALIGN_RIGHT_MID, 0);
    lv_obj_t *fl = lv_label_create(fr);
    lv_obj_set_style_text_font(fl, aos_font_caption, 0);
    lv_obj_set_style_text_color(fl, sub, 0);
    mj_label_safe(fl, foot);
    if (v->m.out) {
        const char *g = v->m.pending ? AOS_SYM_CLOCK_OUTLINE : v->m.failed ? AOS_SYM_ALERT_OUTLINE : AOS_SYM_CHECK;
        lv_obj_t *ck = mj_label(fr, v->read ? AOS_SYM_CHECK AOS_SYM_CHECK : g, &aos_sym_28, sub);
        lv_obj_set_style_text_letter_space(ck, -16, 0);
    }
    if (sticker) {
        lv_obj_set_style_bg_color(fr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(fr, LV_OPA_50, 0);
        lv_obj_set_style_radius(fr, 12, 0);
        lv_obj_set_style_pad_hor(fr, 8, 0);
    }
    return row;
}

/* ---- the message's sheet ---- */

static int32_t s_sheet_id;

static void react_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int i = find_view(s_sheet_id);
    mj_sheet_close();
    if (i < 0) return;
    const char *em = MJ_REACTIONS[k];
    /* the same one again takes it away */
    bool same = !strcmp(s_v[i].m.my_reaction, em);
    mj_api_react(U.open, s_sheet_id, same ? "" : em);
}

static void composer_mode(void);

static void act_cb(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    int i = find_view(s_sheet_id);
    mj_sheet_close();
    if (i < 0) return;
    const mv_t *v = &s_v[i];
    switch (what) {
    case 0: /* reply */
        U.reply_to = v->m.id;
        U.editing = 0;
        composer_mode();
        mj_kb_show(U.ta, LV_KEYBOARD_MODE_TEXT_LOWER, NULL);
        break;
    case 1: /* forward */
        mj_forward_pick(U.open, v->m.id);
        break;
    case 2: /* copy */
        aos_ui_clip_set(v->m.text);
        aos_ui_toast(_("Copiado"), 1200);
        break;
    case 3: /* edit */
        U.editing = v->m.id;
        U.reply_to = 0;
        lv_textarea_set_text(U.ta, v->m.text);
        composer_mode();
        mj_kb_show(U.ta, LV_KEYBOARD_MODE_TEXT_LOWER, NULL);
        break;
    case 4: /* delete */
        mj_api_delete(U.open, v->m.id, true);
        break;
    }
}

static void sheet_item(lv_obj_t *b, const char *glyph, const char *text, int what, lv_color_t c)
{
    lv_obj_t *r = mj_box(b, lv_pct(100), 84);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_add_event_cb(r, act_cb, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    lv_obj_t *g = mj_label(r, glyph, &aos_sym_28, c);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *l = mj_label(r, text, aos_font_body, c);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 64, 0);
}

static void bubble_long(lv_event_t *e)
{
    int32_t id = (int32_t)(intptr_t)lv_event_get_user_data(e);
    int i = find_view(id);
    if (i < 0 || s_v[i].m.pending) return;
    s_sheet_id = id;
    const mv_t *v = &s_v[i];
    bool can_post = !(s_broadcast && !v->m.out);
    lv_obj_t *b = mj_sheet_open(_("Mensaje"), U.land ? 620 : 700);
    /* the reactions, in a row */
    lv_obj_t *rr = mj_box(b, lv_pct(100), 96);
    lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int k = 0; k < MJ_NREACTIONS; k++) {
        lv_obj_t *c = mj_box(rr, 72, 72);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        bool mine = !strcmp(v->m.my_reaction, MJ_REACTIONS[k]);
        lv_obj_set_style_bg_color(c, mine ? AOS_C_ACCENT : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, react_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_obj_t *l = lv_label_create(c);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        mj_label_safe(l, MJ_REACTIONS[k]);
        lv_obj_center(l);
    }
    if (can_post) sheet_item(b, AOS_SYM_ARROW_LEFT, _("Responder"), 0, AOS_C_TEXT);
    sheet_item(b, AOS_SYM_ARROW_RIGHT_BOLD, _("Reenviar"), 1, AOS_C_TEXT);
    if (v->m.text[0]) sheet_item(b, AOS_SYM_CONTENT_COPY, _("Copiar"), 2, AOS_C_TEXT);
    if (v->m.out && v->m.text[0] && !v->m.media) sheet_item(b, AOS_SYM_PENCIL, _("Editar"), 3, AOS_C_TEXT);
    if (v->m.out || U.open.type == MJ_PEER_USER)
        sheet_item(b, AOS_SYM_DELETE, v->m.out ? _("Borrar para todos") : _("Borrar"), 4, AOS_C_RED);
}

/* ---- the composer ---- */

static void composer_mode(void)
{
    if (!U.reply_bar) return;
    bool on = U.reply_to || U.editing;
    if (!on) {
        lv_obj_add_flag(U.reply_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        int i = find_view(U.reply_to ? U.reply_to : U.editing);
        char t[200];
        if (U.editing) snprintf(t, sizeof t, "%s", _("Editando el mensaje"));
        else snprintf(t, sizeof t, _("Respondiendo a %s: %s"), i >= 0 ? s_v[i].from_name : "?", i >= 0 ? s_v[i].m.text : "");
        mj_label_safe(U.reply_lbl, t);
        lv_obj_remove_flag(U.reply_bar, LV_OBJ_FLAG_HIDDEN);
    }
    mj_chat_refresh(true);
}

static void reply_close_cb(lv_event_t *e)
{
    (void)e;
    if (U.editing) lv_textarea_set_text(U.ta, "");
    U.reply_to = U.editing = 0;
    composer_mode();
}

static void send_now(void)
{
    if (!U.ta || !U.open.type) return;
    const char *t = lv_textarea_get_text(U.ta);
    while (*t == ' ' || *t == '\n') t++;
    if (!*t) return;
    if (U.editing) mj_api_edit(U.open, U.editing, t);
    else mj_api_send_text(U.open, t, U.reply_to);
    lv_textarea_set_text(U.ta, "");
    U.reply_to = U.editing = 0;
    U.want_bottom = true;
    composer_mode();
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    send_now();
}

static void ta_ready(lv_event_t *e)
{
    (void)e;
    send_now();
}

static void emoji_hide(void)
{
    if (U.emoji_panel) {
        lv_obj_delete(U.emoji_panel);
        U.emoji_panel = NULL;
    }
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_CLICKED) {
        emoji_hide();
        mj_kb_show(U.ta, LV_KEYBOARD_MODE_TEXT_LOWER, ta_ready);
    } else if (c == LV_EVENT_VALUE_CHANGED) {
        if (lv_textarea_get_text(U.ta)[0]) mj_api_typing(U.open);
    }
}

static void emoji_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (U.ta && i >= 0 && i < MJ_NEMOJI) lv_textarea_add_text(U.ta, MJ_EMOJI[i]);
}

static void emoji_cb(lv_event_t *e)
{
    (void)e;
    if (U.emoji_panel) {
        emoji_hide();
        mj_chat_refresh(true);
        return;
    }
    mj_kb_hide();
    int32_t w = lv_obj_get_width(U.chat_pane);
    U.emoji_panel = mj_box(U.chat_pane, w, 320);
    lv_obj_set_style_bg_color(U.emoji_panel, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(U.emoji_panel, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(U.emoji_panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(U.emoji_panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(U.emoji_panel, 8, 0);
    lv_obj_add_flag(U.emoji_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    int per = w >= 700 ? 10 : 8;
    int32_t cell = (w - 16) / per - 4;
    for (int i = 0; i < MJ_NEMOJI; i++) {
        lv_obj_t *c = mj_box(U.emoji_panel, cell, 72);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(c, 14, 0);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, emoji_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(c);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        mj_label_safe(l, MJ_EMOJI[i]);
        lv_obj_center(l);
    }
    mj_chat_refresh(true);
}

/* sending a photo: the JPEGs of the photos folder */

typedef struct {
    char path[200];
    lv_obj_t *cell;             /* while it waits for its thumbnail          */
} pick_t;
static pick_t *s_pick;
static int s_npick;
static int32_t s_pick_cell;

static void pick_cell_gone(lv_event_t *e)
{
    pick_t *p = (pick_t *)lv_event_get_user_data(e);
    p->cell = NULL;
}

/* the thumbnails of the photo picker that were not decoded yet */
static void pick_refresh(void)
{
    for (int i = 0; s_pick && i < s_npick; i++) {
        lv_obj_t *c = s_pick[i].cell;
        if (!c) continue;
        const lv_image_dsc_t *d = mj_img_get(s_pick[i].path, s_pick_cell, s_pick_cell, true);
        if (!d) continue;
        lv_obj_clean(c);
        lv_obj_t *img = lv_image_create(c);
        mj_img_attach(img, d);
        lv_obj_center(img);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        s_pick[i].cell = NULL;
    }
}

static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_npick) return;
    const char *cap = U.ta ? lv_textarea_get_text(U.ta) : "";
    mj_api_send_photo(U.open, s_pick[i].path, cap, U.reply_to);
    if (U.ta) lv_textarea_set_text(U.ta, "");
    U.reply_to = 0;
    U.want_bottom = true;
    mj_sheet_close();
    composer_mode();
}

static bool is_jpeg(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg"));
}

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_npick < 60) {
        if (e->d_name[0] == '.') continue;
        char p[320];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 1) scan_dir(p, depth + 1);
        } else if (is_jpeg(e->d_name) && strlen(p) < sizeof s_pick[0].path) {
            mj_scpy(s_pick[s_npick++].path, sizeof s_pick[0].path, p);
        }
    }
    closedir(d);
}

static void attach_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pick) s_pick = (pick_t *)mj_calloc(60 * sizeof *s_pick);
    if (!s_pick) return;
    s_npick = 0;
    mj_sheet_close();
    memset(s_pick, 0, 60 * sizeof *s_pick);
    const char *ph = aos_hal_path_photos();
    if (ph) scan_dir(ph, 0);
    lv_obj_t *b = mj_sheet_open(_("Mandar una foto"), U.land ? 660 : 900);
    if (!s_npick) {
        char t[240];
        snprintf(t, sizeof t, _("No hay fotos JPEG en %s. Copialas ahí desde el portal (Archivos) o la computadora."),
                 ph ? ph : "/sdcard");
        lv_obj_t *l = mj_label(b, t, aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        return;
    }
    lv_obj_t *grid = mj_box(b, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    int32_t cell = ((U.land ? 720 : U.W) - 2 * AOS_UI_PAD - 20) / 3;
    s_pick_cell = cell;
    for (int i = 0; i < s_npick; i++) {
        lv_obj_t *c = mj_box(grid, cell, cell);
        lv_obj_set_style_radius(c, 14, 0);
        lv_obj_set_style_clip_corner(c, true, 0);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        const lv_image_dsc_t *d = mj_img_get(s_pick[i].path, cell, cell, true);
        if (d) {
            lv_obj_t *img = lv_image_create(c);
            mj_img_attach(img, d);
            lv_obj_center(img);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        } else {
            const char *base = strrchr(s_pick[i].path, '/');
            lv_obj_t *l = mj_label(c, base ? base + 1 : s_pick[i].path, aos_font_caption, AOS_C_DIM);
            lv_obj_set_width(l, cell - 12);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_center(l);
            s_pick[i].cell = c;
            lv_obj_add_event_cb(c, pick_cell_gone, LV_EVENT_DELETE, &s_pick[i]);
        }
    }
}

/* ---- the pane ---- */

static void back_cb(lv_event_t *e)
{
    (void)e;
    mj_ui_close_chat();
}

static void list_scroll_cb(lv_event_t *e)
{
    (void)e;
    if (!U.chat_list) return;
    if (lv_obj_get_scroll_y(U.chat_list) < 240 && !s_start && !s_loading && s_nv) {
        s_loading = true;
        mj_api_more_history();
    }
}

void mj_chat_free_rows(void)
{
    mj_free(U.rows);
    U.rows = NULL;
    U.nrows = U.rows_cap = 0;
    views_free();
}

void mj_chat_status(void)
{
    if (!U.chat_status || !U.open.type) return;
    char st[96];
    mj_status_line(U.open, st, sizeof st);
    lv_label_set_text(U.chat_status, st);
    bool typing = strstr(st, "…") != NULL;
    lv_obj_set_style_text_color(U.chat_status, typing ? AOS_C_ACCENT : AOS_C_DIM, 0);
}

void mj_chat_build(lv_obj_t *pane)
{
    mj_chat_free_rows();
    U.chat_list = U.chat_title = U.chat_status = U.composer = U.ta = NULL;
    U.reply_bar = U.emoji_panel = NULL;
    lv_obj_update_layout(pane);
    int32_t w = lv_obj_get_width(pane);
    if (!U.open.type) {
        lv_obj_t *l = mj_label(pane, _("Elegí un chat de la lista"), aos_font_body, AOS_C_DIM);
        lv_obj_center(l);
        return;
    }
    /* the header */
    lv_obj_t *hd = mj_box(pane, w, 96);
    lv_obj_set_style_bg_color(hd, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(hd, LV_OPA_COVER, 0);
    int32_t x = 16;
    if (!U.land) {
        lv_obj_t *bk = mj_icon_btn(hd, AOS_SYM_CHEVRON_LEFT, 64, AOS_C_CARD, back_cb, NULL);
        lv_obj_align(bk, LV_ALIGN_LEFT_MID, 8, 0);
        x = 80;
    }
    lv_obj_t *av = mj_avatar(hd, U.open, 64);
    lv_obj_align(av, LV_ALIGN_LEFT_MID, x, 0);
    U.chat_title = lv_label_create(hd);
    lv_obj_set_style_text_font(U.chat_title, aos_font_body, 0);
    lv_obj_set_style_text_color(U.chat_title, AOS_C_TEXT, 0);
    mj_store_lock();
    char name[MJ_NAME];
    mj_peer_t *p = mj_store_peer(U.open);
    snprintf(name, sizeof name, "%s", p && p->self ? _("Mensajes guardados") : mj_peer_name(U.open));
    mj_store_unlock();
    mj_label_safe(U.chat_title, name);
    lv_obj_set_width(U.chat_title, w - x - 100);
    lv_label_set_long_mode(U.chat_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.chat_title, x + 80, 12);
    U.chat_status = mj_label(hd, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(U.chat_status, x + 80, 56);
    mj_chat_status();

    /* the messages */
    U.chat_list = mj_box(pane, w, 100);
    lv_obj_set_pos(U.chat_list, 0, 96);
    lv_obj_add_flag(U.chat_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(U.chat_list, LV_DIR_VER);
    lv_obj_set_flex_flow(U.chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(U.chat_list, 12, 0);
    lv_obj_add_event_cb(U.chat_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

    /* reply / edit */
    U.reply_bar = mj_box(pane, w, 64);
    lv_obj_set_style_bg_color(U.reply_bar, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(U.reply_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(U.reply_bar, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(U.reply_bar, 6, 0);
    lv_obj_set_style_border_color(U.reply_bar, AOS_C_ACCENT, 0);
    U.reply_lbl = mj_label(U.reply_bar, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_set_width(U.reply_lbl, w - 110);
    lv_label_set_long_mode(U.reply_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.reply_lbl, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_t *rx = mj_icon_btn(U.reply_bar, AOS_SYM_CLOSE, 52, AOS_C_CARD2, reply_close_cb, NULL);
    lv_obj_align(rx, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_flag(U.reply_bar, LV_OBJ_FLAG_HIDDEN);

    /* the composer */
    mj_store_lock();
    bool can_post = !(p && p->broadcast);
    mj_store_unlock();
    U.composer = mj_box(pane, w, COMPOSER_H);
    lv_obj_set_style_bg_color(U.composer, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(U.composer, LV_OPA_COVER, 0);
    if (can_post) {
        lv_obj_t *at = mj_icon_btn(U.composer, AOS_SYM_IMAGE, 64, AOS_C_CARD2, attach_cb, NULL);
        lv_obj_align(at, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *em = mj_box(U.composer, 64, 64);
        lv_obj_set_style_radius(em, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(em, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(em, LV_OPA_COVER, 0);
        lv_obj_add_flag(em, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(em, emoji_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *el = lv_label_create(em);
        lv_obj_set_style_text_font(el, aos_font_body, 0);
        mj_label_safe(el, "\xF0\x9F\x98\x80");
        lv_obj_center(el);
        lv_obj_align(em, LV_ALIGN_LEFT_MID, 84, 0);
        U.ta = lv_textarea_create(U.composer);
        lv_textarea_set_one_line(U.ta, true);
        lv_obj_set_scrollbar_mode(U.ta, LV_SCROLLBAR_MODE_OFF);
        lv_textarea_set_placeholder_text(U.ta, _("Mensaje"));
        lv_obj_set_size(U.ta, w - 84 - 72 - 96 - 12, 64);
        lv_obj_align(U.ta, LV_ALIGN_LEFT_MID, 160, 0);
        lv_obj_set_style_text_font(U.ta, aos_font_small, 0);
        lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
        lv_obj_set_style_bg_color(U.ta, AOS_C_CARD2, 0);
        lv_obj_set_style_border_width(U.ta, 0, 0);
        lv_obj_set_style_radius(U.ta, 32, 0);
        lv_obj_set_style_pad_hor(U.ta, 22, 0);
        lv_obj_set_style_pad_ver(U.ta, 16, 0);
        lv_obj_add_event_cb(U.ta, ta_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(U.ta, ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
        U.send_btn = mj_icon_btn(U.composer, AOS_SYM_SEND, 68, AOS_C_ACCENT, send_cb, NULL);
        lv_obj_align(U.send_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    } else {
        lv_obj_t *l = mj_label(U.composer, _("Sólo los administradores escriben en este canal"), aos_font_small, AOS_C_DIM);
        lv_obj_center(l);
    }
    U.want_bottom = true;
    mj_chat_refresh(true);
}

/* Sizes the list between the header and the composer (and the keyboard or
 * the emoji under it). */
static void layout(void)
{
    if (!U.chat_list || !U.chat_pane) return;
    int32_t ph = lv_obj_get_height(U.chat_pane);
    int32_t w = lv_obj_get_width(U.chat_pane);
    int32_t under = 0;
    if (U.kb) under = lv_obj_get_height(U.kb);
    else if (U.emoji_panel) under = 320;
    int32_t cy = ph - under - COMPOSER_H;
    if (U.composer) lv_obj_set_pos(U.composer, 0, cy);
    if (U.emoji_panel) lv_obj_set_pos(U.emoji_panel, 0, ph - 320);
    int32_t bottom = cy;
    if (U.reply_bar && !lv_obj_has_flag(U.reply_bar, LV_OBJ_FLAG_HIDDEN)) {
        bottom -= 64;
        lv_obj_set_pos(U.reply_bar, 0, bottom);
    }
    lv_obj_set_size(U.chat_list, w, bottom - 96 > 100 ? bottom - 96 : 100);
}

static int find_row(int32_t id, bool day)
{
    for (int i = 0; i < U.nrows; i++)
        if (U.rows[i].id == id && U.rows[i].day == day && U.rows[i].obj) return i;
    return -1;
}

void mj_chat_refresh(bool force)
{
    (void)force;
    if (!U.chat_list) return;
    /* at the bottom before the composer or the keyboard moved: it stays there */
    lv_obj_update_layout(U.chat_list);
    bool at_bottom = lv_obj_get_scroll_bottom(U.chat_list) < 120;
    layout();
    lv_obj_update_layout(U.chat_list);
    copy_views();
    int32_t w = lv_obj_get_width(U.chat_list);
    int32_t old_top_id = U.nrows ? U.rows[0].id : 0;
    lv_obj_t *anchor = NULL;
    int32_t anchor_y = 0;

    /* the rows wanted: a day's label before the first message of each day */
    int want = s_nv * 2 + 1;
    mj_row_t *nr = (mj_row_t *)mj_calloc((size_t)want * sizeof *nr);
    if (!nr) return;
    int n = 0, last_day = -1;
    for (int i = 0; i < s_nv; i++) {
        int dd = day_of(s_v[i].m.date);
        if (dd != last_day) {
            nr[n].id = dd;
            nr[n].day = true;
            nr[n].sig = (uint32_t)dd;
            n++;
            last_day = dd;
        }
        nr[n].id = s_v[i].m.id;
        nr[n].sig = sig_of(&s_v[i], media_ready(&s_v[i]));
        n++;
    }
    /* reuse what did not change */
    for (int i = 0; i < n; i++) {
        int j = find_row(nr[i].id, nr[i].day);
        if (j >= 0 && U.rows[j].sig == nr[i].sig) {
            nr[i].obj = U.rows[j].obj;
            nr[i].wants_media = U.rows[j].wants_media;
            U.rows[j].obj = NULL;
            if (!anchor && old_top_id && nr[i].id != old_top_id) {
                /* the first row that stays, to keep it in place */
            }
            if (!anchor) {
                anchor = nr[i].obj;
                anchor_y = lv_obj_get_y(anchor) - lv_obj_get_scroll_y(U.chat_list);
            }
        }
    }
    for (int j = 0; j < U.nrows; j++)
        if (U.rows[j].obj) lv_obj_delete(U.rows[j].obj);
    /* make the new ones, and put every row in its place */
    int vi = 0;
    for (int i = 0; i < n; i++) {
        if (!nr[i].day) {
            while (vi < s_nv && s_v[vi].m.id != nr[i].id) vi++;
        }
        if (!nr[i].obj) {
            bool wants = false;
            if (nr[i].day) {
                int k = vi < s_nv ? vi : s_nv - 1;
                /* the day's first message is the next one */
                for (int q = 0; q < s_nv; q++)
                    if (day_of(s_v[q].m.date) == nr[i].id) {
                        k = q;
                        break;
                    }
                nr[i].obj = make_day(U.chat_list, s_v[k].m.date, w);
            } else if (vi < s_nv) {
                nr[i].obj = make_bubble(U.chat_list, &s_v[vi], w, &wants);
            }
            nr[i].wants_media = wants;
        }
        if (nr[i].obj && lv_obj_get_index(nr[i].obj) != i) lv_obj_move_to_index(nr[i].obj, i);
    }
    mj_free(U.rows);
    U.rows = nr;
    U.nrows = n;
    U.rows_cap = want;

    lv_obj_update_layout(U.chat_list);
    if (U.want_bottom || at_bottom) {
        lv_obj_scroll_to_y(U.chat_list, LV_COORD_MAX, LV_ANIM_OFF);
        U.want_bottom = false;
    } else if (anchor) {
        int32_t now_y = lv_obj_get_y(anchor) - lv_obj_get_scroll_y(U.chat_list);
        if (now_y != anchor_y) lv_obj_scroll_by(U.chat_list, 0, anchor_y - now_y, LV_ANIM_OFF);
    }
    if (U.visible && s_unread) mj_api_mark_read();
    mj_chat_status();
}

/* Files and pictures arrived: the rows that waited for them are made again
 * by the next refresh (their signature counts whether the file is there),
 * and the pictures already decoded are set in place. */
void mj_chat_media(void)
{
    if (!U.chat_list) {
        if (U.sheet) pick_refresh();
        return;
    }
    bool any = false;
    for (int i = 0; i < U.nrows; i++) {
        if (!U.rows[i].wants_media || U.rows[i].day) continue;
        int k = find_view(U.rows[i].id);
        if (k < 0) continue;
        const mv_t *v = &s_v[k];
        if (!media_ready(v)) continue;
        /* the file is there: a picture waits until it is decoded too */
        if (v->md.kind == MJ_MEDIA_PHOTO || (v->md.kind == MJ_MEDIA_STICKER && v->md.thumb_small[0])) {
            char p[176];
            if (v->m.pending) snprintf(p, sizeof p, "%s", v->md.name);
            else path_for(v, v->md.kind == MJ_MEDIA_PHOTO ? MJ_FETCH_PHOTO : MJ_FETCH_THUMB, p, sizeof p);
            int w = 200, h = 200;
            bool fill = false;
            if (v->md.kind == MJ_MEDIA_PHOTO) {
                /* the same size add_photo asks for */
                int32_t bw = lv_obj_get_width(U.chat_list) * BUBBLE_MAX_PCT / 100;
                int pw = v->md.w > 0 ? v->md.w : 4, ph = v->md.h > 0 ? v->md.h : 3;
                w = bw - 20 < PHOTO_MAX_W ? bw - 20 : PHOTO_MAX_W;
                h = w * ph / pw;
                if (h > 460) {
                    h = 460;
                    w = 460 * pw / ph;
                }
                if (w < 120) w = 120;
                fill = true;
            }
            if (!mj_img_get(p, w, h, fill)) continue;
        }
        U.rows[i].sig ^= 0x5bd1e995u;   /* made again by the refresh */
        any = true;
    }
    if (any) mj_chat_refresh(false);
    if (U.sheet) pick_refresh();
}

bool mj_chat_back(void)
{
    if (U.viewer) {
        viewer_close(NULL);
        return true;
    }
    if (U.emoji_panel) {
        emoji_hide();
        mj_chat_refresh(true);
        return true;
    }
    if (U.reply_to || U.editing) {
        reply_close_cb(NULL);
        return true;
    }
    return false;
}
