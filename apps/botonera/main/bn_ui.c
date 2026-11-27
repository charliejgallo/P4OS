/*
 * Botonera - the grid.
 *
 * Twelve buttons a page (3 x 4 upright, 4 x 3 lying down), a bar on top to
 * go from page to page and to edit, and a strip at the bottom with the pages
 * and "stop everything".
 *
 * Played, the buttons do not go through LVGL's pointer: LVGL follows one
 * finger, and a soundboard is hit with several. A 10 ms timer reads every
 * finger (aos_touch_points_n), as DJ does, and a finger landing on a button
 * plays it there and then, on the touch and not on the release; one lifting
 * off a "hold" button stops it. Edited, the buttons are ordinary LVGL
 * objects that take a click.
 *
 * The sounds are decoded on a loader thread of the app's own, a page at a
 * time, and kept in PSRAM (BN_CACHE_BYTES, the least used go first). A touch
 * on a sound still being decoded is kept and played as soon as it is ready.
 *
 * The portal's page plays the buttons from the phone through the live
 * channel (aos_hal_live_*): it sends "down 5" / "up 5", "stop 5", "stopall",
 * "page 1", "reload", and reads "state", a JSON of what is playing that the
 * app puts while someone looks.
 */
#include "bn_ui.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__riscv)
#define BN_FENCE() __asm__ volatile("fence rw,rw" ::: "memory")
#else
#define BN_FENCE() __sync_synchronize()
#endif

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

bn_pad_t *bn_ui_pad(bn_ui_t *u, int gi)
{
    if (gi < 0 || gi >= BN_PADS) return NULL;
    return &u->g->page[gi / BN_PER_PAGE].pad[gi % BN_PER_PAGE];
}

/* volume 0..100 to a gain, on a curve the ear hears as even */
float bn_gain(int vol)
{
    float v = (float)vol / 100.0f;
    return v * v;
}

/* ---- the loader ---- */

static void loader_main(void *arg)
{
    bn_ui_t *u = arg;
    while (!u->ld_quit) {
        if (u->req_r == u->req_w || u->res_w - u->res_r >= BN_REQ_N) {
            aos_hal_sleep_ms(10);
            continue;
        }
        BN_FENCE();
        bn_req_t q = u->req[u->req_r % BN_REQ_N];
        BN_FENCE();
        u->req_r = u->req_r + 1;
        bn_res_t r = { .slot = q.slot, .clip = NULL };
        if (q.slot < 0) {
            bn_synth_missing();
        } else {
            uint32_t t0 = now_ms();
            r.clip = bn_decode(q.path, r.why, sizeof r.why);
            if (r.clip)
                aos_hal_log(BN_TAG, "%s: %u ms of sound in %u ms", q.path,
                            (unsigned)(r.clip->n / (BN_RATE / 1000)), (unsigned)(now_ms() - t0));
            else
                aos_hal_log(BN_TAG, "%s: not read (%s)", q.path, r.why);
        }
        if (u->ld_quit) {
            bn_clip_free(r.clip);
            break;
        }
        u->res[u->res_w % BN_REQ_N] = r;
        BN_FENCE();
        u->res_w = u->res_w + 1;
    }
    u->ld_done = true;
}

static bool loader_ask(bn_ui_t *u, int slot, const char *path)
{
    if (u->req_w - u->req_r >= BN_REQ_N) return false;
    bn_req_t *q = &u->req[u->req_w % BN_REQ_N];
    q->slot = (int16_t)slot;
    snprintf(q->path, sizeof q->path, "%s", path ? path : "");
    BN_FENCE();
    u->req_w = u->req_w + 1;
    return true;
}

static uint32_t cache_bytes(bn_ui_t *u)
{
    uint32_t b = 0;
    for (int i = 0; i < BN_CACHE_N; i++) if (u->cache[i].clip) b += u->cache[i].clip->n * 2u;
    return b;
}

static bool on_page(bn_ui_t *u, const char *file)
{
    for (int s = 0; s < BN_PER_PAGE; s++)
        if (!strcmp(u->g->page[u->page].pad[s].file, file)) return true;
    return false;
}

static void cache_drop(bn_ui_t *u, int i)
{
    bn_cache_t *c = &u->cache[i];
    bn_clip_free(c->clip);
    memset(c, 0, sizeof *c);
}

/* the least used entry that nothing plays and nothing waits for; with
 * 'spare_page' the current page's are kept */
static int cache_victim(bn_ui_t *u, bool spare_page)
{
    int best = -1;
    for (int i = 0; i < BN_CACHE_N; i++) {
        bn_cache_t *c = &u->cache[i];
        if (c->state == CS_NONE || c->state == CS_QUEUED) continue;
        if (!bn_eng_clip_idle(c->clip) || i == u->preview_slot) continue;
        if (spare_page && on_page(u, c->file)) continue;
        if (best < 0 || c->used < u->cache[best].used) best = i;
    }
    return best;
}

static void file_stat(const char *file, uint32_t *size, uint32_t *mtime)
{
    char path[200];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", bn_dir(), file);
    if (stat(path, &st) == 0) {
        *size = (uint32_t)st.st_size;
        *mtime = (uint32_t)st.st_mtime;
    } else {
        *size = *mtime = 0;
    }
}

int bn_cache_get(bn_ui_t *u, const char *file)
{
    if (!file || !file[0]) return -1;
    for (int i = 0; i < BN_CACHE_N; i++) {
        if (u->cache[i].state != CS_NONE && !strcmp(u->cache[i].file, file)) {
            u->cache[i].used = ++u->clock;
            return i;
        }
    }
    /* room: under the budget, and a free entry */
    while (cache_bytes(u) > BN_CACHE_BYTES) {
        int v = cache_victim(u, true);
        if (v < 0) v = cache_victim(u, false);
        if (v < 0) break;
        cache_drop(u, v);
    }
    int slot = -1;
    for (int i = 0; i < BN_CACHE_N && slot < 0; i++) if (u->cache[i].state == CS_NONE) slot = i;
    if (slot < 0) {
        slot = cache_victim(u, true);
        if (slot < 0) slot = cache_victim(u, false);
        if (slot < 0) return -1;
        cache_drop(u, slot);
    }
    bn_cache_t *c = &u->cache[slot];
    snprintf(c->file, sizeof c->file, "%s", file);
    file_stat(file, &c->size, &c->mtime);
    char path[200];
    snprintf(path, sizeof path, "%s/%s", bn_dir(), file);
    if (!loader_ask(u, slot, path)) {
        memset(c, 0, sizeof *c);
        return -1;
    }
    c->state = CS_QUEUED;
    c->used = ++u->clock;
    return slot;
}

/* a file that changed on the card (uploaded again, recorded over) is read
 * again the next time it is wanted */
static void cache_check(bn_ui_t *u)
{
    bn_files_t f = { 0 };
    if (!bn_files_scan(&f)) return;
    for (int i = 0; i < BN_CACHE_N; i++) {
        bn_cache_t *c = &u->cache[i];
        if (c->state != CS_READY && c->state != CS_FAILED) continue;
        int k = bn_files_find(&f, c->file);
        bool same = k >= 0 && f.size[k] == c->size && f.mtime[k] == c->mtime;
        if (!same && bn_eng_clip_idle(c->clip) && i != u->preview_slot) cache_drop(u, i);
    }
    bn_files_free(&f);
}

static void preload_page(bn_ui_t *u)
{
    for (int s = 0; s < BN_PER_PAGE; s++) {
        const bn_pad_t *p = &u->g->page[u->page].pad[s];
        if (!bn_pad_empty(p)) bn_cache_get(u, p->file);
    }
}

/* ---- playing ---- */

bool bn_ui_engine(bn_ui_t *u, bool on)
{
    (void)u;
    if (on) return bn_eng_start();
    bn_eng_stop();
    return true;
}

static void play_now(bn_ui_t *u, int gi, int slot)
{
    bn_pad_t *p = bn_ui_pad(u, gi);
    bn_cache_t *c = &u->cache[slot];
    if (!p || !c->clip) return;
    if (!bn_eng_running() && !bn_ui_engine(u, true)) {
        aos_ui_toast(_("No se pudo abrir el parlante"), 2000);
        return;
    }
    c->used = ++u->clock;
    bn_eng_play(gi, c->clip, bn_gain(p->vol), p->loop);
    if (c->clip->cut && !u->cut_told) {
        u->cut_told = true;
        char m[96];
        snprintf(m, sizeof m, _("Suenan los primeros %d segundos"), BN_CLIP_MAX_S);
        aos_ui_toast(m, 2500);
    }
}

static void tell_failed(bn_ui_t *u, int slot)
{
    char m[160];
    snprintf(m, sizeof m, _("No se pudo leer %s"), u->cache[slot].file);
    aos_ui_toast(m, 2500);
}

void bn_ui_press(bn_ui_t *u, int gi)
{
    bn_pad_t *p = bn_ui_pad(u, gi);
    if (!p || !u->ready) return;
    if (bn_pad_empty(p)) {
        if (!u->edit) bn_edit_open(u, gi);
        return;
    }
    if (p->mode == BN_MODE_HOLD) {
        u->held[gi] = true;
        bn_eng_stop_pad(gi);
    } else if (p->mode == BN_MODE_CUT || p->loop) {
        if (bn_eng_busy(gi) || u->want[gi]) {
            u->want[gi] = false;
            bn_eng_stop_pad(gi);
            return;
        }
    }
    int slot = bn_cache_get(u, p->file);
    if (slot < 0) return;
    if (u->cache[slot].state == CS_READY) play_now(u, gi, slot);
    else if (u->cache[slot].state == CS_QUEUED) u->want[gi] = true;
    else tell_failed(u, slot);
}

void bn_ui_release(bn_ui_t *u, int gi)
{
    bn_pad_t *p = bn_ui_pad(u, gi);
    if (!p || !u->held[gi]) return;
    u->held[gi] = false;
    if (p->mode == BN_MODE_HOLD) {
        u->want[gi] = false;
        bn_eng_stop_pad(gi);
    }
}

void bn_ui_preview(bn_ui_t *u, const char *file)
{
    bn_eng_stop_pad(-1);
    u->preview_slot = -1;
    if (!file) return;
    int slot = bn_cache_get(u, file);
    if (slot < 0) return;
    u->preview_slot = slot;
    if (u->cache[slot].state == CS_READY) {
        if (bn_eng_running() || bn_ui_engine(u, true)) bn_eng_play(-1, u->cache[slot].clip, 1.0f, false);
    } else if (u->cache[slot].state == CS_FAILED) {
        tell_failed(u, slot);
        u->preview_slot = -1;
    }
}

static void stop_everything(bn_ui_t *u)
{
    memset(u->want, 0, sizeof u->want);
    memset(u->held, 0, sizeof u->held);
    bn_eng_stop_all();
}

static void take_results(bn_ui_t *u)
{
    while (u->res_r != u->res_w) {
        BN_FENCE();
        bn_res_t r = u->res[u->res_r % BN_REQ_N];
        BN_FENCE();
        u->res_r = u->res_r + 1;
        if (r.slot < 0) {
            /* the examples are made: now the grid can be read */
            u->ready = true;
            bn_ui_refresh(u);
            preload_page(u);
            continue;
        }
        bn_cache_t *c = &u->cache[r.slot];
        c->clip = r.clip;
        c->state = r.clip ? CS_READY : CS_FAILED;
        snprintf(c->why, sizeof c->why, "%s", r.why);
        if (r.slot == u->preview_slot && r.clip && (bn_eng_running() || bn_ui_engine(u, true)))
            bn_eng_play(-1, r.clip, 1.0f, false);
        for (int gi = 0; gi < BN_PADS; gi++) {
            if (!u->want[gi]) continue;
            bn_pad_t *p = bn_ui_pad(u, gi);
            if (strcmp(p->file, c->file)) continue;
            u->want[gi] = false;
            if (p->mode == BN_MODE_HOLD && !u->held[gi]) continue;
            if (r.clip) play_now(u, gi, r.slot);
            else tell_failed(u, r.slot);
        }
    }
}

/* ---- drawing a button ---- */

static bool light(uint32_t rgb)
{
    uint32_t r = rgb >> 16 & 0xFF, g = rgb >> 8 & 0xFF, b = rgb & 0xFF;
    return r * 299 + g * 587 + b * 114 > 160000;
}

void bn_draw_pad(lv_obj_t *o, lv_obj_t *icon, lv_obj_t *name, const bn_pad_t *p, bool lit, bool big)
{
    (void)big;
    if (bn_pad_empty(p)) {
        lv_obj_set_style_bg_color(o, AOS_C_CARD, 0);
        lv_obj_set_style_border_color(o, AOS_C_CARD2, 0);
        lv_obj_set_style_border_width(o, 3, 0);
        lv_label_set_text(icon, AOS_SYM_PLUS);
        lv_obj_set_style_text_color(icon, AOS_C_DIM, 0);
        lv_label_set_text(name, _("Vacío"));
        lv_obj_set_style_text_color(name, AOS_C_DIM, 0);
        return;
    }
    lv_color_t c = lv_color_hex(p->color);
    bool dark_text = light(p->color);
    if (lit) {
        lv_obj_set_style_bg_color(o, lv_color_mix(lv_color_white(), c, 90), 0);
        lv_obj_set_style_border_color(o, lv_color_white(), 0);
        lv_obj_set_style_border_width(o, 6, 0);
    } else {
        lv_obj_set_style_bg_color(o, c, 0);
        lv_obj_set_style_border_width(o, 0, 0);
    }
    lv_color_t tc = dark_text ? lv_color_hex(0x111111) : lv_color_white();
    lv_label_set_text(icon, bn_icons[p->icon < bn_n_icons ? p->icon : 0].glyph);
    lv_obj_set_style_text_color(icon, tc, 0);
    char safe[BN_NAME_MAX * 2];
    aos_text_safe(safe, sizeof safe, p->name);
    lv_label_set_text(name, safe);
    lv_obj_set_style_text_color(name, tc, 0);
}

static void tag_text(const bn_pad_t *p, char *out, size_t n)
{
    out[0] = 0;
    if (bn_pad_empty(p)) return;
    size_t k = 0;
    const char *a[3] = { 0 };
    int na = 0;
    if (p->loop) a[na++] = AOS_SYM_RESTART;
    if (p->mode == BN_MODE_OVER && !p->loop) a[na++] = AOS_SYM_CONTENT_COPY;
    if (p->mode == BN_MODE_HOLD) a[na++] = AOS_SYM_GESTURE_TAP_BUTTON;
    for (int i = 0; i < na; i++) {
        size_t l = strlen(a[i]);
        if (k + l + 1 > n) break;
        memcpy(out + k, a[i], l);
        k += l;
    }
    out[k] = 0;
}

static void draw_slot(bn_ui_t *u, int s)
{
    if (!u->pad[s]) return;
    int gi = u->page * BN_PER_PAGE + s;
    const bn_pad_t *p = bn_ui_pad(u, gi);
    bool lit = bn_eng_voices(gi) > 0;
    uint8_t look = lit ? 1 : u->want[gi] ? 2 : 0;
    bn_draw_pad(u->pad[s], u->pad_icon[s], u->pad_name[s], p, lit, !u->land);
    char tag[16];
    tag_text(p, tag, sizeof tag);
    if (u->edit && !bn_pad_empty(p)) snprintf(tag, sizeof tag, "%s", AOS_SYM_PENCIL);
    lv_label_set_text(u->pad_tag[s], tag);
    lv_obj_set_style_text_color(u->pad_tag[s], lv_obj_get_style_text_color(u->pad_name[s], 0), 0);
    lv_obj_set_style_opa(u->pad[s], look == 2 ? LV_OPA_60 : LV_OPA_COVER, 0);
    if (u->move_from == gi) {
        lv_obj_set_style_border_color(u->pad[s], AOS_C_ACCENT, 0);
        lv_obj_set_style_border_width(u->pad[s], 8, 0);
    }
    if (lit) lv_obj_remove_flag(u->pad_bar[s], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(u->pad_bar[s], LV_OBJ_FLAG_HIDDEN);
    u->pad_look[s] = look;
}

static void draw_bar(bn_ui_t *u)
{
    char safe[BN_NAME_MAX * 2], sub[48];
    aos_text_safe(safe, sizeof safe, u->g->page[u->page].name);
    lv_label_set_text(u->title, safe);
    /* edited, the name is underlined: it is tapped to change it */
    lv_obj_set_style_text_decor(u->title, u->edit ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE, 0);
    snprintf(sub, sizeof sub, _("Página %d de %d"), u->page + 1, u->g->n_pages);
    lv_label_set_text(u->sub, sub);
    lv_obj_set_style_opa(u->b_prev, u->page > 0 ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_opa(u->b_next, u->page + 1 < u->g->n_pages ? LV_OPA_COVER : LV_OPA_30, 0);
    if (u->edit) {
        lv_obj_remove_flag(u->b_add, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(u->b_del, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(u->b_add, u->g->n_pages < BN_PAGES ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_opa(u->b_del, u->g->n_pages > 1 ? LV_OPA_COVER : LV_OPA_30, 0);
    } else {
        lv_obj_add_flag(u->b_add, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(u->b_del, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *el = lv_obj_get_child(u->b_edit, 0), *et = lv_obj_get_child(u->b_edit, 1);
    lv_label_set_text(el, u->edit ? AOS_SYM_CHECK : AOS_SYM_PENCIL);
    lv_label_set_text(et, u->edit ? _("Listo") : _("Editar"));
    lv_obj_set_style_bg_color(u->b_edit, u->edit ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_t *dl = lv_obj_get_child(u->b_del, 0);
    lv_obj_set_style_text_color(dl, u->del_armed ? AOS_C_TEXT : AOS_C_RED, 0);
    lv_obj_set_style_bg_color(u->b_del, u->del_armed ? AOS_C_RED : AOS_C_CARD2, 0);
    if (u->title) {
        if (u->edit) lv_obj_add_flag(lv_obj_get_parent(u->title), LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(lv_obj_get_parent(u->title), LV_OBJ_FLAG_CLICKABLE);
    }
}

static void dot_cb(lv_event_t *e);

static void draw_dots(bn_ui_t *u)
{
    lv_obj_clean(u->dots);
    if (u->edit || u->move_from >= 0 || !u->ready) {
        lv_obj_t *l = aos_label(u->dots, !u->ready ? _("Preparando los sonidos de ejemplo...")
                                : u->move_from >= 0 ? _("Tocá dónde ponerlo (también en otra página)")
                                : _("Tocá un botón, o el nombre de la página, para cambiarlo"),
                                aos_font_small, AOS_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        return;
    }
    for (int i = 0; i < u->g->n_pages; i++) {
        lv_obj_t *d = lv_obj_create(u->dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 44, 64);
        lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(d, dot_cb, LV_EVENT_CLICKED, u);
        lv_obj_set_user_data(d, (void *)(intptr_t)i);
        lv_obj_t *c = lv_obj_create(d);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, i == u->page ? 22 : 16, i == u->page ? 22 : 16);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, i == u->page ? AOS_C_TEXT : AOS_C_CARD2, 0);
        lv_obj_center(c);
        aos_make_decorative(c);
    }
}

void bn_ui_refresh(bn_ui_t *u)
{
    if (!u->bar) return;
    for (int s = 0; s < BN_PER_PAGE; s++) {
        draw_slot(u, s);
        if (u->edit) lv_obj_add_flag(u->pad[s], LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(u->pad[s], LV_OBJ_FLAG_CLICKABLE);
    }
    draw_bar(u);
    draw_dots(u);
}

void bn_ui_go_page(bn_ui_t *u, int page)
{
    if (page < 0 || page >= u->g->n_pages) return;
    u->page = page;
    u->del_armed = false;
    bn_ui_refresh(u);
    cache_check(u);
    preload_page(u);
}

void bn_ui_save(bn_ui_t *u)
{
    if (!bn_grid_save(u->g)) aos_ui_toast(_("No se pudo guardar la botonera"), 2000);
    u->grid_mt = bn_grid_mtime();
}

/* ---- events ---- */

static void dot_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    lv_obj_t *d = lv_event_get_current_target(e);
    bn_ui_go_page(u, (int)(intptr_t)lv_obj_get_user_data(d));
}

static void prev_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    bn_ui_go_page(u, u->page - 1);
}

static void next_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    bn_ui_go_page(u, u->page + 1);
}

static void mark_stale(bn_ui_t *u)
{
    /* the finger that tapped "Done" is still down: it is not a touch on the
     * button that happens to be under it */
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    for (int i = 0; i < AOS_TOUCH_MAX; i++) u->stale[i] = pts[i].down ? pts[i].id : 0;
}

static void edit_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    u->edit = !u->edit;
    u->move_from = -1;
    u->del_armed = false;
    if (u->edit) stop_everything(u);
    else mark_stale(u);
    bn_ui_refresh(u);
}

static void stop_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    stop_everything(u);
    bn_ui_preview(u, NULL);
}

static void rename_done(bn_ui_t *u, const char *text)
{
    if (text && text[0]) {
        snprintf(u->g->page[u->page].name, BN_NAME_MAX, "%s", text);
        bn_ui_save(u);
    }
    bn_ui_refresh(u);
}

static void title_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    if (!u->edit) return;
    bn_text_open(u, _("Nombre de la página"), u->g->page[u->page].name, rename_done);
}

static void add_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    if (u->g->n_pages >= BN_PAGES) return;
    bn_page_t *p = &u->g->page[u->g->n_pages];
    memset(p, 0, sizeof *p);
    for (int s = 0; s < BN_PER_PAGE; s++) bn_pad_clear(&p->pad[s]);
    snprintf(p->name, sizeof p->name, _("Página %d"), u->g->n_pages + 1);
    u->g->n_pages++;
    bn_ui_save(u);
    bn_ui_go_page(u, u->g->n_pages - 1);
}

static void del_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    if (u->g->n_pages <= 1) return;
    if (!u->del_armed || now_ms() - u->del_t > 4000) {
        u->del_armed = true;
        u->del_t = now_ms();
        aos_ui_toast(_("Tocá de nuevo para borrar la página (los sonidos quedan en la tarjeta)"), 3000);
        draw_bar(u);
        return;
    }
    stop_everything(u);
    for (int p = u->page; p + 1 < u->g->n_pages; p++) u->g->page[p] = u->g->page[p + 1];
    u->g->n_pages--;
    memset(u->want, 0, sizeof u->want);
    bn_ui_save(u);
    bn_ui_go_page(u, u->page >= u->g->n_pages ? u->g->n_pages - 1 : u->page);
}

static void pad_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target(e);
    int s = (int)(intptr_t)lv_obj_get_user_data(o);
    int gi = u->page * BN_PER_PAGE + s;
    if (!u->edit) return;
    if (u->move_from >= 0) {
        if (gi != u->move_from) {
            bn_pad_t *a = bn_ui_pad(u, u->move_from), *b = bn_ui_pad(u, gi), t = *a;
            *a = *b;
            *b = t;
            bn_ui_save(u);
        }
        u->move_from = -1;
        bn_ui_refresh(u);
        return;
    }
    bn_edit_open(u, gi);
}

/* ---- the fingers ---- */

static int hit(bn_ui_t *u, float x, float y)
{
    for (int s = 0; s < BN_PER_PAGE; s++) {
        lv_area_t a;
        lv_obj_get_coords(u->pad[s], &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) return u->page * BN_PER_PAGE + s;
    }
    return -1;
}

static void release_all(bn_ui_t *u)
{
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        if (u->f[i].id && u->f[i].pad >= 0) bn_ui_release(u, u->f[i].pad);
        u->f[i].id = 0;
        u->f[i].pad = -1;
    }
}

static void touch_timer(lv_timer_t *t)
{
    bn_ui_t *u = lv_timer_get_user_data(t);
    /* a toast is a line at the bottom, not a reason to drop the fingers */
    bool covered = !u->front || u->edit || u->sheet || !u->ready ||
                   (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    if (covered != u->covered) {
        u->covered = covered;
        if (covered) release_all(u);
        else mark_stale(u);
    }
    if (covered) return;
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    for (int s = 0; s < AOS_TOUCH_MAX; s++) {
        bool there = false;
        for (int i = 0; i < AOS_TOUCH_MAX; i++) if (pts[i].down && pts[i].id == u->stale[s]) there = true;
        if (!there) u->stale[s] = 0;
    }
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        for (int s = 0; s < AOS_TOUCH_MAX && id; s++) if (u->stale[s] == id) id = 0;
        bn_finger_t *f = &u->f[i];
        if (id == f->id) continue;
        if (f->id && f->pad >= 0) bn_ui_release(u, f->pad);
        f->id = id;
        f->pad = -1;
        if (id) {
            f->pad = hit(u, pts[i].x, pts[i].y);
            if (f->pad >= 0) bn_ui_press(u, f->pad);
        }
    }
}

/* ---- the portal's page ---- */

static void live_take(bn_ui_t *u)
{
    char msg[AOS_LIVE_MSG_MAX + 1];
    int n;
    while ((n = aos_hal_live_take(BN_APP_ID, msg, AOS_LIVE_MSG_MAX)) > 0) {
        msg[n] = 0;
        char *line = msg;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char cmd[16] = { 0 };
            int arg = -1;
            sscanf(line, "%15s %d", cmd, &arg);
            bool ok = u->front && u->ready && !u->rc_on;
            if (!strcmp(cmd, "reload")) {
                u->t_check = 0;
            } else if (!ok) {
                /* nothing plays while the app is not in front */
            } else if (!strcmp(cmd, "down") && arg >= 0 && arg < BN_PADS && !u->edit) {
                bn_pad_t *p = bn_ui_pad(u, arg);
                if (p && !bn_pad_empty(p)) bn_ui_press(u, arg);
            } else if (!strcmp(cmd, "up") && arg >= 0 && arg < BN_PADS) {
                bn_ui_release(u, arg);
            } else if (!strcmp(cmd, "stop") && arg >= 0 && arg < BN_PADS) {
                u->want[arg] = false;
                bn_eng_stop_pad(arg);
            } else if (!strcmp(cmd, "stopall")) {
                stop_everything(u);
            } else if (!strcmp(cmd, "page") && !u->sheet) {
                bn_ui_go_page(u, arg);
            }
            line = nl ? nl + 1 : NULL;
        }
    }
}

static void live_put(bn_ui_t *u)
{
    if (aos_hal_live_idle_ms(BN_APP_ID) > 4000) return;
    char j[sizeof u->live_last];
    int k = snprintf(j, sizeof j, "{\"front\":%s,\"ready\":%s,\"edit\":%s,\"rec\":%s,\"page\":%d,\"rev\":%u,\"play\":[",
                     u->front ? "true" : "false", u->ready ? "true" : "false", u->edit ? "true" : "false",
                     u->rc_on ? "true" : "false", u->page, (unsigned)u->grid_mt);
    bool first = true;
    for (int gi = 0; gi < BN_PADS && k < (int)sizeof j - 40; gi++) {
        int v = bn_eng_voices(gi);
        if (!v && !u->want[gi]) continue;
        k += snprintf(j + k, sizeof j - (size_t)k, "%s[%d,%d,%d]", first ? "" : ",", gi, v,
                      bn_eng_progress(gi));
        first = false;
    }
    if (k < (int)sizeof j - 4) k += snprintf(j + k, sizeof j - (size_t)k, "]}");
    uint32_t t = now_ms();
    if (!strcmp(j, u->live_last) && t - u->live_t < 1000) return;
    memcpy(u->live_last, j, sizeof j);
    u->live_t = t;
    aos_hal_live_put(BN_APP_ID, "state", "application/json", j, strlen(j));
}

static void ui_timer(lv_timer_t *t)
{
    bn_ui_t *u = lv_timer_get_user_data(t);
    take_results(u);
    live_take(u);
    if (u->bar) {
        for (int s = 0; s < BN_PER_PAGE; s++) {
            int gi = u->page * BN_PER_PAGE + s;
            bool lit = bn_eng_voices(gi) > 0;
            uint8_t look = lit ? 1 : u->want[gi] ? 2 : 0;
            if (look != u->pad_look[s] && u->move_from != gi) draw_slot(u, s);
            if (lit) {
                int pr = bn_eng_progress(gi) / 10;
                if (pr != u->pad_prog[s]) {
                    u->pad_prog[s] = (int16_t)pr;
                    lv_bar_set_value(u->pad_bar[s], pr, LV_ANIM_OFF);
                }
            }
        }
    }
    bn_rec_tick(u);
    live_put(u);
}

/* ---- building ---- */

lv_obj_t *bn_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 80);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, 80, 0);
    lv_obj_set_style_radius(b, 40, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(lv_color_white(), bg, 60), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, text ? 24 : 0, 0);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *g = lv_label_create(b);
    lv_obj_set_style_text_font(g, &aos_sym_28, 0);
    lv_obj_set_style_text_color(g, AOS_C_TEXT, 0);
    lv_label_set_text(g, glyph ? glyph : "");
    if (!glyph) lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_label_set_text(l, text ? text : "");
    if (!text) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return b;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void bn_ui_build(bn_ui_t *u)
{
    lv_obj_t *root = u->root;
    u->sheet = u->sheet2 = NULL;
    u->kb = u->ta = NULL;
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(root);
    int32_t W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    u->land = W > H;
    const int side = 20, gap = u->land ? 14 : 16;
    const int bar_h = u->land ? 96 : 108, foot_h = u->land ? 100 : 112;

    /* the bar */
    u->bar = box(root);
    lv_obj_set_size(u->bar, W, bar_h);
    lv_obj_set_style_pad_hor(u->bar, side, 0);
    lv_obj_set_style_pad_column(u->bar, 12, 0);
    lv_obj_set_flex_flow(u->bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(u->bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    u->b_prev = bn_btn(u->bar, AOS_SYM_CHEVRON_LEFT, NULL, AOS_C_CARD2, prev_cb, u);
    lv_obj_t *tb = box(u->bar);
    lv_obj_set_height(tb, bar_h - 8);
    lv_obj_set_flex_grow(tb, 1);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(tb, title_cb, LV_EVENT_CLICKED, u);
    u->title = aos_label(tb, "", aos_font_title, AOS_C_TEXT);
    lv_label_set_long_mode(u->title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(u->title, lv_pct(100), lv_font_get_line_height(aos_font_title));
    lv_obj_set_style_text_align(u->title, LV_TEXT_ALIGN_CENTER, 0);
    u->sub = aos_label(tb, "", aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(u->sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(u->sub, lv_pct(100), lv_font_get_line_height(aos_font_caption));
    lv_obj_set_style_text_align(u->sub, LV_TEXT_ALIGN_CENTER, 0);
    aos_make_decorative(u->title);
    aos_make_decorative(u->sub);
    u->b_next = bn_btn(u->bar, AOS_SYM_CHEVRON_RIGHT, NULL, AOS_C_CARD2, next_cb, u);
    u->b_add = bn_btn(u->bar, AOS_SYM_PLUS, NULL, AOS_C_CARD2, add_cb, u);
    u->b_del = bn_btn(u->bar, AOS_SYM_DELETE, NULL, AOS_C_CARD2, del_cb, u);
    u->b_edit = bn_btn(u->bar, AOS_SYM_PENCIL, u->land ? _("Editar") : NULL, AOS_C_CARD2, edit_cb, u);
    if (!u->land) {
        /* upright there is no room for the word; the glyph says it */
        lv_obj_add_flag(lv_obj_get_child(u->b_edit, 1), LV_OBJ_FLAG_HIDDEN);
    }

    /* the buttons */
    int cols = u->land ? 4 : BN_COLS_P, rows = u->land ? 3 : BN_ROWS_P;
    int gh = H - bar_h - foot_h;
    u->box = box(root);
    lv_obj_set_size(u->box, W, gh);
    lv_obj_set_pos(u->box, 0, bar_h);
    int cw = (W - 2 * side - (cols - 1) * gap) / cols;
    int ch = (gh - (rows - 1) * gap) / rows;
    bool small = ch < 200;
    for (int s = 0; s < BN_PER_PAGE; s++) {
        lv_obj_t *o = lv_obj_create(u->box);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, cw, ch);
        lv_obj_set_pos(o, side + (s % cols) * (cw + gap), (s / cols) * (ch + gap));
        lv_obj_set_style_radius(o, 28, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(o, true, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(o, (void *)(intptr_t)s);
        lv_obj_add_event_cb(o, pad_cb, LV_EVENT_CLICKED, u);
        lv_obj_set_style_transform_scale(o, 245, LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(o, cw / 2, 0);
        lv_obj_set_style_transform_pivot_y(o, ch / 2, 0);
        u->pad[s] = o;
        lv_obj_t *ic = lv_label_create(o);
        lv_obj_set_style_text_font(ic, small ? &aos_sym_44 : &aos_sym_72, 0);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, small ? -24 : -30);
        u->pad_icon[s] = ic;
        lv_obj_t *nm = lv_label_create(o);
        const lv_font_t *nf = small ? aos_font_small : aos_font_body;
        lv_obj_set_style_text_font(nm, nf, 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(nm, cw - 24, lv_font_get_line_height(nf) * (small ? 1 : 2));
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, small ? -24 : -22);
        u->pad_name[s] = nm;
        lv_obj_t *tg = lv_label_create(o);
        lv_obj_set_style_text_font(tg, &aos_sym_28, 0);
        lv_obj_align(tg, LV_ALIGN_TOP_RIGHT, -14, 12);
        lv_label_set_text(tg, "");
        u->pad_tag[s] = tg;
        lv_obj_t *br = lv_bar_create(o);
        lv_obj_set_size(br, cw - 40, small ? 8 : 10);
        lv_obj_align(br, LV_ALIGN_BOTTOM_MID, 0, small ? -8 : -8);
        lv_bar_set_range(br, 0, 100);
        lv_obj_set_style_bg_color(br, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(br, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_bg_color(br, lv_color_white(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(br, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_add_flag(br, LV_OBJ_FLAG_HIDDEN);
        u->pad_bar[s] = br;
        aos_make_decorative(ic);
        aos_make_decorative(nm);
        aos_make_decorative(tg);
        aos_make_decorative(br);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        u->pad_look[s] = 0xFF;
        u->pad_prog[s] = -1;
    }

    /* the foot: the pages and the big stop */
    lv_obj_t *foot = box(root);
    lv_obj_set_size(foot, W, foot_h);
    lv_obj_set_pos(foot, 0, H - foot_h);
    lv_obj_set_style_pad_hor(foot, side, 0);
    lv_obj_set_style_pad_column(foot, 16, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    u->dots = box(foot);
    lv_obj_set_height(u->dots, foot_h);
    lv_obj_set_flex_grow(u->dots, 1);
    lv_obj_set_flex_flow(u->dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(u->dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    u->b_stop = bn_btn(foot, AOS_SYM_STOP, _("Parar todo"), AOS_C_RED, stop_cb, u);
    lv_obj_set_height(u->b_stop, foot_h - 24);
    lv_obj_set_style_radius(u->b_stop, (foot_h - 24) / 2, 0);

    bn_ui_refresh(u);
}

/* ---- life ---- */

bn_ui_t *bn_ui_new(lv_obj_t *root)
{
    bn_ui_t *u = bn_alloc(sizeof *u);
    if (!u) return NULL;
    u->g = bn_alloc(sizeof *u->g);
    if (!u->g) {
        bn_free(u);
        return NULL;
    }
    u->root = root;
    u->move_from = -1;
    u->preview_slot = -1;
    u->ed_pad = -1;
    u->front = true;
    for (int i = 0; i < AOS_TOUCH_MAX; i++) u->f[i].pad = -1;
    bn_dec_abort = false;

    mkdir(bn_dir(), 0777);
    bool have = bn_grid_load(u->g);
    if (!have) {
        bn_grid_defaults(u->g);
        bn_grid_save(u->g);
    }
    u->grid_mt = bn_grid_mtime();

    u->ld_quit = false;
    u->ld_done = false;
    u->ld_running = aos_hal_thread_start("bn_load", loader_main, u, 16384, 3);
    if (!u->ld_running) aos_hal_log(BN_TAG, "no loader thread");

    /* the examples the grid names and the card does not have are made
     * first, on the loader; until then the grid waits */
    bool missing = false;
    bn_files_t f = { 0 };
    bn_files_scan(&f);
    for (int k = 0; k < bn_n_examples && !missing; k++) {
        if (bn_files_find(&f, bn_examples[k].file) >= 0) continue;
        for (int p = 0; p < u->g->n_pages && !missing; p++)
            for (int s = 0; s < BN_PER_PAGE; s++)
                if (!strcmp(u->g->page[p].pad[s].file, bn_examples[k].file)) missing = true;
    }
    bn_files_free(&f);
    if (missing && u->ld_running) loader_ask(u, -1, NULL);
    else u->ready = true;

    bn_ui_build(u);
    if (u->ready) preload_page(u);
    bn_ui_engine(u, true);
    u->t_touch = lv_timer_create(touch_timer, 10, u);
    u->t_ui = lv_timer_create(ui_timer, 40, u);
    return u;
}

void bn_ui_free(bn_ui_t *u)
{
    if (!u) return;
    if (u->t_touch) lv_timer_delete(u->t_touch);
    if (u->t_ui) lv_timer_delete(u->t_ui);
    if (u->rc_on) aos_hal_rec_stop();
    aos_hal_live_clear(BN_APP_ID);
    /* the loader first: a decode in progress gives up at its next chunk */
    if (u->ld_running) {
        bn_dec_abort = true;
        u->ld_quit = true;
        for (int i = 0; i < 600 && !u->ld_done; i++) aos_hal_sleep_ms(5);
        if (!u->ld_done) aos_hal_log(BN_TAG, "the loader did not stop");
    }
    bn_ui_engine(u, false);
    /* results nobody took */
    while (u->res_r != u->res_w) {
        bn_clip_free(u->res[u->res_r % BN_REQ_N].clip);
        u->res_r++;
    }
    for (int i = 0; i < BN_CACHE_N; i++) bn_clip_free(u->cache[i].clip);
    bn_files_free(&u->files);
    if (u->ld_done || !u->ld_running) {
        bn_free(u->g);
        bn_free(u);
    }
}

void bn_ui_show(bn_ui_t *u)
{
    u->front = true;
    bn_ui_engine(u, true);
    mark_stale(u);
    u->t_check = 0;
    cache_check(u);
    bn_ui_refresh(u);
}

void bn_ui_hide(bn_ui_t *u)
{
    /* nothing goes on sounding behind another app, and the speaker goes
     * back to the music player */
    u->front = false;
    release_all(u);
    stop_everything(u);
    if (u->rc_on) {
        aos_hal_rec_stop();
        u->rc_on = false;
    }
    bn_ui_engine(u, false);
}

void bn_ui_tick(bn_ui_t *u)
{
    /* the portal's page wrote the grid: read it again, unless a sheet is
     * open over it (then when it closes) */
    uint32_t t = now_ms();
    if (u->sheet || t - u->t_check < 1000) return;
    u->t_check = t;
    uint32_t mt = bn_grid_mtime();
    if (mt == u->grid_mt || !mt) return;
    bn_grid_t *g = bn_alloc(sizeof *g);
    if (!g) return;
    if (bn_grid_load(g)) {
        stop_everything(u);
        memcpy(u->g, g, sizeof *g);
        if (u->page >= u->g->n_pages) u->page = u->g->n_pages - 1;
        u->move_from = -1;
        cache_check(u);
        bn_ui_refresh(u);
        preload_page(u);
    }
    bn_free(g);
    u->grid_mt = mt;
}

bool bn_ui_back(bn_ui_t *u)
{
    if (u->sheet2 || u->sheet) {
        bn_sheets_close(u);
        return true;
    }
    if (u->move_from >= 0) {
        u->move_from = -1;
        bn_ui_refresh(u);
        return true;
    }
    if (u->edit) {
        u->edit = false;
        bn_ui_refresh(u);
        return true;
    }
    return false;
}

void bn_ui_relayout(bn_ui_t *u, lv_obj_t *root)
{
    bn_sheets_close(u);
    u->root = root;
    release_all(u);
    bn_ui_build(u);
}
