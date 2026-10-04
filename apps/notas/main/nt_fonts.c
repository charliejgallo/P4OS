/*
 * P4OS - Notes: the four styles of Inter, in four sizes.
 *
 * They come from notas_p4.pak (tools/pack_fonts.py), read a font at a time
 * the first time a note draws with it: a note in plain text only ever loads
 * the regular weight of its sizes. Each font is lv_font_conv's own tables,
 * rebuilt here into an lv_font_fmt_txt_dsc_t in PSRAM, so LVGL draws them
 * with its stock lv_font_get_glyph_dsc_fmt_txt / lv_font_get_bitmap_fmt_txt.
 *
 * Without the pack the firmware's Inter Medium stands in for all four:
 * notes still open and edit, bold is drawn twice a pixel apart (the editor
 * asks nt_font_faux_bold) and italic is upright.
 */
#include "nt.h"
#include "aos_fonts.h"
#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

static const int s_px[NT_SZ_COUNT] = { 28, 24, 36, 48 };

typedef struct {
    uint8_t  style, px;
    uint16_t pad;
    uint32_t offset, length;
} pak_entry_t;

typedef struct {
    lv_font_t                       font;
    lv_font_fmt_txt_dsc_t           dsc;
    lv_font_fmt_txt_kern_classes_t  kern;
    void                           *mem[8];      /* what to free */
} loaded_t;

static pak_entry_t *s_table;
static int          s_count = -1;               /* -1: pack not looked for yet */
static loaded_t    *s_font[4][NT_SZ_COUNT];
static bool         s_failed[4][NT_SZ_COUNT];

int nt_font_px(int size)
{
    return s_px[size & 3];
}

static const lv_font_t *firmware(int size)
{
    switch (size) {
    case NT_SZ_S:  return &aos_inter_24;
    case NT_SZ_L:  return &aos_inter_36;
    case NT_SZ_XL: return &aos_inter_48;
    default:       return &aos_inter_28;
    }
}

static void pak_path(char *out, size_t n)
{
    snprintf(out, n, "%s/notas_p4.pak", aos_hal_path_apps());
}

static void open_table(void)
{
    if (s_count >= 0) return;
    s_count = 0;
    char path[160];
    pak_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        aos_hal_log("notas", "no %s, fonts from the firmware", path);
        return;
    }
    char magic[4];
    uint32_t count = 0;
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "NTF1", 4) == 0 &&
        fread(&count, 4, 1, f) == 1 && count > 0 && count < 64) {
        s_table = nt_alloc(sizeof(pak_entry_t) * count);
        if (s_table && fread(s_table, sizeof(pak_entry_t), count, f) == count) s_count = (int)count;
    }
    fclose(f);
    if (s_count == 0) aos_hal_log("notas", "%s is not a font pack", path);
}

bool nt_fonts_have_pack(void)
{
    open_table();
    return s_count > 0;
}

/* Reads little-endian fields off a cursor, checking the end. */
typedef struct { const uint8_t *p, *end; bool bad; } rd_t;

static const uint8_t *take(rd_t *r, size_t n)
{
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = true; return NULL; }
    const uint8_t *q = r->p;
    r->p += n;
    return q;
}
static uint32_t u8_(rd_t *r)  { const uint8_t *q = take(r, 1); return q ? q[0] : 0; }
static int32_t  i8_(rd_t *r)  { return (int8_t)u8_(r); }
static uint32_t u16_(rd_t *r) { const uint8_t *q = take(r, 2); return q ? (uint32_t)(q[0] | q[1] << 8) : 0; }
static int32_t  i16_(rd_t *r) { return (int16_t)u16_(r); }
static uint32_t u32_(rd_t *r)
{
    const uint8_t *q = take(r, 4);
    return q ? (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24 : 0;
}

static void unload(loaded_t *l)
{
    if (!l) return;
    for (int i = 0; i < 8; i++) nt_free(l->mem[i]);
    nt_free(l);
}

static loaded_t *load(const pak_entry_t *e)
{
    char path[160];
    pak_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *blob = nt_alloc(e->length);
    bool ok = blob && fseek(f, (long)e->offset, SEEK_SET) == 0 &&
              fread(blob, 1, e->length, f) == e->length;
    fclose(f);
    if (!ok) { nt_free(blob); return NULL; }

    loaded_t *l = nt_alloc(sizeof *l);
    if (!l) { nt_free(blob); return NULL; }
    l->mem[0] = blob;

    rd_t r = { blob, blob + e->length, false };
    int32_t line_h = i16_(&r), base = i16_(&r);
    int32_t ul_pos = i8_(&r), ul_th = i8_(&r);
    uint32_t bpp = u8_(&r), has_kern = u8_(&r);
    uint32_t kern_scale = u16_(&r), ncmap = u16_(&r), nglyph = u16_(&r);
    uint32_t left = u8_(&r), right = u8_(&r), bmp_len = u32_(&r);
    const uint8_t *bmp = take(&r, bmp_len);

    lv_font_fmt_txt_glyph_dsc_t *gd = nt_alloc(sizeof(*gd) * (nglyph ? nglyph : 1));
    lv_font_fmt_txt_cmap_t *cm = nt_alloc(sizeof(*cm) * (ncmap ? ncmap : 1));
    l->mem[1] = gd;
    l->mem[2] = cm;
    if (!gd || !cm || r.bad) { unload(l); return NULL; }

    for (uint32_t i = 0; i < nglyph; i++) {
        gd[i].bitmap_index = u32_(&r);
        gd[i].adv_w = u16_(&r);
        gd[i].box_w = u8_(&r);
        gd[i].box_h = u8_(&r);
        gd[i].ofs_x = i8_(&r);
        gd[i].ofs_y = i8_(&r);
    }
    /* The lists get their own aligned arrays: in the blob a u16 can sit on
     * an odd address. */
    size_t lists = 0;
    const uint8_t *cm_start = r.p;
    for (int pass = 0; pass < 2; pass++) {
        r.p = cm_start;
        uint16_t *pool = pass ? l->mem[3] : NULL;
        size_t used = 0;
        for (uint32_t i = 0; i < ncmap && !r.bad; i++) {
            uint32_t start = u32_(&r), len = u16_(&r), gid = u16_(&r), llen = u16_(&r);
            uint32_t type = u8_(&r);
            u8_(&r);
            if (pass) {
                cm[i].range_start = start;
                cm[i].range_length = (uint16_t)len;
                cm[i].glyph_id_start = (uint16_t)gid;
                cm[i].list_length = (uint16_t)llen;
                cm[i].type = (lv_font_fmt_txt_cmap_type_t)type;
            }
            bool sparse = type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL || type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY;
            if (sparse) {
                const uint8_t *q = take(&r, llen * 2);
                if (pass && q) {
                    memcpy(pool + used, q, llen * 2);
                    cm[i].unicode_list = pool + used;
                }
                used += llen;
            }
            if (type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL) {
                const uint8_t *q = take(&r, len);
                if (pass) cm[i].glyph_id_ofs_list = q;
            } else if (type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
                const uint8_t *q = take(&r, llen * 2);
                if (pass && q) {
                    memcpy(pool + used, q, llen * 2);
                    cm[i].glyph_id_ofs_list = pool + used;
                }
                used += llen;
            }
        }
        if (!pass) {
            lists = used;
            l->mem[3] = nt_alloc((lists ? lists : 1) * 2);
            if (!l->mem[3]) { unload(l); return NULL; }
        }
    }
    if (has_kern) {
        const uint8_t *lm = take(&r, nglyph), *rm = take(&r, nglyph);
        const uint8_t *kv = take(&r, left * right);
        l->kern.left_class_mapping = lm;
        l->kern.right_class_mapping = rm;
        l->kern.class_pair_values = (const int8_t *)kv;
        l->kern.left_class_cnt = (uint8_t)left;
        l->kern.right_class_cnt = (uint8_t)right;
    }
    if (r.bad || !bmp) { unload(l); return NULL; }

    l->dsc.glyph_bitmap = bmp;
    l->dsc.glyph_dsc = gd;
    l->dsc.cmaps = cm;
    l->dsc.kern_dsc = has_kern ? &l->kern : NULL;
    l->dsc.kern_scale = (uint16_t)kern_scale;
    l->dsc.cmap_num = ncmap;
    l->dsc.bpp = bpp;
    l->dsc.kern_classes = has_kern ? 1 : 0;
    l->dsc.bitmap_format = LV_FONT_FMT_TXT_PLAIN;

    l->font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    l->font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    l->font.line_height = line_h;
    l->font.base_line = base;
    l->font.subpx = LV_FONT_SUBPX_NONE;
    l->font.underline_position = (int8_t)ul_pos;
    l->font.underline_thickness = (int8_t)ul_th;
    l->font.dsc = &l->dsc;
    l->font.fallback = NULL;
    return l;
}

const lv_font_t *nt_font(int style, int size)
{
    style &= 3;
    size &= 3;
    if (s_font[style][size]) return &s_font[style][size]->font;
    if (!s_failed[style][size]) {
        open_table();
        for (int i = 0; i < s_count; i++) {
            if (s_table[i].style == style && s_table[i].px == s_px[size]) {
                s_font[style][size] = load(&s_table[i]);
                /* what the pack lacks (LV_SYMBOL_*, Greek...) the firmware's has */
                if (s_font[style][size]) s_font[style][size]->font.fallback = firmware(size);
                break;
            }
        }
        if (s_font[style][size]) return &s_font[style][size]->font;
        s_failed[style][size] = true;
    }
    /* the regular weight from the pack beats the firmware's, if there */
    if (style && s_font[0][size]) return &s_font[0][size]->font;
    if (style && !s_failed[0][size]) return nt_font(0, size);
    return firmware(size);
}

bool nt_font_faux_bold(int style, int size)
{
    if (!(style & NT_ST_BOLD)) return false;
    nt_font(style, size);
    return s_font[style & 3][size & 3] == NULL;
}

void nt_fonts_free(void)
{
    for (int s = 0; s < 4; s++) {
        for (int z = 0; z < NT_SZ_COUNT; z++) {
            unload(s_font[s][z]);
            s_font[s][z] = NULL;
            s_failed[s][z] = false;
        }
    }
    nt_free(s_table);
    s_table = NULL;
    s_count = -1;
}
