/*
 * PLANEO - words in the frame (see vu_text.h)
 */
#include "vu_text.h"
#include "vu_mem.h"
#include "vu_render.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "lvgl.h"

#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

static const char *const WORD[VT_WORDS] = {
    N_("ANILLOS"), N_("CARRERA"), N_("ENTREGAS"), N_("VUELO LIBRE"),
    N_("¡YA!"), N_("¡LLEGASTE!"), N_("¡GANASTE!"), N_("TE GANÓ"), N_("¡PAQUETE!"), N_("¡ENTREGADO!"),
    N_("¡PÉRDIDA!"), N_("¡CHOQUE!"), N_("¡RÉCORD!"), N_("km/h"), N_("m"), N_("1.º"), N_("2.º"),
    N_("¡MUY BAJO!"), N_("Buscá el paquete"), N_("Llevalo a destino"), N_("ÚLTIMO"),
};

static bool make(vt_mask_t *m, const char *txt, const lv_font_t *font)
{
    memset(m, 0, sizeof *m);
    lv_point_t sz;
    lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x + 2, h = sz.y;
    if (w <= 2 || h <= 0) return false;
    lv_draw_buf_t *db = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_L8, 0);
    if (!db) return false;
    lv_obj_t *c = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(c, db);
    lv_canvas_fill_bg(c, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_hex(0xFFFFFF);
    d.font = font;
    d.text = txt;
    d.text_local = 1;
    lv_area_t area = { 1, 0, w - 1, h - 1 };
    lv_draw_label(&layer, &d, &area);
    lv_canvas_finish_layer(c, &layer);
    m->a = (uint8_t *)vu_malloc((size_t)w * h);
    if (m->a) {
        m->w = (int16_t)w;
        m->h = (int16_t)h;
        for (int y = 0; y < h; y++) memcpy(m->a + (size_t)y * w, db->data + (size_t)y * db->header.stride, (size_t)w);
    }
    lv_obj_delete(c);
    lv_draw_buf_destroy(db);
    return m->a != NULL;
}

bool vt_build(vt_set_t *t, int H)
{
    vt_free(t);
    const lv_font_t *f[VT_SIZES];
    if (H <= 180) {
        f[0] = &aos_inter_14;
        f[1] = &aos_inter_16;
        f[2] = &aos_inter_24;
    } else if (H <= 240) {
        f[0] = &aos_inter_14;
        f[1] = &aos_inter_20;
        f[2] = &aos_inter_32;
    } else if (H <= 288) {
        f[0] = &aos_inter_16;
        f[1] = &aos_inter_24;
        f[2] = &aos_inter_36;
    } else {
        f[0] = &aos_inter_20;
        f[1] = &aos_inter_28;
        f[2] = &aos_inter_48;
    }
    for (int s = 0; s < VT_SIZES; s++) {
        for (int i = 0; i < VT_WORDS; i++) make(&t->word[s][i], _(WORD[i]), f[s]);
        for (int i = 0; i < VT_NDIG; i++) {
            char c[2] = { VT_DIGITS[i], 0 };
            make(&t->dig[s][i], c, f[s]);
        }
    }
    t->frame_h = H;
    t->ok = true;
    return true;
}

void vt_free(vt_set_t *t)
{
    for (int s = 0; s < VT_SIZES; s++) {
        for (int i = 0; i < VT_WORDS; i++) vu_free(t->word[s][i].a);
        for (int i = 0; i < VT_NDIG; i++) vu_free(t->dig[s][i].a);
    }
    memset(t, 0, sizeof *t);
}

/* --------------------------------------------------------------------------
 * Drawing
 * -------------------------------------------------------------------------- */

static void blit(uint16_t *out, int W, int H, const vt_mask_t *m, int x0, int y0, uint16_t col)
{
    /* the edge: every covered pixel darkens its neighbours, then the text */
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < m->h; y++) {
            int oy = y0 + y;
            const uint8_t *r = m->a + (size_t)y * m->w;
            for (int x = 0; x < m->w; x++) {
                int a = r[x];
                if (a < 24) continue;
                int ox = x0 + x;
                if (pass == 0) {
                    for (int dy = -1; dy <= 1; dy++) {
                        int py = oy + dy;
                        if ((unsigned)py >= (unsigned)H) continue;
                        uint16_t *o = out + (size_t)py * W;
                        for (int dx = -1; dx <= 1; dx++) {
                            int px = ox + dx;
                            if ((unsigned)px >= (unsigned)W) continue;
                            o[px] = vr_blend(o[px], 0x0841, (a * 22) >> 8);
                        }
                    }
                } else if ((unsigned)oy < (unsigned)H && (unsigned)ox < (unsigned)W) {
                    uint16_t *o = out + (size_t)oy * W + ox;
                    *o = vr_blend(*o, col, (a + 4) >> 3);
                }
            }
        }
    }
}

int vt_draw(uint16_t *out, int W, int H, const vt_mask_t *m, int x, int y, uint16_t col, int align)
{
    if (!m || !m->a) return 0;
    if (align == VT_CENTER) x -= m->w / 2;
    else if (align == VT_RIGHT) x -= m->w;
    blit(out, W, H, m, x, y, col);
    return m->w;
}

static const vt_mask_t *dig(const vt_set_t *t, int size, char c)
{
    const char *p = strchr(VT_DIGITS, c);
    if (!p || !c) return NULL;
    return &t->dig[size][p - VT_DIGITS];
}

int vt_num_w(const vt_set_t *t, int size, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        const vt_mask_t *m = dig(t, size, *s);
        if (m && m->a) w += m->w - 1;
        else if (*s == ' ') w += t->dig[size][0].w / 2;
    }
    return w;
}

int vt_num(uint16_t *out, int W, int H, const vt_set_t *t, int size, const char *s, int x, int y, uint16_t col,
           int align)
{
    if (!t->ok) return 0;
    int w = vt_num_w(t, size, s);
    if (align == VT_CENTER) x -= w / 2;
    else if (align == VT_RIGHT) x -= w;
    for (; *s; s++) {
        const vt_mask_t *m = dig(t, size, *s);
        if (m && m->a) {
            blit(out, W, H, m, x, y, col);
            x += m->w - 1;
        } else if (*s == ' ') {
            x += t->dig[size][0].w / 2;
        }
    }
    return w;
}
