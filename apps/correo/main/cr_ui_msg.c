/*
 * P4OS - Correo: a message on the screen (cr_ui.h).
 *
 * Each block of the parsed message (cr_mime.c) becomes a span group, or a
 * plain label when it has no styles: headings bigger, quotes indented with
 * a bar per level, list items with their bullet, preformatted text in the
 * mono font. The theme fonts have no bold or italic cut, so bold is the
 * brightest white over a softer grey and italics a cooler grey; links are
 * the accent, underlined, and a touch shows their address as a QR code
 * (the board has no browser). Pictures named in the message are decoded by
 * the network thread (aos_hal_image_decode reads a file and is slow) and
 * kept while the message is open; pictures from the network are not
 * fetched.
 *
 * Attachments are saved to the card's Correo folder; documents open in
 * Oficina when it is installed, pictures in a viewer of their own.
 */
#include "cr_ui.h"

#include "aos_hal.h"
#include "aos_mono.h"

#include <stdio.h>
#include <stdlib.h>

#define BLOCKS_PAGE 300
#define IMG_MAX     64

typedef struct {
    uint16_t *px;
    int       w, h;
    lv_image_dsc_t dsc;
    bool      asked, failed;
    uint32_t  token;
    lv_obj_t *box;
} img_t;

static struct {
    img_t     img[IMG_MAX];
    lv_obj_t *col;
    lv_obj_t *viewer;
    int       shown;           /* blocks built */
    struct {
        lv_span_t *sp;
        int16_t    link;
    } lk[512];
    int       nlk;
    bool      styles;
    lv_style_t st_bold, st_ital, st_link, st_code, st_under, st_head;
    int       att_pick;
} M;

static void styles_init(void)
{
    if (M.styles) return;
    M.styles = true;
    lv_style_init(&M.st_bold);
    lv_style_set_text_color(&M.st_bold, lv_color_hex(0xFFFFFF));
    lv_style_init(&M.st_ital);
    lv_style_set_text_color(&M.st_ital, lv_color_hex(0xA9B8D8));
    lv_style_init(&M.st_link);
    lv_style_set_text_color(&M.st_link, AOS_C_ACCENT);
    lv_style_set_text_decor(&M.st_link, LV_TEXT_DECOR_UNDERLINE);
    lv_style_init(&M.st_code);
    lv_style_set_text_font(&M.st_code, &aos_mono_22);
    lv_style_set_text_color(&M.st_code, lv_color_hex(0xFFD60A));
    lv_style_init(&M.st_under);
    lv_style_set_text_decor(&M.st_under, LV_TEXT_DECOR_UNDERLINE);
}

static void images_free(void)
{
    for (int i = 0; i < IMG_MAX; i++) {
        if (M.img[i].px) aos_hal_image_free(M.img[i].px);
    }
    memset(M.img, 0, sizeof M.img);
}

void cr_msg_close(void)
{
    if (M.viewer) {
        lv_obj_delete(M.viewer);
        M.viewer = NULL;
    }
    images_free();
    cr_msg_free(U.msg);
    U.msg = NULL;
    U.m_token = 0;
    M.col = NULL;
}

void cr_msg_open(int acct, int folder, uint32_t uid)
{
    bool replace = ui_top() == SCR_MSG;
    cr_msg_close();
    U.m_acct = acct;
    U.m_folder = folder;
    U.m_uid = uid;
    U.m_why[0] = '\0';
    cr_job_t *j = cr_job_new(CR_J_OPEN);
    if (j) {
        j->acct = acct;
        j->folder = folder;
        j->uid = uid;
        U.m_token = j->token = ui_token(OWN_MSG);
        cr_job_put(j);
    }
    if (replace) ui_rebuild();
    else ui_go(SCR_MSG);
}

/* ---- the blocks -------------------------------------------------------------------------------- */

static void link_cb(lv_event_t *e)
{
    lv_obj_t *sg = lv_event_get_current_target(e);
    cr_msg_t *m = U.msg;
    if (!m) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_span_t *sp = lv_spangroup_get_span_by_point(sg, &p);
    int link = -1;
    for (int i = 0; i < M.nlk && sp; i++) {
        if (M.lk[i].sp == sp) link = M.lk[i].link;
    }
    if (link < 0) {
        /* not on a link: the first of the block, if it has one */
        for (int i = 0; i < M.nlk; i++) {
            for (uint32_t k = 0; k < lv_spangroup_get_span_count(sg); k++) {
                if (lv_spangroup_get_child(sg, (int32_t)k) == M.lk[i].sp) {
                    link = M.lk[i].link;
                    break;
                }
            }
            if (link >= 0) break;
        }
    }
    if (link >= 0 && link < m->nlinks) ui_qr_sheet(_("Enlace"), m->links[link]);
}

static lv_color_t text_color(int type)
{
    switch (type) {
    case CR_B_QUOTE: return lv_color_hex(0x9E9EA6);
    case CR_B_NOTE: return AOS_C_DIM;
    case CR_B_FWD: return AOS_C_DIM;
    case CR_B_H1:
    case CR_B_H2: return AOS_C_TEXT;
    }
    return lv_color_hex(0xD8D8DE);
}

static const lv_font_t *text_font(int type)
{
    switch (type) {
    case CR_B_H1: return aos_font_title;
    case CR_B_PRE: return &aos_mono_22;
    case CR_B_NOTE:
    case CR_B_FWD: return aos_font_small;
    }
    return aos_font_body;
}

/* The text of a block, as spans. */
static lv_obj_t *spans(lv_obj_t *parent, const cr_block_t *b, int32_t w)
{
    lv_obj_t *sg = lv_spangroup_create(parent);
    lv_obj_set_width(sg, w);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(sg, text_font(b->type), 0);
    lv_obj_set_style_text_color(sg, text_color(b->type), 0);
    lv_obj_set_style_text_line_space(sg, 4, 0);
    bool has_link = false;
    uint32_t pos = 0;
    char tmp[1024];
    for (int k = 0; k <= b->nsp; k++) {
        uint32_t end = k < b->nsp ? b->sp[k].start : b->len;
        /* the plain text before this span, then the span */
        for (int part = 0; part < 2; part++) {
            uint32_t s = part ? (k < b->nsp ? b->sp[k].start : b->len) : pos;
            uint32_t e = part ? (k < b->nsp ? b->sp[k].start + b->sp[k].len : b->len) : end;
            while (s < e) {
                uint32_t take = e - s > sizeof tmp - 1 ? sizeof tmp - 1 : e - s;
                /* never cut a letter */
                while (take < e - s && take && ((unsigned char)b->text[s + take] & 0xC0) == 0x80) take--;
                memcpy(tmp, b->text + s, take);
                tmp[take] = '\0';
                lv_span_t *sp = lv_spangroup_add_span(sg);
                lv_span_set_text(sp, tmp);
                if (part && k < b->nsp) {
                    uint8_t st = b->sp[k].style;
                    lv_style_t *sty = lv_span_get_style(sp);
                    if (st & CR_S_BOLD) lv_style_set_text_color(sty, lv_color_hex(0xFFFFFF));
                    if (st & CR_S_ITAL) lv_style_set_text_color(sty, lv_color_hex(0xA9B8D8));
                    if (st & CR_S_UNDER) lv_style_set_text_decor(sty, LV_TEXT_DECOR_UNDERLINE);
                    if (st & CR_S_CODE) {
                        lv_style_set_text_font(sty, &aos_mono_22);
                        lv_style_set_text_color(sty, lv_color_hex(0xFFD60A));
                    }
                    if (st & CR_S_LINK) {
                        lv_style_set_text_color(sty, AOS_C_ACCENT);
                        lv_style_set_text_decor(sty, LV_TEXT_DECOR_UNDERLINE);
                        if (M.nlk < (int)(sizeof M.lk / sizeof M.lk[0])) {
                            M.lk[M.nlk].sp = sp;
                            M.lk[M.nlk].link = b->sp[k].link;
                            M.nlk++;
                        }
                        has_link = true;
                    }
                }
                s += take;
            }
        }
        if (k < b->nsp) pos = b->sp[k].start + b->sp[k].len;
    }
    lv_spangroup_refresh(sg);
    if (has_link) {
        lv_obj_add_flag(sg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sg, link_cb, LV_EVENT_CLICKED, NULL);
    }
    return sg;
}

static lv_obj_t *text_obj(lv_obj_t *parent, const cr_block_t *b, int32_t w)
{
    if (b->nsp) return spans(parent, b, w);
    lv_obj_t *l = ui_label(parent, b->text, text_font(b->type), text_color(b->type));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_line_space(l, 4, 0);
    return l;
}

static void img_view_close_cb(lv_event_t *e)
{
    (void)e;
    if (M.viewer) {
        lv_obj_delete_async(M.viewer);
        M.viewer = NULL;
    }
}

static void img_view(int a)
{
    if (a < 0 || a >= IMG_MAX || !M.img[a].px) return;
    M.viewer = ui_box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(M.viewer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(M.viewer, LV_OPA_COVER, 0);
    lv_obj_add_flag(M.viewer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(M.viewer, img_view_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *im = lv_image_create(M.viewer);
    lv_image_set_src(im, &M.img[a].dsc);
    /* the decoded size fits the column: shown 1:1, centred */
    lv_obj_center(im);
    if (U.msg && a < U.msg->natt) {
        lv_obj_t *l = ui_label(M.viewer, U.msg->att[a].name, aos_font_small, AOS_C_DIM);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -30);
    }
}

static void img_tap_cb(lv_event_t *e)
{
    img_view((int)(intptr_t)lv_event_get_user_data(e));
}

static void img_show(int a)
{
    img_t *im = &M.img[a];
    if (!im->box) return;
    lv_obj_clean(im->box);
    if (im->px) {
        lv_obj_set_size(im->box, im->w, im->h);
        lv_obj_t *o = lv_image_create(im->box);
        lv_image_set_src(o, &im->dsc);
        lv_obj_add_flag(im->box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(im->box, img_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    } else {
        lv_obj_t *l = ui_label(im->box, im->failed ? _("No se pudo mostrar la imagen") : _("Cargando la imagen..."),
                               aos_font_small, AOS_C_DIM);
        lv_obj_center(l);
    }
}

static void img_ask(int a, int32_t w)
{
    cr_msg_t *m = U.msg;
    img_t *im = &M.img[a];
    if (im->asked || !m) return;
    size_t n = 0;
    uint8_t *d = cr_att_bytes(m, a, &n);
    if (!d) {
        im->failed = true;
        return;
    }
    cr_job_t *j = cr_job_new(CR_J_IMAGE);
    if (!j) {
        cr_free(d);
        return;
    }
    j->in = d;
    j->in_n = n;
    j->arg = w;
    j->arg2 = U.H * 3 / 4;
    j->arg = w;
    j->uid = (uint32_t)a;
    im->token = j->token = ui_token(OWN_IMG);
    im->asked = true;
    cr_job_put(j);
}

void cr_img_job(cr_job_t *j)
{
    int a = (int)j->uid;
    if (a < 0 || a >= IMG_MAX || M.img[a].token != j->token || !U.msg) return;
    img_t *im = &M.img[a];
    if (!j->ok || !j->out) {
        im->failed = true;
    } else {
        im->px = j->out;
        j->out = NULL;
        im->w = j->n & 0xFFFF;
        im->h = (j->n >> 16) & 0xFFFF;
        memset(&im->dsc, 0, sizeof im->dsc);
        im->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        im->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        im->dsc.header.w = (uint32_t)im->w;
        im->dsc.header.h = (uint32_t)im->h;
        im->dsc.header.stride = (uint32_t)im->w * 2;
        im->dsc.data = (const uint8_t *)im->px;
        im->dsc.data_size = (uint32_t)(im->w * im->h * 2);
    }
    img_show(a);
}

static void block_build(lv_obj_t *col, const cr_block_t *b, int32_t w)
{
    switch (b->type) {
    case CR_B_HR: {
        lv_obj_t *l = ui_box(col, w, 2);
        lv_obj_set_style_bg_color(l, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        return;
    }
    case CR_B_IMG: {
        int a = b->img;
        if (a < 0 || a >= IMG_MAX) return;
        img_t *im = &M.img[a];
        im->box = ui_box(col, im->px ? im->w : w, im->px ? im->h : 120);
        if (!im->px && !im->asked && !im->failed) img_ask(a, w);
        img_show(a);
        return;
    }
    case CR_B_QUOTE: {
        int lvl = b->lvl ? b->lvl : 1;
        if (lvl > 4) lvl = 4;
        lv_obj_t *q = ui_box(col, w, LV_SIZE_CONTENT);
        static const uint32_t C[4] = {0x0A84FF, 0x30D158, 0xBF5AF2, 0xFF9F0A};
        lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(q, 4, 0);
        lv_obj_set_style_border_color(q, lv_color_hex(C[(lvl - 1) % 4]), 0);
        lv_obj_set_style_pad_left(q, 18 + (lvl - 1) * 14, 0);
        text_obj(q, b, w - 22 - (lvl - 1) * 14);
        return;
    }
    case CR_B_LI: {
        lv_obj_t *row = ui_box(col, w, LV_SIZE_CONTENT);
        int32_t ind = 10 + b->lvl * 32;
        lv_obj_t *bl = ui_label(row, b->bullet[0] ? b->bullet : "-", aos_font_body, AOS_C_DIM);
        lv_obj_set_pos(bl, ind, 0);
        lv_obj_t *t = text_obj(row, b, w - ind - 50);
        lv_obj_set_pos(t, ind + 44, 0);
        return;
    }
    case CR_B_PRE: {
        lv_obj_t *box = ui_box(col, w, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(box, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 12, 0);
        lv_obj_set_style_pad_all(box, 12, 0);
        text_obj(box, b, w - 24);
        return;
    }
    case CR_B_FWD: {
        lv_obj_t *box = ui_box(col, w, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(box, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 12, 0);
        lv_obj_set_style_pad_all(box, 14, 0);
        text_obj(box, b, w - 28);
        return;
    }
    case CR_B_H1:
    case CR_B_H2: {
        lv_obj_t *t = text_obj(col, b, w);
        lv_obj_set_style_pad_top(t, 10, 0);
        return;
    }
    default: text_obj(col, b, w); return;
    }
}

static void more_blocks_cb(lv_event_t *e);

static void blocks_build(int from)
{
    cr_msg_t *m = U.msg;
    if (!m || !M.col) return;
    int32_t w = lv_obj_get_style_width(M.col, 0) - 2 * AOS_UI_PAD;
    int end = from + BLOCKS_PAGE;
    if (end > m->nb) end = m->nb;
    for (int i = from; i < end; i++) block_build(M.col, &m->b[i], w);
    M.shown = end;
    if (end < m->nb) ui_btn(M.col, NULL, _("Ver el resto del mensaje"), AOS_C_CARD2, more_blocks_cb, NULL);
}

static void more_blocks_cb(lv_event_t *e)
{
    lv_obj_delete(lv_event_get_current_target(e));
    blocks_build(M.shown);
}

/* ---- attachments ------------------------------------------------------------------------------- */

static bool office_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *const X[] = {"pdf", "docx", "doc", "xlsx", "xls", "pptx", "ppt", "odt", "ods",
                                    "odp", "rtf", "csv", "txt", "md", "key", "pages", "numbers"};
    for (size_t i = 0; i < sizeof X / sizeof X[0]; i++) {
        if (!strcasecmp(dot + 1, X[i])) return true;
    }
    return false;
}

/* Where an attachment goes: <card>/Correo/<name>, not over another file. */
static bool save_path(char *out, size_t n, const char *name)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd) return false;
    char dir[200];
    snprintf(dir, sizeof dir, "%s/Correo", sd);
    cr_mkdirs(dir);
    /* the name made safe for FAT */
    char safe[128];
    size_t k = 0;
    for (const char *p = name; *p && k + 1 < sizeof safe; p++) {
        char c = *p;
        if (strchr("/\\:*?\"<>|", c) || (unsigned char)c < 32) c = '_';
        safe[k++] = c;
    }
    safe[k] = '\0';
    if (!safe[0]) cr_copy(safe, sizeof safe, "adjunto");
    snprintf(out, n, "%s/%s", dir, safe);
    for (int i = 2; i < 100 && cr_file_size(out); i++) {
        const char *dot = strrchr(safe, '.');
        int bl = dot ? (int)(dot - safe) : (int)strlen(safe);
        snprintf(out, n, "%s/%.*s (%d)%s", dir, bl, safe, i, dot ? dot : "");
    }
    return true;
}

static int  s_open_after = -1;
static char s_saved[300];

static void att_saved(int a, const char *path, bool open)
{
    char t[300];
    const char *sl = strrchr(path, '/');
    snprintf(t, sizeof t, "%s Correo/%s", _("Guardado en"), sl ? sl + 1 : path);
    aos_ui_toast(t, 2500);
    if (open && U.msg && a < U.msg->natt) {
        if (!aos_ui_open_app_with("aos.oficina", path)) aos_ui_toast(_("Oficina no está instalada"), 2500);
    }
}

static void att_save(int a, bool open)
{
    cr_msg_t *m = U.msg;
    if (!m || a < 0 || a >= m->natt) return;
    const cr_att_t *at = &m->att[a];
    char path[300];
    if (!save_path(path, sizeof path, at->name)) {
        aos_ui_toast(_("No hay tarjeta"), 2000);
        return;
    }
    if (at->truncated || m->partial) {
        /* not all here: straight from the server to the card */
        cr_job_t *j = cr_job_new(CR_J_PART);
        if (!j) return;
        j->acct = m->acct;
        j->folder = m->folder;
        j->uid = m->uid;
        cr_copy(j->text, sizeof j->text, at->part);
        cr_copy(j->path, sizeof j->path, path);
        j->arg = at->enc;
        j->arg2 = a;
        j->token = ui_token(OWN_PART);
        s_open_after = open ? a : -1;
        cr_job_put(j);
        aos_ui_toast(_("Bajando el adjunto..."), 2000);
        return;
    }
    size_t n = 0;
    uint8_t *d = cr_att_bytes(m, a, &n);
    bool ok = d && cr_file_write(path, d, n);
    cr_free(d);
    if (!ok) {
        aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2500);
        return;
    }
    cr_copy(s_saved, sizeof s_saved, path);
    att_saved(a, path, open);
}

void cr_part_job(cr_job_t *j)
{
    if (!j->ok) {
        char t[260];
        snprintf(t, sizeof t, "%s: %s", _("No se pudo bajar el adjunto"), j->why);
        aos_ui_toast(t, 3000);
        return;
    }
    att_saved(j->arg2, j->path, s_open_after == j->arg2);
    s_open_after = -1;
}

static void att_pick_cb(int k, void *ud)
{
    (void)ud;
    int a = M.att_pick;
    cr_msg_t *m = U.msg;
    if (!m || a < 0 || a >= m->natt) return;
    bool img = cr_istarts(m->att[a].type, "image/");
    bool office = office_type(m->att[a].name) && aos_ui_app_find("aos.oficina");
    int i = 0;
    if (img) {
        if (k == i) {
            int32_t w = U.W - 40;
            if (!M.img[a].px && !M.img[a].asked) {
                img_ask(a, w);
                aos_ui_toast(_("Cargando la imagen..."), 1500);
                M.img[a].box = NULL;
            } else if (M.img[a].px) {
                img_view(a);
            }
            return;
        }
        i++;
    }
    if (office) {
        if (k == i) {
            att_save(a, true);
            return;
        }
        i++;
    }
    if (k == i) att_save(a, false);
}

static void att_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    cr_msg_t *m = U.msg;
    if (!m || a >= m->natt) return;
    M.att_pick = a;
    static const char *items[3];
    int n = 0;
    if (cr_istarts(m->att[a].type, "image/")) items[n++] = _("Ver");
    if (office_type(m->att[a].name) && aos_ui_app_find("aos.oficina")) items[n++] = _("Abrir con Oficina");
    items[n++] = _("Guardar en la tarjeta");
    ui_sheet(m->att[a].name, items, n, att_pick_cb, NULL);
}

static const char *att_glyph(const cr_att_t *a)
{
    if (cr_istarts(a->type, "image/")) return AOS_SYM_FILE_IMAGE_OUTLINE;
    if (cr_istarts(a->type, "audio/")) return AOS_SYM_FILE_MUSIC_OUTLINE;
    if (cr_istarts(a->type, "text/")) return AOS_SYM_FILE_DOCUMENT_OUTLINE;
    if (strstr(a->type, "pdf") || strstr(a->type, "document") || strstr(a->type, "sheet")) return AOS_SYM_FILE_DOCUMENT_OUTLINE;
    return AOS_SYM_FILE_OUTLINE;
}

static void atts_build(lv_obj_t *col, int32_t w)
{
    cr_msg_t *m = U.msg;
    int shown = 0;
    for (int i = 0; i < m->natt; i++) {
        if (m->att[i].inl && m->att[i].cid[0]) continue;      /* already in the text */
        if (m->att[i].inl && cr_istarts(m->att[i].type, "image/")) continue;
        if (!shown++) {
            char t[64];
            snprintf(t, sizeof t, "%s", _("Adjuntos"));
            lv_obj_t *l = ui_label(col, t, aos_font_small, AOS_C_DIM);
            lv_obj_set_style_pad_top(l, 16, 0);
        }
        const cr_att_t *a = &m->att[i];
        lv_obj_t *row = ui_box(col, w, 92);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, att_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *g = ui_label(row, att_glyph(a), &aos_sym_28, AOS_C_ACCENT);
        lv_obj_align(g, LV_ALIGN_LEFT_MID, 18, 0);
        char safe[160];
        aos_text_safe(safe, sizeof safe, a->name);
        lv_obj_t *n = ui_label_dots(row, safe, aos_font_body, AOS_C_TEXT, w - 220);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 70, -14);
        char sz[32];
        ui_size(sz, sizeof sz, a->size);
        char info[96];
        snprintf(info, sizeof info, "%s%s", sz, a->truncated ? _(" - se baja al guardarlo") : "");
        lv_obj_t *s = ui_label(row, info, aos_font_caption, AOS_C_DIM);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 70, 22);
        lv_obj_t *d = ui_label(row, AOS_SYM_DOWNLOAD, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, -18, 0);
    }
}

/* ---- the screen -------------------------------------------------------------------------------- */

static void msg_back_cb(lv_event_t *e)
{
    (void)e;
    ui_pop();
}

static bool many_recipients(const cr_msg_t *m)
{
    cr_addr_t ad[4];
    int n = cr_addr_parse(m->to, ad, 4) + cr_addr_parse(m->cc, ad, 4);
    return n > 1;
}

static void act_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    cr_msg_t *m = U.msg;
    if (!m) return;
    switch (k) {
    case 0: cr_compose_reply(m, CR_REPLY); break;
    case 1: cr_compose_reply(m, CR_REPLY_ALL); break;
    case 2: cr_compose_reply(m, CR_FORWARD); break;
    case 3: {
        int a = m->acct, f = m->folder;
        uint32_t u = m->uid;
        ui_pop();
        cr_act_archive(a, f, u);
        break;
    }
    case 4: {
        int a = m->acct, f = m->folder;
        uint32_t u = m->uid;
        ui_pop();
        cr_act_delete(a, f, u);
        break;
    }
    }
}

static void links_pick_cb(int i, void *ud)
{
    (void)ud;
    if (U.msg && i >= 0 && i < U.msg->nlinks) ui_qr_sheet(_("Enlace"), U.msg->links[i]);
}

static void more_pick_cb(int k, void *ud)
{
    (void)ud;
    cr_msg_t *m = U.msg;
    if (!m) return;
    switch (k) {
    case 0:
        cr_act_flags(m->acct, m->folder, m->uid, 0, CR_F_SEEN);
        m->flags &= (uint16_t)~CR_F_SEEN;
        ui_pop();
        break;
    case 1: {
        bool on = m->flags & CR_F_FLAGGED;
        cr_act_flags(m->acct, m->folder, m->uid, on ? 0 : CR_F_FLAGGED, on ? CR_F_FLAGGED : 0);
        m->flags ^= CR_F_FLAGGED;
        ui_rebuild();
        break;
    }
    case 2: cr_act_move_pick(m->acct, m->folder, m->uid); break;
    case 3: {
        static const char *items[40];
        int n = m->nlinks < 40 ? m->nlinks : 40;
        for (int i = 0; i < n; i++) items[i] = m->links[i];
        if (n) ui_sheet(_("Enlaces del mensaje"), items, n, links_pick_cb, NULL);
        else aos_ui_toast(_("El mensaje no tiene enlaces"), 1500);
        break;
    }
    case 4: {
        char path[300];
        char name[160];
        snprintf(name, sizeof name, "%.100s.eml", m->subject[0] ? m->subject : "mensaje");
        if (!save_path(path, sizeof path, name)) {
            aos_ui_toast(_("No hay tarjeta"), 2000);
            break;
        }
        if (cr_file_write(path, m->raw, m->raw_len)) att_saved(-1, path, false);
        else aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2500);
        break;
    }
    }
}

static void more_cb(lv_event_t *e)
{
    (void)e;
    cr_msg_t *m = U.msg;
    if (!m) return;
    static const char *items[5];
    items[0] = _("Marcar como no leído");
    items[1] = m->flags & CR_F_FLAGGED ? _("Quitar la estrella") : _("Destacar");
    items[2] = _("Mover a...");
    items[3] = _("Ver los enlaces");
    items[4] = _("Guardar el mensaje en la tarjeta");
    ui_sheet(NULL, items, 5, more_pick_cb, NULL);
}

static void retry_cb(lv_event_t *e)
{
    (void)e;
    cr_msg_open(U.m_acct, U.m_folder, U.m_uid);
}

static void draft_cb(lv_event_t *e)
{
    (void)e;
    if (U.msg) cr_compose_draft(U.msg);
}

static void star_cb(lv_event_t *e)
{
    (void)e;
    more_pick_cb(1, NULL);
}

static lv_obj_t *tool(lv_obj_t *bar, const char *glyph, const char *text, int k)
{
    lv_obj_t *b = ui_box(bar, LV_SIZE_CONTENT, 100);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, k >= 0 ? act_cb : more_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    ui_label(b, glyph, &aos_sym_28, AOS_C_ACCENT);
    ui_label(b, text, aos_font_caption, AOS_C_ACCENT);
    return b;
}

void cr_msg_build(lv_obj_t *page)
{
    styles_init();
    M.nlk = 0;
    for (int i = 0; i < IMG_MAX; i++) M.img[i].box = NULL;
    int32_t pw = lv_obj_get_style_width(page, 0);
    const char *back = U.left ? NULL : _("Lista");
    if (ui_top() == SCR_MSG && U.depth >= 2 && U.stack[U.depth - 2] == SCR_HOME) back = _("Buzones");
    lv_obj_t *bar = ui_bar(page, back ? back : _("Cerrar"), NULL, msg_back_cb);
    cr_msg_t *m = U.msg;
    int32_t tool_h = 104;
    M.col = ui_col(page, pw, U.H - 96 - (m ? tool_h : 0));
    lv_obj_set_y(M.col, 96);
    lv_obj_set_style_pad_row(M.col, 14, 0);
    int32_t w = pw - 2 * AOS_UI_PAD;
    if (!m) {
        if (U.m_why[0]) {
            ui_label_wrap(M.col, _("No se pudo abrir el mensaje"), aos_font_body, AOS_C_TEXT);
            ui_label_wrap(M.col, U.m_why, aos_font_small, AOS_C_DIM);
            ui_btn(M.col, AOS_SYM_RESTART, _("Probar de nuevo"), AOS_C_CARD2, retry_cb, NULL);
        } else {
            ui_label(M.col, _("Abriendo..."), aos_font_body, AOS_C_DIM);
        }
        return;
    }
    lv_obj_t *r = ui_bar_right(bar);
    ui_icon_btn(r, m->flags & CR_F_FLAGGED ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE, star_cb, NULL);

    /* who, to whom, when */
    cr_addr_t ad[1];
    char from[200] = "", addr[100] = "";
    if (cr_addr_parse(m->from, ad, 1)) {
        aos_text_safe(from, sizeof from, ad[0].name[0] ? ad[0].name : ad[0].addr);
        cr_copy(addr, sizeof addr, ad[0].addr);
    }
    lv_obj_t *head = ui_box(M.col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, 4, 0);
    lv_obj_t *fl = ui_label_dots(head, from, aos_font_body, AOS_C_TEXT, w);
    (void)fl;
    if (strcmp(from, addr)) ui_label_dots(head, addr, aos_font_small, AOS_C_DIM, w);
    cr_addr_t *list = cr_alloc(sizeof(cr_addr_t) * 30);
    if (list) {
        for (int pass = 0; pass < 2; pass++) {
            int n = cr_addr_parse(pass ? m->cc : m->to, list, 30);
            if (!n) continue;
            cr_buf_t b = {0};
            cr_buf_str(&b, pass ? _("Cc:") : _("Para:"));
            for (int i = 0; i < n; i++) {
                char who[100];
                cr_addr_show(who, sizeof who, &list[i]);
                cr_buf_printf(&b, "%s %s", i ? "," : "", who);
            }
            char safe[700];
            aos_text_safe(safe, sizeof safe, b.p ? b.p : "");
            lv_obj_t *l = ui_label_wrap(head, safe, aos_font_caption, AOS_C_DIM);
            lv_obj_set_style_max_height(l, 3 * lv_font_get_line_height(aos_font_caption), 0);
            cr_buf_free(&b);
        }
        cr_free(list);
    }
    char d[80];
    cr_date_long(d, sizeof d, m->date);
    ui_label(head, d, aos_font_caption, AOS_C_DIM);
    char subj[320];
    aos_text_safe(subj, sizeof subj, m->subject[0] ? m->subject : _("(sin asunto)"));
    lv_obj_t *s = ui_label_wrap(M.col, subj, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(s, w);
    lv_obj_t *line = ui_box(M.col, w, 2);
    lv_obj_set_style_bg_color(line, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    cr_lock();
    bool draft = m->acct < CR.na && m->folder < CR.a[m->acct].nf && CR.a[m->acct].f[m->folder].special == CR_SP_DRAFTS;
    cr_unlock();
    if (draft) ui_btn(M.col, AOS_SYM_PENCIL, _("Seguir escribiendo"), AOS_C_ACCENT, draft_cb, NULL);

    blocks_build(0);
    atts_build(M.col, w);
    if (m->remote_img) {
        ui_label_wrap(M.col, _("Las imágenes de internet no se bajan: avisan a quien las manda que abriste el correo."),
                      aos_font_caption, AOS_C_DIM);
    }
    if (m->partial) {
        char sz[32], t[160];
        ui_size(sz, sizeof sz, m->full_size);
        snprintf(t, sizeof t, _("El mensaje pesa %s: se bajó el principio, y los adjuntos se bajan al guardarlos."), sz);
        ui_label_wrap(M.col, t, aos_font_caption, AOS_C_DIM);
    }
    if (m->cut) ui_label_wrap(M.col, _("El mensaje es muy largo: se muestra el principio."), aos_font_caption, AOS_C_DIM);

    /* the tools, along the bottom */
    lv_obj_t *tb = ui_box(page, pw, tool_h);
    lv_obj_set_y(tb, U.H - tool_h);
    lv_obj_set_style_bg_color(tb, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    tool(tb, AOS_SYM_ARROW_LEFT_BOLD, _("Responder"), 0);
    if (many_recipients(m)) tool(tb, AOS_SYM_ARROW_LEFT_BOLD, _("A todos"), 1);
    tool(tb, AOS_SYM_ARROW_RIGHT_BOLD, _("Reenviar"), 2);
    tool(tb, AOS_SYM_PACKAGE_VARIANT, _("Archivar"), 3);
    tool(tb, AOS_SYM_DELETE, _("Borrar"), 4);
    tool(tb, AOS_SYM_DOTS_HORIZONTAL, _("Más"), -1);
}

void cr_msg_job(cr_job_t *j)
{
    if (j->kind != CR_J_OPEN || j->token != U.m_token) return;
    if (!j->ok || !j->out) {
        cr_copy(U.m_why, sizeof U.m_why, j->why[0] ? j->why : "?");
    } else {
        cr_msg_free(U.msg);
        U.msg = j->out;
        j->out = NULL;
        images_free();
    }
    if (ui_top() == SCR_MSG) ui_rebuild();
}

void cr_msg_refresh(void)
{
}
