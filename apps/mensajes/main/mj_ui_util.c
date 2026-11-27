/*
 * P4OS - Mensajes: small widgets, times, names and words.
 */
#include <time.h>

#include "aos_text_safe.h"
#include "mj_ui.h"

const char *const MJ_REACTIONS[] = {
    "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\xAE",
    "\xF0\x9F\x98\xA2", "\xF0\x9F\x99\x8F", "\xF0\x9F\x94\xA5", "\xF0\x9F\x91\x8E",
};
const int MJ_NREACTIONS = 8;

/* the emoji of the composer's panel: the faces and hands people use most */
const char *const MJ_EMOJI[] = {
    "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x81", "\xF0\x9F\x98\x82", "\xF0\x9F\xA4\xA3", "\xF0\x9F\x98\x8A",
    "\xF0\x9F\x98\x8D", "\xF0\x9F\x98\x98", "\xF0\x9F\x98\x89", "\xF0\x9F\x98\x8E", "\xF0\x9F\xA4\x94",
    "\xF0\x9F\x98\x85", "\xF0\x9F\x98\xA2", "\xF0\x9F\x98\xAD", "\xF0\x9F\x98\xA1", "\xF0\x9F\x98\xB1",
    "\xF0\x9F\x98\xB4", "\xF0\x9F\xA5\xB3", "\xF0\x9F\x98\x87", "\xF0\x9F\x99\x83", "\xF0\x9F\xA4\x97",
    "\xF0\x9F\x91\x8D", "\xF0\x9F\x91\x8E", "\xF0\x9F\x91\x8F", "\xF0\x9F\x99\x8F", "\xF0\x9F\x92\xAA",
    "\xF0\x9F\x91\x8B", "\xF0\x9F\xA4\x9D", "\xE2\x9C\x8C\xEF\xB8\x8F", "\xF0\x9F\x91\x8C", "\xF0\x9F\xA4\x9E",
    "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x92\x94", "\xF0\x9F\x94\xA5", "\xE2\x9C\xA8", "\xF0\x9F\x8E\x89",
    "\xE2\x9C\x85", "\xE2\x9D\x8C", "\xE2\x9A\xBD", "\xF0\x9F\x8D\x95", "\xE2\x98\x95",
};
const int MJ_NEMOJI = 40;

lv_obj_t *mj_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *mj_label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t ? t : "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

void mj_label_safe(lv_obj_t *l, const char *t)
{
    if (!t) t = "";
    size_t n = strlen(t) * 2 + 16;
    char *s = (char *)mj_malloc(n);
    if (!s) {
        lv_label_set_text(l, t);
        return;
    }
    aos_text_safe(s, n, t);
    lv_label_set_text(l, s);
    mj_free(s);
}

lv_obj_t *mj_icon_btn(lv_obj_t *parent, const char *glyph, int32_t d, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = mj_box(parent, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *l = mj_label(b, glyph, &aos_sym_28, AOS_C_TEXT);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *mj_pill_btn(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = mj_box(parent, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, 32, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = mj_label(b, text, aos_font_body, AOS_C_TEXT);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static const uint32_t PALETTE[] = {0xE17076, 0xFAA774, 0xA695E7, 0x7BC862, 0x6EC9CB, 0x65AADD, 0xEE7AAE, 0xD4A15C};

lv_color_t mj_peer_color(int64_t id)
{
    uint32_t h = (uint32_t)id ^ (uint32_t)(id >> 32);
    return lv_color_hex(PALETTE[h % 8]);
}

/* the first letters of a name, "AP" for "Ana Pérez": whole UTF-8 characters */
static void initials(const char *name, char *out, size_t n)
{
    size_t o = 0;
    int words = 0;
    bool start = true;
    for (const char *p = name; *p && words < 2; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ') {
            start = true;
            continue;
        }
        if (start && (c & 0xC0) != 0x80) {
            int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (len > 2 || o + (size_t)len >= n) break;   /* no emoji as initials */
            for (int k = 0; k < len && p[k]; k++) {
                char ch = p[k];
                if (len == 1 && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                out[o++] = ch;
            }
            words++;
            start = false;
        }
    }
    out[o] = 0;
}

/* An avatar: the picture when it is in the cache (downloaded if not), the
 * initials on a colour meanwhile. The image is the box's first child and
 * keeps the peer in its user data, for mj_avatars_refresh. */
typedef struct {
    mj_pkey_t k;
    int32_t d;
} av_t;

static void av_free(lv_event_t *e)
{
    mj_free(lv_event_get_user_data(e));
}

static bool av_try(lv_obj_t *box, const av_t *a)
{
    mj_store_lock();
    mj_peer_t *p = mj_store_peer(a->k);
    mj_peer_t copy;
    bool have = p && p->photo_id;
    if (have) copy = *p;
    mj_store_unlock();
    if (!have) return true;
    char path[176];
    mj_api_path(MJ_FETCH_AVATAR, &copy, NULL, 0, path, sizeof path);
    if (!mj_file_exists(path)) {
        mj_api_fetch(MJ_FETCH_AVATAR, &copy, NULL, 0);
        return false;
    }
    const lv_image_dsc_t *d = mj_img_get(path, a->d, a->d, true);
    if (!d) return false;
    lv_obj_t *img = lv_image_create(box);
    mj_img_attach(img, d);
    lv_obj_set_style_radius(img, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return true;
}

lv_obj_t *mj_avatar(lv_obj_t *parent, mj_pkey_t k, int32_t d)
{
    lv_obj_t *b = mj_box(parent, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, mj_peer_color(k.id), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_darken(mj_peer_color(k.id), 60), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    char ini[12];
    mj_store_lock();
    initials(mj_peer_name(k), ini, sizeof ini);
    mj_store_unlock();
    lv_obj_t *l = mj_label(b, ini, d >= 64 ? aos_font_body : aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    av_t *a = (av_t *)mj_malloc(sizeof *a);
    if (a) {
        a->k = k;
        a->d = d;
        lv_obj_set_user_data(b, a);
        lv_obj_add_event_cb(b, av_free, LV_EVENT_DELETE, a);
        if (!av_try(b, a)) lv_obj_add_flag(b, LV_OBJ_FLAG_USER_1);   /* waiting for its picture */
    }
    return b;
}

static void av_walk(lv_obj_t *o)
{
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1) && lv_obj_get_user_data(c)) {
            if (av_try(c, (const av_t *)lv_obj_get_user_data(c))) lv_obj_remove_flag(c, LV_OBJ_FLAG_USER_1);
        } else {
            av_walk(c);
        }
    }
}

void mj_avatars_refresh(lv_obj_t *root)
{
    if (root) av_walk(root);
}

/* ---- times ---- */

static bool same_day(const struct tm *a, const struct tm *b)
{
    return a->tm_year == b->tm_year && a->tm_yday == b->tm_yday;
}

void mj_time_short(int32_t t, char *out, size_t n)
{
    time_t tt = (time_t)t, now = time(NULL);
    struct tm a, b;
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    if (!aos_hal_time_is_valid() || same_day(&a, &b)) {
        snprintf(out, n, "%02d:%02d", a.tm_hour, a.tm_min);
        return;
    }
    time_t y = now - 86400;
    struct tm c;
    localtime_r(&y, &c);
    if (same_day(&a, &c)) {
        snprintf(out, n, "%s", _("ayer"));
        return;
    }
    if (now - tt < 6 * 86400) {
        static const char *const D[] = {N_("dom"), N_("lun"), N_("mar"), N_("mié"), N_("jue"), N_("vie"), N_("sáb")};
        snprintf(out, n, "%s", _(D[a.tm_wday]));
        return;
    }
    snprintf(out, n, "%d/%d", a.tm_mday, a.tm_mon + 1);
}

void mj_day_label(int32_t t, char *out, size_t n)
{
    static const char *const M[] = {N_("enero"), N_("febrero"), N_("marzo"), N_("abril"), N_("mayo"), N_("junio"),
                                    N_("julio"), N_("agosto"), N_("septiembre"), N_("octubre"), N_("noviembre"),
                                    N_("diciembre")};
    time_t tt = (time_t)t, now = time(NULL);
    struct tm a, b;
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    if (aos_hal_time_is_valid() && same_day(&a, &b)) {
        snprintf(out, n, "%s", _("Hoy"));
        return;
    }
    time_t y = now - 86400;
    struct tm c;
    localtime_r(&y, &c);
    if (aos_hal_time_is_valid() && same_day(&a, &c)) {
        snprintf(out, n, "%s", _("Ayer"));
        return;
    }
    /* "27 de noviembre": the order and the "de" are the catalogue's */
    snprintf(out, n, _("%d de %s"), a.tm_mday, _(M[a.tm_mon]));
    if (a.tm_year != b.tm_year) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, " %d", a.tm_year + 1900);
    }
}

void mj_status_line(mj_pkey_t k, char *out, size_t n)
{
    out[0] = 0;
    mj_store_lock();
    mj_peer_t *p = mj_store_peer(k);
    if (!p) {
        mj_store_unlock();
        return;
    }
    if (mj_S.typing[0] && mj_pkey_eq(k, mj_S.chat)) {
        if (k.type == MJ_PEER_USER) snprintf(out, n, "%s", _("escribiendo…"));
        else snprintf(out, n, _("%s está escribiendo…"), mj_S.typing);
    } else if (k.type == MJ_PEER_USER) {
        int64_t now = (int64_t)time(NULL);
        if (p->self) snprintf(out, n, "%s", _("tus mensajes guardados"));
        else if (p->bot) snprintf(out, n, "%s", _("bot"));
        else if (p->online_until > 0 && p->online_until > now) snprintf(out, n, "%s", _("en línea"));
        else if (p->last_seen > 0) {
            char t[16];
            mj_time_short(p->last_seen, t, sizeof t);
            snprintf(out, n, _("visto: %s"), t);
        } else snprintf(out, n, "%s", _("visto hace un rato"));
    } else if (p->broadcast) {
        snprintf(out, n, "%s", _("canal"));
    } else {
        snprintf(out, n, "%s", _("grupo"));
    }
    mj_store_unlock();
}

void mj_size_words(int64_t b, char *out, size_t n)
{
    if (b < 1024) snprintf(out, n, "%d B", (int)b);
    else if (b < 1024 * 1024) snprintf(out, n, "%d KB", (int)(b / 1024));
    else snprintf(out, n, "%d.%d MB", (int)(b / (1024 * 1024)), (int)(b % (1024 * 1024) * 10 / (1024 * 1024)));
}

void mj_media_words(const mj_msg_t *m, char *out, size_t n)
{
    out[0] = 0;
    if (!m->media) return;
    const mj_media_t *md = m->media;
    int s = md->duration;
    switch (md->kind) {
    case MJ_MEDIA_PHOTO: snprintf(out, n, "\xF0\x9F\x93\xB7 %s", _("Foto")); break;
    case MJ_MEDIA_VOICE: snprintf(out, n, "\xF0\x9F\x8E\xA4 %s %d:%02d", _("Nota de voz"), s / 60, s % 60); break;
    case MJ_MEDIA_AUDIO: snprintf(out, n, "\xF0\x9F\x8E\xB5 %s", md->title[0] ? md->title : md->name[0] ? md->name : _("Audio")); break;
    case MJ_MEDIA_VIDEO: snprintf(out, n, "\xF0\x9F\x8E\xAC %s %d:%02d", _("Video"), s / 60, s % 60); break;
    case MJ_MEDIA_ROUND: snprintf(out, n, "\xF0\x9F\x8E\xAC %s", _("Videomensaje")); break;
    case MJ_MEDIA_GIF: snprintf(out, n, "GIF"); break;
    case MJ_MEDIA_STICKER: snprintf(out, n, "%s %s", md->emoji, _("Sticker")); break;
    case MJ_MEDIA_DOC: snprintf(out, n, "\xF0\x9F\x93\x8E %s", md->name[0] ? md->name : _("Archivo")); break;
    case MJ_MEDIA_GEO: snprintf(out, n, "\xF0\x9F\x93\x8D %s", md->title[0] ? md->title : _("Ubicación")); break;
    case MJ_MEDIA_CONTACT: snprintf(out, n, "\xF0\x9F\x91\xA4 %s", md->title); break;
    case MJ_MEDIA_POLL: snprintf(out, n, "\xF0\x9F\x93\x8A %s", md->title[0] ? md->title : _("Encuesta")); break;
    case MJ_MEDIA_WEB: break;
    default: snprintf(out, n, "%s", _("Un adjunto que la placa no muestra")); break;
    }
}

void mj_action_words(const mj_msg_t *m, const char *who, char *out, size_t n)
{
    switch (m->action) {
    case MJ_ACT_CREATE: snprintf(out, n, _("%s creó el grupo «%s»"), who, m->action_arg); break;
    case MJ_ACT_TITLE: snprintf(out, n, _("%s cambió el nombre a «%s»"), who, m->action_arg); break;
    case MJ_ACT_PHOTO: snprintf(out, n, _("%s cambió la foto del grupo"), who); break;
    case MJ_ACT_ADD: snprintf(out, n, _("%s sumó a %s"), who, m->action_arg); break;
    case MJ_ACT_LEAVE: snprintf(out, n, _("%s salió del grupo"), m->action_arg[0] ? m->action_arg : who); break;
    case MJ_ACT_JOIN: snprintf(out, n, _("%s se sumó con un enlace"), who); break;
    case MJ_ACT_PIN: snprintf(out, n, _("%s fijó un mensaje"), who); break;
    case MJ_ACT_CALL: snprintf(out, n, "%s", _("Llamada")); break;
    default: snprintf(out, n, "%s", _("Un aviso del grupo")); break;
    }
}

/* ---- sheets ---- */

static void sheet_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) mj_sheet_close();
}

static void sheet_gone(void *p)
{
    (void)p;
}

lv_obj_t *mj_sheet_open(const char *title, int32_t h)
{
    mj_sheet_close();
    mj_kb_hide();
    lv_obj_t *bg = mj_box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_50, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bg, sheet_bg_cb, LV_EVENT_CLICKED, NULL);
    int32_t w = U.land ? 720 : U.W;
    if (h > U.H - 40) h = U.H - 40;
    lv_obj_t *s = mj_box(bg, w, h);
    lv_obj_align(s, U.land ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s, AOS_UI_RADIUS, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = mj_label(s, title, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(t, w - 2 * AOS_UI_PAD);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 22);
    lv_obj_t *body = mj_box(s, w, h - 78);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 74);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(body, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(body, 24, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    U.sheet = bg;
    (void)sheet_gone;
    return body;
}

void mj_sheet_close(void)
{
    if (!U.sheet) return;
    lv_obj_t *s = U.sheet;
    U.sheet = NULL;
    lv_obj_delete_async(s);
}

/* ---- the toasts of the network thread, in words ---- */

void mj_toast_code(int code, const char *arg)
{
    char t[160];
    switch (code) {
    case MJ_T_SEND_FAILED: snprintf(t, sizeof t, "%s", _("No se pudo mandar el mensaje")); break;
    case MJ_T_EDIT_FAILED: snprintf(t, sizeof t, "%s", _("No se pudo editar")); break;
    case MJ_T_DELETE_FAILED: snprintf(t, sizeof t, "%s", _("No se pudo borrar")); break;
    case MJ_T_FORWARDED: snprintf(t, sizeof t, "%s", _("Reenviado")); break;
    case MJ_T_FORWARD_FAILED: snprintf(t, sizeof t, "%s", _("No se pudo reenviar")); break;
    case MJ_T_SAVED: {
        const char *base = arg ? strrchr(arg, '/') : NULL;
        snprintf(t, sizeof t, _("Guardado en la tarjeta: %s"), base ? base + 1 : (arg ? arg : ""));
        break;
    }
    case MJ_T_DOWNLOAD_FAILED: snprintf(t, sizeof t, "%s", _("No se pudo bajar el archivo")); break;
    case MJ_T_PHOTO_FAILED: snprintf(t, sizeof t, "%s", _("No se pudo mandar la foto")); break;
    case MJ_T_PHOTO_TOO_BIG: snprintf(t, sizeof t, "%s", _("La foto pasa de 10 MB")); break;
    case MJ_T_FLOOD: snprintf(t, sizeof t, "%s", _("Telegram pide esperar un poco")); break;
    case MJ_T_LOGGED_OUT: snprintf(t, sizeof t, "%s", _("La sesión se cerró")); break;
    case MJ_T_REACT_FAILED: snprintf(t, sizeof t, "%s", _("No se pudo reaccionar")); break;
    default: return;
    }
    aos_ui_toast(t, 2500);
}

/* ---- the keyboard ---- */

static lv_event_cb_t s_ready;

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY && s_ready) s_ready(e);
    else if (c == LV_EVENT_CANCEL) lv_async_call((lv_async_cb_t)(void (*)(void *))mj_kb_hide, NULL);
}

void mj_kb_hide(void)
{
    if (!U.kb) return;
    lv_obj_t *k = U.kb;
    U.kb = NULL;
    lv_keyboard_set_textarea(k, NULL);
    /* async: this is often called from the keyboard's own READY event, and
     * an object deleted inside its own event corrupts LVGL's memory (a
     * search went out as garbage) */
    lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(k);
    if (U.kb_ta) lv_obj_remove_state(U.kb_ta, LV_STATE_FOCUSED);
    U.kb_ta = NULL;
    if (U.open.type) mj_chat_refresh(true);
}

void mj_kb_show(lv_obj_t *ta, lv_keyboard_mode_t mode, lv_event_cb_t ready)
{
    s_ready = ready;
    if (U.kb) {
        lv_keyboard_set_textarea(U.kb, ta);
        lv_keyboard_set_mode(U.kb, mode);
        U.kb_ta = ta;
        return;
    }
    int32_t h = U.land ? U.H * 46 / 100 : 430;
    U.kb = lv_keyboard_create(U.root);
    lv_obj_set_size(U.kb, U.W, h);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_mode(U.kb, mode);
    lv_keyboard_set_textarea(U.kb, ta);
    lv_obj_add_event_cb(U.kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(U.kb, kb_cb, LV_EVENT_CANCEL, NULL);
    U.kb_ta = ta;
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    if (U.open.type) mj_chat_refresh(true);
}
