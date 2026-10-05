/*
 * P4OS - EEPROM: the Memory page, the image in hex and ASCII.
 *
 * The view is one scrollable object with a tall transparent spacer inside
 * (a row of the image per row_h pixels, so LVGL's scrolling and its momentum
 * work as on any list) and the rows drawn by hand in its DRAW_MAIN event:
 * only the ones on screen, in runs of one colour. A 16 MB flash is two
 * million rows and costs no more than a 128-byte 24LC01. A thin slider on the
 * right jumps anywhere at once. A label per row would not do: LVGL's recolor
 * cannot show a '#', and a few hundred objects rebuilt on every scroll step
 * are slow on the board.
 *
 * Colours: the bytes edited and not written in orange, the ones that differ
 * from the version being compared in red, 00 and FF dimmed, the cursor on
 * blue and a search hit on amber.
 *
 * Tapping a byte opens the editor: a hex keypad (two digits a byte, the
 * cursor moves on), back and forth, text typed at the cursor.
 */
#include "ee.h"

#define SCRUB_W   40
#define C_DIMHEX  lv_color_hex(0x636366)
#define C_MARK    lv_color_hex(0x7A4D00)
#define C_DIFFBG  lv_color_hex(0x4A1512)

static struct {
    lv_obj_t *view, *spacer, *scrub, *info1, *info2, *ed_val, *empty;
    const lv_font_t *font;
    int32_t cw, rh, pad;
    int bpr, adig, col_hex, col_asc;
    uint32_t rows;
    bool editing;
    int nib;                    /* the high half typed, -1 none */
    bool scrub_lock;
    uint32_t top_row;           /* kept across rebuilds */
    uint8_t pat[48];
    int patn;
    char pat_show[64];
} M = { .nib = -1 };

static void info_refresh(void);
static void ed_refresh(void);

void ee_pg_mem_forget(void)
{
    M.view = M.spacer = M.scrub = M.info1 = M.info2 = M.ed_val = M.empty = NULL;
}

/* -------------------------------------------------------------------------- */
/* Geometry                                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t img_size(void) { return S.valid ? S.size : ee_cur()->size; }

static void metrics(void)
{
    M.bpr = U.land ? 16 : 8;
    M.font = U.land ? &aos_mono_18 : &aos_mono_22;
    M.cw = lv_font_get_glyph_width(M.font, '0', '0');
    M.rh = lv_font_get_line_height(M.font) + (U.land ? 6 : 10);
    M.pad = 12;
    M.adig = img_size() > 0x10000 ? 6 : 4;
    M.col_hex = M.adig + 2;
    M.col_asc = M.col_hex + M.bpr * 3 - 1 + (M.bpr == 16) + 2;
    M.rows = (img_size() + (uint32_t)M.bpr - 1) / (uint32_t)M.bpr;
}

static int hex_col(int j) { return M.col_hex + j * 3 + (M.bpr == 16 && j >= 8); }

static int32_t view_w(void) { return (M.col_asc + M.bpr) * M.cw + 2 * M.pad; }

static int visible_rows(void)
{
    if (!M.view) return 1;
    int n = (int)((lv_obj_get_height(M.view) - 2 * M.pad) / M.rh);
    return n > 1 ? n : 1;
}

static int32_t max_scroll(void)
{
    if (!M.view) return 0;
    int32_t m = (int32_t)M.rows * M.rh + 2 * M.pad - lv_obj_get_height(M.view);
    return m > 0 ? m : 0;
}

static void scroll_row(uint32_t row)
{
    if (!M.view) return;
    int32_t y = (int32_t)row * M.rh;
    if (y > max_scroll()) y = max_scroll();
    lv_obj_scroll_to_y(M.view, y, LV_ANIM_OFF);
}

static void ensure_visible(uint32_t a)
{
    if (!M.view) return;
    uint32_t row = a / (uint32_t)M.bpr;
    uint32_t top = (uint32_t)(lv_obj_get_scroll_y(M.view) / M.rh);
    int vis = visible_rows();
    if (row < top || row >= top + (uint32_t)vis - 1) scroll_row(row > (uint32_t)vis / 2 ? row - (uint32_t)vis / 2 : 0);
}

void ee_pg_mem_goto(uint32_t addr, uint32_t len)
{
    S.cursor = addr;
    S.mark_len = len;
    ensure_visible(addr);
    if (M.view) lv_obj_invalidate(M.view);
}

/* -------------------------------------------------------------------------- */
/* Drawing the rows                                                            */
/* -------------------------------------------------------------------------- */

static void put(lv_layer_t *layer, int32_t x, int32_t y, const char *t, lv_color_t c)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = M.font;
    d.color = c;
    d.text = t;
    d.text_local = 1;
    lv_area_t a = { x, y, x + (int32_t)strlen(t) * M.cw + M.cw, y + M.rh };
    lv_draw_label(layer, &d, &a);
}

static void box_at(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    d.radius = 4;
    int32_t top = y + (M.rh - lv_font_get_line_height(M.font)) / 2 - 2;
    lv_area_t a = { x, top, x + w - 1, top + lv_font_get_line_height(M.font) + 3 };
    lv_draw_rect(layer, &d, &a);
}

enum { K_NORM, K_DIM, K_EDIT, K_DIFF, K_CUR };

static int kind(uint32_t a)
{
    if (a == S.cursor && M.editing) return K_CUR;
    if (ee_is_dirty(a)) return K_EDIT;
    if (S.ref && (a >= S.ref_size || S.ref[a] != S.img[a])) return K_DIFF;
    return S.img[a] == 0x00 || S.img[a] == 0xFF ? K_DIM : K_NORM;
}

static lv_color_t kind_color(int k)
{
    switch (k) {
    case K_DIM:  return C_DIMHEX;
    case K_EDIT: return C_EDIT;
    case K_DIFF: return C_DIFF;
    case K_CUR:  return lv_color_white();
    }
    return AOS_C_TEXT;
}

static void draw_row(lv_layer_t *layer, uint32_t r, int32_t x0, int32_t y)
{
    uint32_t base = r * (uint32_t)M.bpr;
    char t[96];
    snprintf(t, sizeof t, "%0*X", M.adig, (unsigned)base);
    put(layer, x0, y, t, AOS_C_DIM);

    int n = M.bpr;
    if (base + (uint32_t)n > S.size) n = (int)(S.size - base);
    /* backgrounds first: cursor, search hit, difference */
    for (int j = 0; j < n; j++) {
        uint32_t a = base + (uint32_t)j;
        lv_color_t bg;
        if (a == S.cursor && M.editing) bg = AOS_C_ACCENT;
        else if (S.mark_len && a >= S.cursor && a < S.cursor + S.mark_len) bg = C_MARK;
        else if (S.ref && (a >= S.ref_size || S.ref[a] != S.img[a])) bg = C_DIFFBG;
        else continue;
        box_at(layer, x0 + hex_col(j) * M.cw - M.cw / 3, y, 2 * M.cw + 2 * M.cw / 3, bg);
        box_at(layer, x0 + (M.col_asc + j) * M.cw, y, M.cw, bg);
    }
    /* the hex, a run per colour */
    int j = 0;
    while (j < n) {
        int k = kind(base + (uint32_t)j), j0 = j;
        size_t len = 0;
        while (j < n && kind(base + (uint32_t)j) == k) {
            if (j > j0) {
                int gap = hex_col(j) - hex_col(j - 1) - 2;
                while (gap-- > 0 && len + 1 < sizeof t) t[len++] = ' ';
            }
            len += (size_t)snprintf(t + len, sizeof t - len, "%02X", S.img[base + (uint32_t)j]);
            j++;
        }
        t[len] = 0;
        put(layer, x0 + hex_col(j0) * M.cw, y, t, kind_color(k));
    }
    /* the ASCII, the same way */
    j = 0;
    while (j < n) {
        int k = kind(base + (uint32_t)j), j0 = j;
        size_t len = 0;
        while (j < n && kind(base + (uint32_t)j) == k) {
            uint8_t b = S.img[base + (uint32_t)j];
            t[len++] = b >= 0x20 && b < 0x7F ? (char)b : '.';
            j++;
        }
        t[len] = 0;
        put(layer, x0 + (M.col_asc + j0) * M.cw, y, t, k == K_NORM ? AOS_C_TEXT : kind_color(k));
    }
}

static void draw_cb(lv_event_t *e)
{
    if (!S.valid || !S.img) return;
    lv_obj_t *v = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(v, &a);
    int32_t sy = lv_obj_get_scroll_y(v);
    uint32_t first = sy > 0 ? (uint32_t)(sy / M.rh) : 0;
    int32_t y = a.y1 + M.pad + (int32_t)first * M.rh - sy;
    for (uint32_t r = first; r < M.rows && y < a.y2; r++, y += M.rh) draw_row(layer, r, a.x1 + M.pad, y);
}

static void scroll_cb(lv_event_t *e)
{
    (void)e;
    if (!M.view) return;
    int32_t sy = lv_obj_get_scroll_y(M.view);
    M.top_row = sy > 0 ? (uint32_t)(sy / M.rh) : 0;
    int32_t ms = max_scroll();
    if (M.scrub && !M.scrub_lock && ms > 0) {
        M.scrub_lock = true;
        lv_slider_set_value(M.scrub, 1000 - (int32_t)((int64_t)sy * 1000 / ms), LV_ANIM_OFF);
        M.scrub_lock = false;
    }
}

static void scrub_cb(lv_event_t *e)
{
    (void)e;
    if (!M.view || M.scrub_lock) return;
    int32_t v = 1000 - lv_slider_get_value(M.scrub);
    M.scrub_lock = true;
    lv_obj_scroll_to_y(M.view, (int32_t)((int64_t)max_scroll() * v / 1000), LV_ANIM_OFF);
    M.scrub_lock = false;
}

/* -------------------------------------------------------------------------- */
/* Touching a byte, and the editor                                             */
/* -------------------------------------------------------------------------- */

static void open_editor(bool on)
{
    M.editing = on;
    M.nib = -1;
    ee_rebuild();
    if (on) ensure_visible(S.cursor);
}

static void click_cb(lv_event_t *e)
{
    (void)e;
    if (!S.valid || !M.view) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(M.view, &a);
    int32_t y = p.y - a.y1 - M.pad + lv_obj_get_scroll_y(M.view);
    int32_t col = (p.x - a.x1 - M.pad) / M.cw;
    if (y < 0 || col < 0) return;
    uint32_t r = (uint32_t)(y / M.rh);
    int j = -1;
    if (col >= M.col_asc && col < M.col_asc + M.bpr) j = col - M.col_asc;
    for (int k = 0; j < 0 && k < M.bpr; k++)
        if (col >= hex_col(k) && col <= hex_col(k) + 2) j = k;
    if (j < 0) return;
    uint32_t addr = r * (uint32_t)M.bpr + (uint32_t)j;
    if (addr >= S.size) return;
    S.cursor = addr;
    S.mark_len = 0;
    if (!M.editing && !J.busy) { open_editor(true); return; }
    M.nib = -1;
    ed_refresh();
    lv_obj_invalidate(M.view);
}

static void ed_refresh(void)
{
    if (!M.ed_val || !S.valid) return;
    uint8_t b = S.img[S.cursor];
    char ch = b >= 0x20 && b < 0x7F ? (char)b : '.';
    if (M.nib >= 0) lv_label_set_text_fmt(M.ed_val, "0x%0*X   %X_   '%c'", M.adig, (unsigned)S.cursor, M.nib, ch);
    else lv_label_set_text_fmt(M.ed_val, "0x%0*X   %02X   '%c'", M.adig, (unsigned)S.cursor, b, ch);
}

static void moved(void)
{
    M.nib = -1;
    S.mark_len = 0;
    ensure_visible(S.cursor);
    ed_refresh();
    info_refresh();
    if (M.view) lv_obj_invalidate(M.view);
}

static void text_done(const char *text, void *ud)
{
    (void)ud;
    for (const char *p = text; *p && S.cursor < S.size; p++) {
        ee_poke(S.cursor, (uint8_t)*p);
        if (S.cursor + 1 < S.size) S.cursor++;
    }
    moved();
}

static void ed_key_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || J.busy) return;
    if (id < 16) {
        if (M.nib < 0) {
            M.nib = (int)id;
            ed_refresh();
            return;
        }
        ee_poke(S.cursor, (uint8_t)(M.nib << 4 | (int)id));
        if (S.cursor + 1 < S.size) S.cursor++;
    } else if (id == 16) {
        if (S.cursor) S.cursor--;
    } else if (id == 17) {
        if (S.cursor + 1 < S.size) S.cursor++;
    } else if (id == 18) {
        char t[48];
        snprintf(t, sizeof t, _("Texto desde 0x%X"), (unsigned)S.cursor);
        ee_ask_text(t, "", 64, text_done, NULL);
        return;
    } else {
        open_editor(false);
        return;
    }
    moved();
}

static void build_editor(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *k = ee_box(parent, w, h);
    lv_obj_set_style_bg_color(k, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(k, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(k, 16, 0);
    M.ed_val = aos_label(k, "", &aos_mono_22, C_AMBER);
    lv_obj_align(M.ed_val, LV_ALIGN_TOP_LEFT, 8, 4);
    ed_refresh();
    static const char *map[32];
    static const char *const D[16] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F" };
    int n = 0;
    for (int i = 0; i < 16; i++) {
        map[n++] = D[i];
        if (i % 4 == 3) map[n++] = "\n";
    }
    map[n++] = "<";
    map[n++] = ">";
    map[n++] = _("Texto");
    map[n++] = _("Listo");
    map[n] = "";
    lv_obj_t *m = lv_buttonmatrix_create(k);
    lv_buttonmatrix_set_map(m, map);
    lv_obj_set_size(m, w - 32, h - 32 - 48);
    lv_obj_align(m, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(m, aos_font_title, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 8, 0);
    lv_obj_set_style_bg_color(m, AOS_C_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, AOS_C_DIM, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_buttonmatrix_set_button_ctrl(m, 18, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_buttonmatrix_set_button_ctrl(m, 19, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_style_bg_color(m, AOS_C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(m, aos_font_body, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, ed_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

bool ee_pg_mem_back(void)
{
    if (!M.editing) return false;
    open_editor(false);
    return true;
}

/* -------------------------------------------------------------------------- */
/* The info lines                                                              */
/* -------------------------------------------------------------------------- */

static void next_diff(int dir)
{
    if (!S.ref || !S.valid || !S.ndiff) return;
    uint32_t n = S.size;
    for (uint32_t k = 1; k <= n; k++) {
        uint32_t i = dir > 0 ? (S.cursor + k) % n : (S.cursor + n - k) % n;
        if (i >= S.ref_size || S.img[i] != S.ref[i]) {
            uint32_t len = 1;
            while (i + len < n && (i + len >= S.ref_size || S.img[i + len] != S.ref[i + len])) len++;
            ee_pg_mem_goto(i, len);
            info_refresh();
            return;
        }
    }
}

static void diff_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (d) { next_diff(d); return; }
    ee_ref_clear();
    ee_rebuild();
}

static void sums_cb(lv_event_t *e);

static void info_refresh(void)
{
    if (!M.info1) return;
    const ee_chip_t *c = ee_cur();
    if (J.busy && (J.job == JOB_READ || J.job == JOB_WRITE)) {
        lv_label_set_text(M.info1, J.job == JOB_READ ? _("Leyendo el chip…") : _("Escribiendo el chip…"));
    } else if (!S.valid) {
        lv_label_set_text_fmt(M.info1, "%s · %s", c->name, _("sin leer"));
    } else {
        lv_label_set_text_fmt(M.info1, "%s · %s · CRC32 %08X", S.img_chip, S.img_from, (unsigned)S.sums.crc);
    }
    if (!M.info2) return;
    char t[160];
    t[0] = 0;
    if (S.valid && S.img_chip[0] && strcmp(S.img_chip, c->name))
        snprintf(t, sizeof t, _("Ojo: la imagen es de un %s y el chip elegido es un %s."), S.img_chip, c->name);
    else if (S.ndirty == 1)
        snprintf(t, sizeof t, "%s", _("1 byte cambiado sin escribir"));
    else if (S.ndirty)
        snprintf(t, sizeof t, _("%u bytes cambiados sin escribir"), (unsigned)S.ndirty);
    else if (S.ref)
        snprintf(t, sizeof t, _("Comparando con %s: %u diferencias"), S.ref_name, (unsigned)S.ndiff);
    lv_label_set_text(M.info2, t);
    lv_obj_set_style_text_color(M.info2, S.ndirty ? C_EDIT : S.ref ? C_DIFF : AOS_C_DIM, 0);
    if (t[0]) lv_obj_remove_flag(M.info2, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(M.info2, LV_OBJ_FLAG_HIDDEN);
}

static void build_info(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *k = ee_card(parent, w);
    lv_obj_set_style_pad_all(k, 16, 0);
    lv_obj_set_style_pad_row(k, 6, 0);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(k, sums_cb, LV_EVENT_CLICKED, NULL);
    M.info1 = aos_label(k, "", aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(M.info1, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(M.info1, w - 32);
    M.info2 = aos_label(k, "", aos_font_small, AOS_C_DIM);
    lv_label_set_long_mode(M.info2, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(M.info2, w - 32);
    aos_make_decorative(M.info1);
    aos_make_decorative(M.info2);
    if (S.ref) {
        lv_obj_t *row = ee_box(k, w - 32, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(row, 12, 0);
        ee_pill(row, AOS_SYM_CHEVRON_LEFT, NULL, AOS_C_CARD2, diff_cb, (void *)(intptr_t)-1);
        ee_pill(row, AOS_SYM_CHEVRON_RIGHT, NULL, AOS_C_CARD2, diff_cb, (void *)(intptr_t)1);
        ee_pill(row, AOS_SYM_CLOSE, _("Dejar"), AOS_C_CARD2, diff_cb, (void *)(intptr_t)0);
    }
    info_refresh();
}

/* -------------------------------------------------------------------------- */
/* The tools                                                                   */
/* -------------------------------------------------------------------------- */

static void read_go(void *ud)
{
    (void)ud;
    M.editing = false;
    ee_start_read(false);
}

static void read_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) { aos_ui_toast(_("Hay un trabajo en curso"), 1600); return; }
    if (S.ndirty) {
        char t[160];
        snprintf(t, sizeof t, _("Hay %u bytes cambiados sin escribir: leer el chip los pisa."), (unsigned)S.ndirty);
        ee_confirm(_("Leer"), t, _("Leer igual"), true, read_go, NULL);
        return;
    }
    read_go(NULL);
}

static void write_go(void *ud)
{
    M.editing = false;
    ee_start_write((int)(intptr_t)ud, false);
}

static void where_text(char *out, size_t len)
{
    const ee_chip_t *c = ee_cur();
    if (c->fam == EE_I2C) snprintf(out, len, "%s 0x%02X", S.conf.i2c_port, S.conf.i2c_addr);
    else if (c->fam == EE_MW) snprintf(out, len, "CS%d SK%d DI%d DO%d", S.conf.mw[0], S.conf.mw[1], S.conf.mw[2], S.conf.mw[3]);
    else if (S.conf.spi_cs >= 0) snprintf(out, len, "%s CS%d", S.conf.spi_port, S.conf.spi_cs);
    else snprintf(out, len, "%s", S.conf.spi_port);
}

static void write_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) { aos_ui_toast(_("Hay un trabajo en curso"), 1600); return; }
    const ee_chip_t *c = ee_cur();
    if (!S.valid) { aos_ui_toast(_("Leé el chip o abrí una versión primero"), 2000); return; }
    if (S.size != c->size) { aos_ui_toast(_("La imagen no es del tamaño de este chip"), 2000); return; }
    char where[48], t[300], warn[96] = "";
    where_text(where, sizeof where);
    if (S.img_chip[0] && strcmp(S.img_chip, c->name))
        snprintf(warn, sizeof warn, _("\nOjo: la imagen es de un %s."), S.img_chip);
    if (S.ndirty) {
        snprintf(t, sizeof t, _("Escribir %u bytes cambiados en el %s (%s). Antes se guarda una copia de lo que tiene, y después se verifica.%s"),
                 (unsigned)S.ndirty, c->name, where, warn);
        ee_confirm(_("Escribir los cambios"), t, _("Escribir"), true, write_go, (void *)(intptr_t)WR_DIRTY);
    } else {
        snprintf(t, sizeof t, _("Escribir la imagen entera (%s) en el %s (%s). Sólo se escriben las páginas que cambian; antes se guarda una copia y después se verifica.%s"),
                 S.img_from, c->name, where, warn);
        ee_confirm(_("Escribir la imagen"), t, _("Escribir"), true, write_go, (void *)(intptr_t)WR_FULL);
    }
}

static void goto_done(uint32_t v, void *ud)
{
    (void)ud;
    if (v >= S.size) { aos_ui_toast(_("Fuera de la memoria"), 1600); return; }
    ee_pg_mem_goto(v, 0);
    ed_refresh();
}

static void goto_cb(lv_event_t *e)
{
    (void)e;
    if (!S.valid) return;
    ee_ask_hex(_("Ir a la dirección"), S.cursor, M.adig, goto_done, NULL);
}

static bool find_next(void)
{
    if (!M.patn || !S.valid || (uint32_t)M.patn > S.size) return false;
    uint32_t n = S.size, from = S.mark_len ? S.cursor + 1 : S.cursor;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (from + k) % n;
        if (i + (uint32_t)M.patn <= n && S.img[i] == M.pat[0] && !memcmp(S.img + i, M.pat, (size_t)M.patn)) {
            ee_pg_mem_goto(i, (uint32_t)M.patn);
            ed_refresh();
            return true;
        }
    }
    return false;
}

static void search_run(void)
{
    if (!find_next()) aos_ui_toast(_("No está"), 1600);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void text_search(const char *t, void *ud)
{
    (void)ud;
    size_t n = strlen(t);
    if (!n) return;
    if (n > sizeof M.pat) n = sizeof M.pat;
    memcpy(M.pat, t, n);
    M.patn = (int)n;
    snprintf(M.pat_show, sizeof M.pat_show, "\"%s\"", t);
    S.mark_len = 0;
    search_run();
}

static void hex_search(const char *t, void *ud)
{
    (void)ud;
    int n = 0, hi = -1;
    for (const char *p = t; *p && n < (int)sizeof M.pat; p++) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p++; continue; }
        int v = hexval(*p);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { M.pat[n++] = (uint8_t)(hi << 4 | v); hi = -1; }
    }
    if (!n || hi >= 0) { aos_ui_toast(_("Escribí bytes en hexa, de a dos cifras"), 2000); return; }
    M.patn = n;
    size_t k = 0;
    for (int i = 0; i < n && k + 4 < sizeof M.pat_show; i++) k += (size_t)snprintf(M.pat_show + k, sizeof M.pat_show - k, "%s%02X", i ? " " : "", M.pat[i]);
    S.mark_len = 0;
    search_run();
}

static void search_pick(int i, void *ud)
{
    (void)ud;
    if (M.patn && i == 0) { search_run(); return; }
    if (M.patn) i--;
    if (i == 0) ee_ask_text(_("Buscar texto"), "", 48, text_search, NULL);
    else ee_ask_text(_("Buscar bytes (hexa)"), "", 96, hex_search, NULL);
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    if (!S.valid) return;
    static ee_item_t it[3];
    static char next[80];
    int n = 0;
    if (M.patn) {
        snprintf(next, sizeof next, _("Siguiente: %s"), M.pat_show);
        it[n++] = (ee_item_t){ next, NULL, false, false };
    }
    it[n++] = (ee_item_t){ _("Buscar texto"), NULL, false, false };
    it[n++] = (ee_item_t){ _("Buscar bytes (hexa)"), _("DE AD BE EF"), false, false };
    ee_sheet(_("Buscar"), it, n, search_pick, NULL);
}

static void undo_cb(lv_event_t *e)
{
    (void)e;
    if (!ee_undo()) { aos_ui_toast(_("Nada para deshacer"), 1400); return; }
    moved();
}

/* ---- more ---- */

static void sums_cb(lv_event_t *e)
{
    (void)e;
    if (!S.valid) return;
    ee_sums_t s;
    ee_sums_of(S.img, S.size, &s);
    int32_t cw;
    lv_obj_t *k = ee_sheet_custom(_("Sumas de la imagen"), &cw);
    char t[200];
    snprintf(t, sizeof t, "CRC32  %08x\nMD5    %s", (unsigned)s.crc, s.md5);
    lv_obj_t *l = aos_label(k, t, &aos_mono_22, AOS_C_TEXT);
    lv_obj_set_style_pad_hor(l, 12, 0);
    l = aos_label(k, "SHA-256", &aos_mono_22, AOS_C_TEXT);
    lv_obj_set_style_pad_hor(l, 12, 0);
    l = aos_label(k, s.sha, &aos_mono_22, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, cw - 24);
    lv_obj_set_style_pad_hor(l, 12, 0);
    lv_obj_set_style_pad_bottom(l, 12, 0);
    if (S.ndirty) ee_caption(k, _("Con los cambios sin escribir."), cw - 24);
}

static void save_done(const char *note, void *ud)
{
    (void)ud;
    if (!S.valid) return;
    const char *chip = S.img_chip[0] ? S.img_chip : ee_cur()->name;
    ee_vw_t w;
    char file[64];
    bool same = false;
    if (!ee_vw_open(&w, chip, "vista") || !ee_vw_put(&w, S.img, S.size) ||
        !ee_vw_close(&w, "vista", note, file, sizeof file, &same)) {
        ee_vw_abort(&w);
        aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2000);
        return;
    }
    S.vers_stale = true;
    aos_ui_toast(same ? _("Igual a la última versión: no se guardó otra") : _("Guardada en Versiones"), 2000);
}

static char s_cmp_file[EE_VER_MAX][64];

static void cmp_pick(int i, void *ud)
{
    (void)ud;
    uint32_t size = 0;
    uint8_t *b = ee_ver_load(ee_cur()->name, s_cmp_file[i], &size);
    if (!b) { aos_ui_toast(_("No se pudo leer la versión"), 1600); return; }
    ee_ref_set(b, size, s_cmp_file[i]);
    ee_rebuild();
    if (S.ndiff) next_diff(1);
}

static void compare_sheet(void)
{
    ee_vers_reload();
    if (!S.nvers) { aos_ui_toast(_("No hay versiones de este chip"), 1600); return; }
    static ee_item_t it[EE_VER_MAX];
    static char sub[EE_VER_MAX][48];
    int n = S.nvers;
    for (int i = 0; i < n; i++) {
        snprintf(s_cmp_file[i], sizeof s_cmp_file[i], "%s", S.vers[i].file);
        snprintf(sub[i], sizeof sub[i], "CRC32 %08x · %.24s", (unsigned)S.vers[i].crc, S.vers[i].note);
        it[i] = (ee_item_t){ s_cmp_file[i], sub[i], false, !strcmp(S.ref_name, s_cmp_file[i]) };
    }
    ee_sheet(_("Comparar con"), it, n, cmp_pick, NULL);
}

static void undo_all(void)
{
    while (ee_undo()) {}
    if (S.ndirty) aos_ui_toast(_("Quedan cambios viejos: leé el chip de nuevo"), 2200);
    moved();
}

static void erase_go(void *ud)
{
    (void)ud;
    M.editing = false;
    ee_start_write(WR_ERASE, false);
}

enum { MO_SUMS, MO_SAVE, MO_CMP, MO_NOCMP, MO_UNDOALL, MO_ERASE };
static int s_more[8];

static void more_pick(int i, void *ud)
{
    (void)ud;
    char t[220], where[48];
    switch (s_more[i]) {
    case MO_SUMS:    sums_cb(NULL); break;
    case MO_SAVE:    ee_ask_text(_("Nota de la versión"), "", 90, save_done, NULL); break;
    case MO_CMP:     compare_sheet(); break;
    case MO_NOCMP:   ee_ref_clear(); ee_rebuild(); break;
    case MO_UNDOALL: undo_all(); break;
    case MO_ERASE:
        where_text(where, sizeof where);
        snprintf(t, sizeof t, _("Dejar el %s (%s) todo en 0xFF. Antes se guarda una copia de lo que tiene."), ee_cur()->name, where);
        ee_confirm(_("Borrar el chip"), t, _("Borrar"), true, erase_go, NULL);
        break;
    }
}

static void more_cb(lv_event_t *e)
{
    (void)e;
    static ee_item_t it[8];
    int n = 0;
    if (S.valid) {
        s_more[n] = MO_SUMS;    it[n++] = (ee_item_t){ _("Sumas: CRC32, MD5, SHA-256"), NULL, false, false };
        s_more[n] = MO_SAVE;    it[n++] = (ee_item_t){ _("Guardar como versión"), _("con una nota, sin escribir el chip"), false, false };
        s_more[n] = MO_CMP;     it[n++] = (ee_item_t){ _("Comparar con una versión"), NULL, false, false };
    }
    if (S.ref) { s_more[n] = MO_NOCMP; it[n++] = (ee_item_t){ _("Dejar de comparar"), NULL, false, false }; }
    if (S.ndirty) { s_more[n] = MO_UNDOALL; it[n++] = (ee_item_t){ _("Deshacer todos los cambios"), NULL, false, false }; }
    s_more[n] = MO_ERASE; it[n++] = (ee_item_t){ _("Borrar el chip"), _("todo en 0xFF"), true, false };
    if (J.busy) { aos_ui_toast(_("Hay un trabajo en curso"), 1600); return; }
    ee_sheet(_("Memoria"), it, n, more_pick, NULL);
}

static void tool(lv_obj_t *parent, int32_t w, const char *glyph, const char *text, lv_event_cb_t cb, bool on)
{
    lv_obj_t *b = ee_box(parent, w, 96);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *g = aos_label(b, glyph, &aos_sym_44, on ? C_AMBER : AOS_C_DIM);
    lv_obj_align(g, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *l = aos_label(b, text, aos_font_tiny, on ? AOS_C_TEXT : AOS_C_DIM);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 30);
    aos_make_decorative(g);
    aos_make_decorative(l);
    if (!on) lv_obj_add_state(b, LV_STATE_DISABLED);
}

static void build_tools(lv_obj_t *parent, int32_t w, int per_row)
{
    lv_obj_t *k = ee_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(k, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(k, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(k, 6, 0);
    lv_obj_set_flex_flow(k, LV_FLEX_FLOW_ROW_WRAP);
    int32_t tw = (w - 12) / per_row;
    bool idle = !J.busy;
    tool(k, tw, AOS_SYM_DOWNLOAD, _("Leer"), read_cb, idle);
    tool(k, tw, AOS_SYM_SEND, _("Escribir"), write_cb, idle && S.valid);
    tool(k, tw, AOS_SYM_POUND, _("Ir a"), goto_cb, S.valid);
    tool(k, tw, AOS_SYM_MAGNIFY, _("Buscar"), search_cb, S.valid);
    tool(k, tw, AOS_SYM_RESTART, _("Deshacer"), undo_cb, idle && S.nundo > 0);
    tool(k, tw, AOS_SYM_DOTS_HORIZONTAL, _("Más"), more_cb, idle);
}

/* -------------------------------------------------------------------------- */
/* The hex card                                                                */
/* -------------------------------------------------------------------------- */

static void build_hex(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *k = ee_box(parent, w, h);
    lv_obj_set_style_bg_color(k, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(k, AOS_UI_RADIUS, 0);
    lv_obj_set_style_clip_corner(k, true, 0);
    if (!S.valid) {
        const char *t = J.busy && J.job == JOB_READ ? _("Leyendo…")
                      : _("Todavía no hay nada leído.\nTocá Leer, o abrí una versión en Versiones.");
        M.empty = aos_label(k, t, aos_font_body, AOS_C_DIM);
        lv_obj_set_style_text_align(M.empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(M.empty, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(M.empty, w - 60);
        lv_obj_center(M.empty);
        return;
    }
    int32_t vw = view_w();
    int32_t x0 = (w - SCRUB_W - vw) / 2;
    if (x0 < 0) x0 = 0;
    M.view = lv_obj_create(k);
    lv_obj_remove_style_all(M.view);
    lv_obj_set_size(M.view, vw, h);
    lv_obj_set_pos(M.view, x0, 0);
    lv_obj_set_scroll_dir(M.view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(M.view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(M.view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(M.view, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(M.view, scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(M.view, click_cb, LV_EVENT_CLICKED, NULL);
    M.spacer = ee_box(M.view, 4, (int32_t)M.rows * M.rh + 2 * M.pad);
    lv_obj_remove_flag(M.spacer, LV_OBJ_FLAG_CLICKABLE);

    M.scrub = lv_slider_create(k);
    lv_obj_set_size(M.scrub, 12, h - 48);
    lv_obj_align(M.scrub, LV_ALIGN_RIGHT_MID, -(SCRUB_W - 12) / 2, 0);
    lv_slider_set_range(M.scrub, 0, 1000);
    lv_slider_set_value(M.scrub, 1000, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(M.scrub, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(M.scrub, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(M.scrub, C_AMBER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(M.scrub, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(M.scrub, 20);
    lv_obj_add_event_cb(M.scrub, scrub_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_update_layout(k);
    scroll_row(M.top_row);
    scroll_cb(NULL);
}

void ee_pg_mem(lv_obj_t *parent)
{
    metrics();
    if (!S.valid) M.editing = false;
    if (S.cursor >= img_size()) S.cursor = 0;
    if ((uint32_t)M.top_row >= M.rows) M.top_row = 0;
    int32_t w = U.CW, h = U.CH - 16;
    if (!U.land) {
        lv_obj_t *col = ee_box(parent, w, h);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 12, 0);
        if (!M.editing) build_tools(col, w, 6);
        build_info(col, w);
        lv_obj_update_layout(col);
        int32_t used = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_count(col); i++) used += lv_obj_get_height(lv_obj_get_child(col, (int32_t)i)) + 12;
        int32_t ed_h = M.editing ? 470 : 0;
        build_hex(col, w, h - used - ed_h - (M.editing ? 12 : 0));
        if (M.editing) build_editor(col, w, ed_h);
    } else {
        int32_t sw = 340, hw = w - sw - AOS_UI_PAD;
        build_hex(parent, hw, h);
        lv_obj_t *side = ee_box(parent, sw, h);
        lv_obj_set_x(side, hw + AOS_UI_PAD);
        lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(side, 12, 0);
        if (M.editing) {
            build_editor(side, sw, h);
        } else {
            build_tools(side, sw, 3);
            build_info(side, sw);
        }
    }
    if (M.editing) ensure_visible(S.cursor);
}

void ee_pg_mem_refresh(void)
{
    if (S.tab != TAB_MEM) return;
    ee_rebuild();
}
