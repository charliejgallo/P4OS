/*
 * P4OS - widgets chosen on the board (2026-10-25): the home screen's edit
 * mode had no way to add one, they came only from the portal's #inicio page
 * or menu.txt.
 *
 * Two sheets over everything. "Agregar widget" lists the types the widgets
 * described with aos_ui_describe_widget() (and the clock, which is the
 * shell's own); the one tapped goes to the home screen, which places it at
 * the top of the page on show and opens the other sheet on it. That one,
 * also a tap on a widget in edit mode, has its size, what it shows when the
 * type has choices (the sensor service's values, Home Assistant's entities:
 * none picked is the widget's own choice), and a way to take it off.
 *
 * Its objects live while it is open, on LVGL's heap (PSRAM); the choices'
 * array is a malloc of over a kilobyte, PSRAM on the board too.
 */
#include "aos_internal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_menu.h"
#include "aos_sys_glyphs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DESC_MAX    8
#define CHOICES_MAX 96
#define PICK_MAX    8

typedef struct {
    char                 type[AOS_MENU_WIDGET_TYPE_MAX];
    const char          *name, *about, *glyph;
    int                  max_pick;
    aos_widget_choices_t choices;
} desc_t;

static desc_t s_desc[DESC_MAX];
static int    s_ndesc;
static const desc_t CLOCK = { "clock", N_("Reloj"), N_("La hora y la fecha, grandes."), AOS_SYM_CLOCK_OUTLINE, 0, NULL };

static const struct { int w, h; const char *name; } SIZES[] = {
    { 2, 2, N_("Chico") }, { 4, 2, N_("Mediano") }, { 4, 4, N_("Grande") },
};
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

void aos_ui_describe_widget(const char *type, const char *name, const char *about, const char *glyph,
                            int max_pick, aos_widget_choices_t choices)
{
    int i = 0;
    while (i < s_ndesc && strcmp(s_desc[i].type, type)) i++;
    if (i == DESC_MAX) return;
    if (i == s_ndesc) s_ndesc++;
    desc_t *d = &s_desc[i];
    snprintf(d->type, sizeof d->type, "%s", type);
    d->name = name;
    d->about = about;
    d->glyph = glyph;
    d->max_pick = max_pick > PICK_MAX ? PICK_MAX : max_pick;
    d->choices = choices;
}

static const desc_t *find(const char *type)
{
    if (!strcmp(type, "clock")) return &CLOCK;
    for (int i = 0; i < s_ndesc; i++)
        if (!strcmp(s_desc[i].type, type)) return &s_desc[i];
    return NULL;
}

const char *aos_wedit_glyph(const char *type)
{
    const desc_t *d = find(type);
    return d ? d->glyph : AOS_SYM_VIEW_GRID_OUTLINE;
}

/* -------------------------------------------------------------------------- */
/* The sheet                                                                   */
/* -------------------------------------------------------------------------- */

static struct {
    lv_obj_t           *sheet;          /* the dimmed layer; NULL while closed */
    void              (*picked)(const char *type);
    aos_wedit_done_t    done;
    const desc_t       *d;
    char                type[AOS_MENU_WIDGET_TYPE_MAX];
    int                 w, h;
    char                pick[PICK_MAX][64];
    int                 npick;
    aos_widget_choice_t *ch;
    int                 nch;
    lv_obj_t           *size_btn[NSIZES];
    lv_obj_t           *auto_mark;
    lv_obj_t          **mark;           /* a number per choice row */
    lv_obj_t           *list, *nothing;
    lv_timer_t         *poll;           /* nothing to choose yet: asks again */
} E;

static void sheet_free(void)
{
    if (E.poll) lv_timer_delete(E.poll);
    E.poll = NULL;
    if (E.sheet) lv_obj_delete_async(E.sheet);
    E.sheet = NULL;
    free(E.ch);
    free(E.mark);
    E.ch = NULL;
    E.mark = NULL;
    E.nch = 0;
}

/* closing applies: the done button, a tap beside the panel, back */
static void finish(void)
{
    if (!E.sheet) return;
    aos_wedit_done_t done = E.done;
    char arg[AOS_MENU_WIDGET_ARG_MAX] = "";
    for (int i = 0; i < E.npick; i++) {
        size_t l = strlen(arg);
        snprintf(arg + l, sizeof arg - l, "%s%s", i ? "," : "", E.pick[i]);
    }
    int w = E.w, h = E.h;
    E.done = NULL;
    E.picked = NULL;
    sheet_free();
    if (done) done(AOS_WEDIT_APPLY, w, h, arg);
}

bool aos_wedit_open(void) { return E.sheet != NULL; }

bool aos_wedit_close(void)
{
    if (!E.sheet) return false;
    finish();
    return true;
}

static void dim_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == E.sheet) finish();
}

static void done_clicked(lv_event_t *e) { (void)e; finish(); }

static lv_obj_t *sheet_open(const char *glyph, const char *title, bool tall)
{
    const aos_geo_t *g = aos_ui_geo();
    E.sheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(E.sheet);
    lv_obj_set_size(E.sheet, g->w, g->h);
    lv_obj_set_style_bg_color(E.sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(E.sheet, LV_OPA_60, 0);
    lv_obj_add_flag(E.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(E.sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(E.sheet, dim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *p = lv_obj_create(E.sheet);
    lv_obj_remove_style_all(p);
    int32_t pw = g->landscape ? g->w * 3 / 5 : g->w;
    lv_obj_set_width(p, pw);
    if (tall) lv_obj_set_height(p, g->h * 82 / 100);
    else lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p, g->h * 88 / 100, 0);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 40, 0);
    lv_obj_set_style_pad_hor(p, 28, 0);
    lv_obj_set_style_pad_top(p, 24, 0);
    lv_obj_set_style_pad_bottom(p, 76, 0);     /* the lower corners hang off the screen */
    lv_obj_set_style_pad_row(p, 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);  /* a tap on it is not a tap beside it */
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, 48);

    lv_obj_t *head = lv_obj_create(p);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 16, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    aos_label(head, glyph, &aos_sym_44, AOS_C_ACCENT);
    lv_obj_t *t = aos_label(head, title, aos_font_title, lv_color_white());
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *done = lv_button_create(head);
    lv_obj_set_style_bg_opa(done, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(done, 0, 0);
    lv_obj_set_style_pad_all(done, 12, 0);
    aos_label(done, _("Listo"), aos_font_body, AOS_C_ACCENT);
    lv_obj_add_event_cb(done, done_clicked, LV_EVENT_CLICKED, NULL);
    aos_make_decorative(head);
    lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static lv_obj_t *caption(lv_obj_t *p, const char *text)
{
    lv_obj_t *l = aos_label(p, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_top(l, 8, 0);
    return l;
}

/* A rounded row: glyph (or none), text and a smaller one under it, and
 * whatever the caller adds at the end. */
static lv_obj_t *row_create(lv_obj_t *parent, const char *glyph, const char *text, const char *sub)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, AOS_UI_ROW_H, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 22, 0);
    lv_obj_set_style_pad_hor(r, 22, 0);
    lv_obj_set_style_pad_ver(r, 14, 0);
    lv_obj_set_style_pad_column(r, 18, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (glyph) aos_make_decorative(aos_label(r, glyph, &aos_sym_44, lv_color_white()));
    lv_obj_t *col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t *l = aos_label(col, text, aos_font_body, lv_color_white());
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    if (sub && sub[0]) {
        lv_obj_t *s = aos_label(col, sub, aos_font_small, AOS_C_DIM);
        lv_obj_set_width(s, LV_PCT(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    }
    aos_make_decorative(col);
    return r;
}

/* -------------------------------------------------------------------------- */
/* Agregar widget                                                              */
/* -------------------------------------------------------------------------- */

static void type_clicked(lv_event_t *e)
{
    const desc_t *d = lv_event_get_user_data(e);
    void (*picked)(const char *type) = E.picked;
    char type[AOS_MENU_WIDGET_TYPE_MAX];
    snprintf(type, sizeof type, "%s", d->type);
    E.picked = NULL;
    E.done = NULL;
    sheet_free();
    if (picked) picked(type);
}

void aos_wedit_add(void (*picked)(const char *type))
{
    if (E.sheet) return;
    memset(&E, 0, sizeof E);
    E.picked = picked;
    lv_obj_t *p = sheet_open(AOS_SYM_PLUS, _("Agregar widget"), false);
    lv_obj_t *list = lv_obj_create(p);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int i = -1; i < s_ndesc; i++) {
        const desc_t *d = i < 0 ? &CLOCK : &s_desc[i];
        lv_obj_t *r = row_create(list, d->glyph, _(d->name), d->about ? _(d->about) : NULL);
        lv_obj_add_event_cb(r, type_clicked, LV_EVENT_CLICKED, (void *)d);
    }
    caption(p, _("Después de agregarlo, tocalo para elegir el tamaño y qué muestra; mantenelo apretado un momento y arrastralo para moverlo."));
}

/* -------------------------------------------------------------------------- */
/* One widget                                                                  */
/* -------------------------------------------------------------------------- */

static void sizes_refresh(void)
{
    for (int i = 0; i < NSIZES; i++) {
        bool on = SIZES[i].w == E.w && SIZES[i].h == E.h;
        lv_obj_set_style_bg_color(E.size_btn[i], on ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    }
}

static void size_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    E.w = SIZES[i].w;
    E.h = SIZES[i].h;
    sizes_refresh();
}

static int pick_index(const char *value)
{
    for (int i = 0; i < E.npick; i++)
        if (!strcmp(E.pick[i], value)) return i;
    return -1;
}

/* the numbers on the rows: the order the widget shows them in */
static void marks_refresh(void)
{
    lv_obj_set_flag(E.auto_mark, LV_OBJ_FLAG_HIDDEN, E.npick > 0);
    for (int i = 0; i < E.nch; i++) {
        int k = pick_index(E.ch[i].value);
        lv_obj_set_flag(E.mark[i], LV_OBJ_FLAG_HIDDEN, k < 0);
        if (k >= 0) lv_label_set_text_fmt(lv_obj_get_child(E.mark[i], 0), "%d", k + 1);
    }
}

static void auto_clicked(lv_event_t *e)
{
    (void)e;
    E.npick = 0;
    marks_refresh();
}

static void choice_clicked(lv_event_t *e)
{
    const char *v = E.ch[(int)(intptr_t)lv_event_get_user_data(e)].value;
    int k = pick_index(v);
    if (k >= 0) {
        memmove(E.pick[k], E.pick[k + 1], (size_t)(E.npick - k - 1) * sizeof E.pick[0]);
        E.npick--;
    } else {
        size_t bytes = 0;
        for (int i = 0; i < E.npick; i++) bytes += strlen(E.pick[i]) + 1;
        if (E.npick >= E.d->max_pick || bytes + strlen(v) >= AOS_MENU_WIDGET_ARG_MAX) {
            char t[64];
            snprintf(t, sizeof t, _("Hasta %d"), E.npick >= E.d->max_pick ? E.d->max_pick : E.npick);
            aos_ui_toast(t, 1500);
            return;
        }
        snprintf(E.pick[E.npick++], sizeof E.pick[0], "%s", v);
    }
    marks_refresh();
}

static lv_obj_t *mark_create(lv_obj_t *row)
{
    lv_obj_t *m = lv_obj_create(row);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, 48, 48);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(m, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_t *l = aos_label(m, "", aos_font_small, lv_color_white());
    lv_obj_center(l);
    aos_make_decorative(m);
    return m;
}

/* a row per choice, under "Automático" */
static void list_fill(void)
{
    E.mark = E.nch ? calloc((size_t)E.nch, sizeof *E.mark) : NULL;
    if (!E.mark) E.nch = 0;
    for (int i = 0; i < E.nch; i++) {
        lv_obj_t *r = row_create(E.list, NULL, E.ch[i].label, E.ch[i].detail);
        lv_obj_add_event_cb(r, choice_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        E.mark[i] = mark_create(r);
    }
    marks_refresh();
}

static void poll_cb(lv_timer_t *t)
{
    if (!E.sheet || !E.ch) return;
    E.nch = E.d->choices(E.ch, CHOICES_MAX);
    if (!E.nch) return;
    lv_timer_delete(t);
    E.poll = NULL;
    lv_obj_delete(E.nothing);
    E.nothing = NULL;
    list_fill();
}

static void remove_clicked(lv_event_t *e)
{
    (void)e;
    aos_wedit_done_t done = E.done;
    E.done = NULL;
    sheet_free();
    if (done) done(AOS_WEDIT_REMOVE, 0, 0, "");
}

void aos_wedit_config(const char *type, int w, int h, const char *arg, aos_wedit_done_t done)
{
    if (E.sheet) return;
    memset(&E, 0, sizeof E);
    static const desc_t unknown = { "", NULL, NULL, AOS_SYM_VIEW_GRID_OUTLINE, 0, NULL };
    E.d = find(type);
    if (!E.d) E.d = &unknown;
    E.done = done;
    snprintf(E.type, sizeof E.type, "%s", type);
    E.w = w;
    E.h = h;
    /* what the arg already has, in its order */
    if (E.d->choices && arg && arg[0]) {
        char buf[AOS_MENU_WIDGET_ARG_MAX];
        snprintf(buf, sizeof buf, "%s", arg);
        for (char *tok = strtok(buf, ","); tok && E.npick < PICK_MAX; tok = strtok(NULL, ",")) {
            while (*tok == ' ') tok++;
            if (*tok) snprintf(E.pick[E.npick++], sizeof E.pick[0], "%s", tok);
        }
    }
    if (E.d->choices) {
        E.ch = malloc(sizeof *E.ch * CHOICES_MAX);
        E.nch = E.ch ? E.d->choices(E.ch, CHOICES_MAX) : 0;
        /* one picked that is not there now (a sensor unplugged, Home
         * Assistant not connected yet) still gets its row, to take it off */
        for (int i = 0; i < E.npick && E.ch; i++) {
            bool have = false;
            for (int k = 0; k < E.nch && !have; k++) have = !strcmp(E.ch[k].value, E.pick[i]);
            if (have) continue;
            if (E.nch == CHOICES_MAX) E.nch--;
            memmove(&E.ch[1], &E.ch[0], sizeof *E.ch * (size_t)E.nch);
            memset(&E.ch[0], 0, sizeof *E.ch);
            snprintf(E.ch[0].value, sizeof E.ch[0].value, "%s", E.pick[i]);
            snprintf(E.ch[0].label, sizeof E.ch[0].label, "%s", E.pick[i]);
            snprintf(E.ch[0].detail, sizeof E.ch[0].detail, "%s", _("no está ahora"));
            E.nch++;
        }
    }

    lv_obj_t *p = sheet_open(E.d->glyph, E.d->name ? _(E.d->name) : type, E.d->choices != NULL);

    caption(p, _("Tamaño"));
    lv_obj_t *sz = lv_obj_create(p);
    lv_obj_remove_style_all(sz);
    lv_obj_set_size(sz, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sz, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sz, 12, 0);
    lv_obj_remove_flag(sz, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < NSIZES; i++) {
        lv_obj_t *b = lv_obj_create(sz);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 22, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_ver(b, 14, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        aos_label(b, _(SIZES[i].name), aos_font_body, lv_color_white());
        char dims[16];
        snprintf(dims, sizeof dims, "%d × %d", SIZES[i].w, SIZES[i].h);
        aos_label(b, dims, aos_font_small, lv_color_hex(0xDDE6F0));
        aos_make_decorative(b);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, size_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        E.size_btn[i] = b;
    }
    sizes_refresh();

    if (E.d->choices) {
        char t[160];
        snprintf(t, sizeof t, _("Qué muestra: hasta %d, en el orden en que los tocás. Los que no entran en el tamaño quedan afuera."),
                 E.d->max_pick);
        caption(p, t);
        lv_obj_t *list = lv_obj_create(p);
        lv_obj_remove_style_all(list);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 10, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_t *r = row_create(list, NULL, _("Automático"), _("El widget elige lo que conviene mostrar"));
        lv_obj_add_event_cb(r, auto_clicked, LV_EVENT_CLICKED, NULL);
        E.auto_mark = aos_label(r, AOS_SYM_CHECK, &aos_sym_44, AOS_C_ACCENT);
        aos_make_decorative(E.auto_mark);
        E.list = list;
        list_fill();
        if (!E.nch) {
            /* a sensor's first reading, Home Assistant's house: they can
             * come a few seconds after the widget */
            E.nothing = caption(list, _("Todavía no hay nada para elegir: mientras tanto, el widget elige solo."));
            E.poll = lv_timer_create(poll_cb, 1000, NULL);
        }
    } else if (E.d->about) {
        caption(p, _(E.d->about));
    }

    lv_obj_t *rm = row_create(p, NULL, _("Quitar widget"), NULL);
    lv_obj_t *rl = lv_obj_get_child(lv_obj_get_child(rm, 0), 0);
    lv_obj_set_style_text_color(rl, AOS_C_RED, 0);
    lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(rm, remove_clicked, LV_EVENT_CLICKED, NULL);
}
