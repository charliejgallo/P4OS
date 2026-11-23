/*
 * Tortuga - the blocks (DESIGN.md, 6.2). A list, not a free canvas: the
 * program is a column of blocks, the ones inside a "repeat" or an "if"
 * moved right. Touching a block of the palette puts it after the selected
 * one; touching a number, a question or a name changes it. Every block is
 * one line of Logo, so line N of a run is block N.
 */
#include "tg_blocks.h"

#include "aos_fonts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { B_CMD, B_REPEAT, B_IF, B_ELSE, B_WHILE, B_END, B_TO, B_CALL, B_SET, B_ADD };

#define MAXB 96
#define NVAR 3
#define NPROC 3

static const char *const VAR[NVAR] = { "cuenta", "lado", "n" };
static const char *const VAR_EN[NVAR] = { "count", "side", "n" };
static const char *const PROC[NPROC] = { "truco_a", "truco_b", "truco_c" };

typedef struct {
    uint8_t kind, op;       /* op: the order, or the question */
    bool neg;               /* "no" before the question */
    bool has;               /* the value is set */
    int8_t var;             /* -1: the value is the number; else VAR[var] */
    float num;
    uint8_t proc;
    bool param;             /* B_TO: takes n */
    uint8_t who;            /* B_SET, B_ADD: which variable */
} blk_t;

static struct {
    lv_obj_t *box, *list, *pal, *tools, *pop;
    const tg_level_t *lv;
    int world;
    bool paper;
    blk_t b[MAXB];
    int n;
    int sel;                /* the block selected; inserting goes after it */
    int mark;               /* the block marked by the run */
    bool mark_err;
    int pad_pal;            /* the palette's item a gamepad points at */
    int npal;
    int edit;               /* the block whose value the pop-up edits */
} K;

static bool en(void)
{
    return tg->tlang != TG_ES;
}

static bool allowed(int o)
{
    return !K.lv->orders || (K.lv->orders & (1u << o));
}

/* ------------------------------------------------------------ the text */

static int fmt_value(const blk_t *b, char *out, size_t n, bool display)
{
    if (b->var >= 0) return snprintf(out, n, "%s", display && en() ? VAR_EN[b->var] : VAR[b->var]);
    if (b->num == floorf(b->num)) return snprintf(out, n, "%d", (int)b->num);
    return snprintf(out, n, "%g", (double)b->num);
}

static int depth_of(int i)
{
    int d = 0;
    for (int k = 0; k < i; k++) {
        int kd = K.b[k].kind;
        if (kd == B_REPEAT || kd == B_IF || kd == B_WHILE || kd == B_TO) d++;
        if (kd == B_END) d--;
    }
    if (K.b[i].kind == B_END || K.b[i].kind == B_ELSE) d--;
    return d < 0 ? 0 : d;
}

/* the block that closes the one at i (a header), or -1 */
static int closer(int i)
{
    int d = 0;
    for (int k = i; k < K.n; k++) {
        int kd = K.b[k].kind;
        if (kd == B_REPEAT || kd == B_IF || kd == B_WHILE || kd == B_TO) d++;
        if (kd == B_END && --d == 0) return k;
    }
    return -1;
}

static int opener(int end)
{
    int d = 0;
    for (int k = end; k >= 0; k--) {
        int kd = K.b[k].kind;
        if (kd == B_END) d++;
        if ((kd == B_REPEAT || kd == B_IF || kd == B_WHILE || kd == B_TO) && --d == 0) return k;
    }
    return -1;
}

/* one block as Logo (display: in the player's language, else Spanish) */
static void line_of(int i, char *out, size_t n, bool display)
{
    const blk_t *b = &K.b[i];
    bool E = display && en();
    char v[24];
    fmt_value(b, v, sizeof v, display);
    const char *q = tg_orders[b->op].logo_es;
    if (E) q = tg_orders[b->op].logo_en;
    switch (b->kind) {
    case B_CMD: {
        const char *name = E ? tg_orders[b->op].logo_en : tg_orders[b->op].logo_es;
        if (b->has) snprintf(out, n, "%s %s", name, v);
        else snprintf(out, n, "%s", name);
        break;
    }
    case B_REPEAT: snprintf(out, n, "%s %s [", E ? "repeat" : "repetir", v); break;
    case B_IF: snprintf(out, n, "%s %s%s [", E ? "if" : "si", b->neg ? (E ? "not " : "no ") : "", q); break;
    case B_WHILE:
        snprintf(out, n, "%s %s%s [", E ? "while" : "mientras", b->neg ? (E ? "not " : "no ") : "", q);
        break;
    case B_ELSE: snprintf(out, n, "] %s [", E ? "else" : "sino"); break;
    case B_END: {
        int o = opener(i);
        snprintf(out, n, "%s", o >= 0 && K.b[o].kind == B_TO ? (E ? "end" : "fin") : "]");
        break;
    }
    case B_TO: snprintf(out, n, "%s %s%s", E ? "to" : "para", PROC[b->proc], b->param ? " n" : ""); break;
    case B_CALL:
        if (b->has) snprintf(out, n, "%s %s", PROC[b->proc], v);
        else snprintf(out, n, "%s", PROC[b->proc]);
        break;
    case B_SET: snprintf(out, n, "%s = %s", E ? VAR_EN[b->who] : VAR[b->who], v); break;
    case B_ADD: snprintf(out, n, "%s += %s", E ? VAR_EN[b->who] : VAR[b->who], v); break;
    }
}

char *tg_blocks_logo(void)
{
    size_t cap = (size_t)K.n * 48 + 8;
    char *o = tg_malloc(cap);
    if (!o) return NULL;
    size_t k = 0;
    for (int i = 0; i < K.n; i++) {
        char l[64];
        line_of(i, l, sizeof l, false);
        k += (size_t)snprintf(o + k, cap - k, "%s\n", l);
    }
    if (k && o[k - 1] == '\n') o[k - 1] = 0;
    return o;
}

/* ------------------------------------------------- reading a program back */

static bool add(blk_t b)
{
    if (K.n >= MAXB) return false;
    K.b[K.n++] = b;
    return true;
}

static int var_index(const tg_prog_t *p, int name)
{
    for (int i = 0; i < NVAR; i++) {
        if (!strcmp(p->name[name], VAR[i])) return i;
    }
    return -1;
}

static bool value_of(const tg_prog_t *p, const tg_node_t *e, blk_t *b)
{
    if (!e) return true;
    b->has = true;
    if (e->kind == TG_E_NUM) {
        b->num = e->num;
        b->var = -1;
        return true;
    }
    if (e->kind == TG_E_VAR) {
        b->var = (int8_t)var_index(p, e->name);
        return b->var >= 0;
    }
    return false;
}

static bool sensor_of(const tg_node_t *e, blk_t *b)
{
    if (e && e->kind == TG_E_NOT) {
        b->neg = true;
        e = e->a;
    }
    if (!e || e->kind != TG_E_SENSE) return false;
    b->op = e->op;
    return true;
}

static int proc_index(const tg_prog_t *p, int name)
{
    for (int i = 0; i < NPROC; i++) {
        if (!strcmp(p->name[name], PROC[i])) return i;
    }
    return -1;
}

static bool walk(const tg_prog_t *p, const tg_node_t *s)
{
    for (; s; s = s->next) {
        blk_t b;
        memset(&b, 0, sizeof b);
        b.var = -1;
        switch (s->kind) {
        case TG_N_CALL:
            b.kind = B_CMD;
            b.op = s->op;
            if (!value_of(p, s->a, &b) || !add(b)) return false;
            break;
        case TG_N_REPEAT:
            b.kind = B_REPEAT;
            if (!value_of(p, s->a, &b) || !add(b) || !walk(p, s->b)) return false;
            memset(&b, 0, sizeof b);
            b.kind = B_END;
            if (!add(b)) return false;
            break;
        case TG_N_IF:
        case TG_N_WHILE:
            b.kind = s->kind == TG_N_IF ? B_IF : B_WHILE;
            if (!sensor_of(s->a, &b) || !add(b) || !walk(p, s->b)) return false;
            if (s->c) {
                blk_t el = { 0 };
                el.kind = B_ELSE;
                el.var = -1;
                if (!add(el) || !walk(p, s->c)) return false;
            }
            memset(&b, 0, sizeof b);
            b.kind = B_END;
            if (!add(b)) return false;
            break;
        case TG_N_TO: {
            int pi = proc_index(p, s->name);
            if (pi < 0) return false;
            b.kind = B_TO;
            b.proc = (uint8_t)pi;
            b.param = s->a != NULL;
            if (!add(b) || !walk(p, s->b)) return false;
            memset(&b, 0, sizeof b);
            b.kind = B_END;
            if (!add(b)) return false;
            break;
        }
        case TG_N_PCALL: {
            int pi = proc_index(p, s->name);
            if (pi < 0) return false;
            b.kind = B_CALL;
            b.proc = (uint8_t)pi;
            if (!value_of(p, s->a, &b) || !add(b)) return false;
            break;
        }
        case TG_N_SET: {
            int vi = var_index(p, s->name);
            if (vi < 0) return false;
            b.who = (uint8_t)vi;
            const tg_node_t *e = s->a;
            if (e && e->kind == TG_E_BIN && e->op == TG_OP_ADD && e->a->kind == TG_E_VAR && e->a->name == s->name) {
                b.kind = B_ADD;
                e = e->b;
            } else {
                b.kind = B_SET;
            }
            if (!value_of(p, e, &b) || !add(b)) return false;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

static void read_logo(const char *text)
{
    K.n = 0;
    if (!text || !text[0]) return;
    tg_prog_t p;
    if (!tg_prog_init(&p, 2048)) return;
    if (!tg_logo_parse(&p, text) || !walk(&p, p.prog)) K.n = 0;
    tg_prog_free(&p);
}

/* ------------------------------------------------------------ drawing */

static uint32_t color_of(const blk_t *b)
{
    switch (b->kind) {
    case B_CMD: return b->op >= TG_O_PENDOWN ? 0xff8fb8 : b->op == TG_O_SAY ? 0xffd60a : 0x4cd964;
    case B_REPEAT: return 0xff9f0a;
    case B_IF: case B_ELSE: case B_WHILE: return 0xc48cff;
    case B_TO: case B_CALL: return 0x64b5ff;
    case B_SET: case B_ADD: return 0xffd60a;
    default: return 0x8e8e93;
    }
}

static const char *sym_of(const blk_t *b)
{
    switch (b->kind) {
    case B_CMD:
        if (b->op == TG_O_FORWARD) return AOS_SYM_ARROW_UP_BOLD;
        if (b->op == TG_O_BACK) return AOS_SYM_ARROW_DOWN_BOLD;
        if (b->op == TG_O_RIGHT) return AOS_SYM_ARROW_RIGHT_BOLD;
        if (b->op == TG_O_LEFT) return AOS_SYM_ARROW_LEFT_BOLD;
        if (b->op == TG_O_PENDOWN || b->op == TG_O_PENUP) return AOS_SYM_PENCIL;
        return AOS_SYM_CHECK;
    case B_REPEAT: return AOS_SYM_RESTART;
    case B_IF: case B_WHILE: case B_ELSE: return AOS_SYM_PUZZLE;
    case B_TO: case B_CALL: return AOS_SYM_STAR_OUTLINE;
    default: return AOS_SYM_PLUS;
    }
}

static void render(void);
static void popup_value(int i);
static void popup_sensor(int i);

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    tg_audio_play(SND_TAP);
    if (K.sel == i) {
        /* a second touch on a block changes what it holds */
        blk_t *b = &K.b[i];
        if (b->kind == B_IF || b->kind == B_WHILE) popup_sensor(i);
        else if (b->kind == B_CMD && (b->op == TG_O_RIGHT || b->op == TG_O_LEFT) && !K.paper) {
            b->op = b->op == TG_O_RIGHT ? TG_O_LEFT : TG_O_RIGHT;
            if (!allowed(b->op)) b->op = b->op == TG_O_RIGHT ? TG_O_LEFT : TG_O_RIGHT;
        } else if (b->kind == B_TO) {
            if (K.world >= 7) b->param = !b->param;
            else b->proc = (uint8_t)((b->proc + 1) % NPROC);
        } else if (b->kind == B_SET || b->kind == B_ADD) {
            b->who = (uint8_t)((b->who + 1) % NVAR);
        } else if (b->kind == B_CALL) {
            b->proc = (uint8_t)((b->proc + 1) % NPROC);
        } else if ((b->kind == B_CMD && tg_orders[b->op].amax > 0) || b->kind == B_REPEAT) {
            popup_value(i);
            return;
        }
    }
    K.sel = i;
    render();
}

static void value_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    K.sel = i;
    popup_value(i);
}

static lv_obj_t *row(int i, int w)
{
    const blk_t *b = &K.b[i];
    int d = depth_of(i);
    lv_obj_t *r = lv_obj_create(K.list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w - d * 34, 54);
    lv_obj_set_style_margin_left(r, d * 34, 0);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    uint32_t c = color_of(b);
    if (b->kind == B_END) {
        lv_obj_set_height(r, 20);
        lv_obj_set_width(r, 120);
        c = color_of(&K.b[opener(i) >= 0 ? opener(i) : i]);
    }
    lv_obj_set_style_bg_color(r, lv_color_hex(c), 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(r, 12, 0);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (i == K.mark) {
        lv_obj_set_style_border_color(r, K.mark_err ? lv_color_hex(0xff453a) : lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(r, 4, 0);
    } else if (i == K.sel) {
        lv_obj_set_style_outline_color(r, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_outline_width(r, 2, 0);
        lv_obj_set_style_outline_pad(r, 2, 0);
    }
    if (b->kind == B_END) return r;
    lv_color_t ink = lv_color_hex(0x10140c);
    lv_obj_t *s = tg_label(r, sym_of(b), &aos_sym_28, ink);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    char t[64];
    line_of(i, t, sizeof t, true);
    /* the value goes in a chip of its own, to be touched */
    bool chip = (b->kind == B_CMD && b->has) || b->kind == B_REPEAT || (b->kind == B_CALL && b->has) ||
                b->kind == B_SET || b->kind == B_ADD;
    char v[24];
    fmt_value(b, v, sizeof v, true);
    if (chip) {
        /* the text without the value at its end (or before the "[") */
        char *at = strstr(t, v);
        if (b->kind == B_REPEAT && at) *at = 0;
        else if (at) *at = 0;
    }
    if (b->kind == B_REPEAT) {
        char *br = strchr(t, '[');
        if (br) *br = 0;
    }
    if (b->kind == B_IF || b->kind == B_WHILE || b->kind == B_ELSE) {
        size_t L = strlen(t);
        if (L && t[L - 1] == '[') t[L - 1] = 0;
        if (t[0] == ']') memmove(t, t + 2, strlen(t + 2) + 1);
    }
    lv_obj_t *l = tg_label(r, t, aos_font_small, ink);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (chip) {
        lv_obj_t *c2 = lv_button_create(r);
        lv_obj_set_height(c2, 40);
        lv_obj_set_style_bg_color(c2, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(c2, LV_OPA_80, 0);
        lv_obj_set_style_radius(c2, 20, 0);
        lv_obj_set_style_shadow_width(c2, 0, 0);
        lv_obj_set_style_pad_hor(c2, 14, 0);
        lv_obj_set_style_pad_ver(c2, 0, 0);
        lv_obj_t *vl = tg_label(c2, v, aos_font_small, ink);
        lv_obj_center(vl);
        lv_obj_add_event_cb(c2, value_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    return r;
}

static void render(void)
{
    if (!K.list) return;
    lv_obj_clean(K.list);
    int w = lv_obj_get_content_width(K.list);
    if (w < 100) w = 300;
    if (!K.n) {
        lv_obj_t *l = tg_label(K.list, _("Tocá las piezas de abajo para armar el programa."), aos_font_small,
                               AOS_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, w);
    }
    lv_obj_t *selobj = NULL;
    for (int i = 0; i < K.n; i++) {
        lv_obj_t *r = row(i, w);
        if (i == K.sel || i == K.mark) selobj = r;
    }
    if (selobj) lv_obj_scroll_to_view(selobj, LV_ANIM_OFF);
}

void tg_blocks_mark(int line, bool error)
{
    K.mark = line - 1;
    K.mark_err = error;
    render();
}

/* ------------------------------------------------------------ editing */

static void insert_at(int pos, blk_t b)
{
    if (K.n >= MAXB) return;
    memmove(&K.b[pos + 1], &K.b[pos], sizeof(blk_t) * (size_t)(K.n - pos));
    K.b[pos] = b;
    K.n++;
}

static void put(blk_t b, bool container)
{
    int pos = K.sel + 1;
    if (pos < 0) pos = 0;
    if (pos > K.n) pos = K.n;
    insert_at(pos, b);
    K.sel = pos;
    if (container) {
        blk_t end = { 0 };
        end.kind = B_END;
        end.var = -1;
        insert_at(pos + 1, end);
        /* the next block goes inside */
    }
    K.mark = -1;
    render();
}

static void del_cb(lv_event_t *e)
{
    (void)e;
    if (K.sel < 0 || K.sel >= K.n) return;
    int i = K.sel;
    int kd = K.b[i].kind;
    int a = i, z = i;
    if (kd == B_END) {
        a = opener(i);
        if (a < 0) a = i;
        z = i;
    } else if (kd == B_REPEAT || kd == B_IF || kd == B_WHILE || kd == B_TO) {
        z = closer(i);
        if (z < 0) z = i;
    }
    int cnt = z - a + 1;
    memmove(&K.b[a], &K.b[z + 1], sizeof(blk_t) * (size_t)(K.n - z - 1));
    K.n -= cnt;
    K.sel = a - 1;
    if (K.sel >= K.n) K.sel = K.n - 1;
    K.mark = -1;
    tg_audio_play(SND_TAP);
    render();
}

static void move(int dir)
{
    int i = K.sel;
    if (i < 0 || i >= K.n) return;
    int kd = K.b[i].kind;
    if (kd != B_CMD && kd != B_CALL && kd != B_SET && kd != B_ADD) return;
    int j = i + dir;
    if (j < 0 || j >= K.n) return;
    blk_t t = K.b[i];
    K.b[i] = K.b[j];
    K.b[j] = t;
    K.sel = j;
    K.mark = -1;
    render();
}

static void up_cb(lv_event_t *e)
{
    (void)e;
    move(-1);
}

static void down_cb(lv_event_t *e)
{
    (void)e;
    move(1);
}

static void top_cb(lv_event_t *e)
{
    (void)e;
    K.sel = -1;     /* the next block goes first */
    render();
}

/* ------------------------------------------------------------ pop-ups */

static void pop_close(void)
{
    if (K.pop) lv_obj_delete(K.pop);
    K.pop = NULL;
}

static void pop_close_cb(lv_event_t *e)
{
    (void)e;
    pop_close();
    render();
}

static lv_obj_t *pop_new(const char *title)
{
    pop_close();
    lv_obj_t *p = lv_obj_create(tg->scr);
    int w = tg->W - 80 > 640 ? 640 : tg->W - 80;
    lv_obj_set_size(p, w, LV_SIZE_CONTENT);
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x1b2027), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0xff9f0a), 0);
    lv_obj_set_style_border_width(p, 3, 0);
    lv_obj_set_style_radius(p, 22, 0);
    lv_obj_set_style_pad_all(p, 18, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(p, 10, 0);
    lv_obj_set_style_pad_column(p, 10, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = tg_label(p, title, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(t, LV_PCT(100));
    K.pop = p;
    return p;
}

static void num_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    blk_t *b = &K.b[K.edit];
    if (v <= -1000) {
        b->var = (int8_t)(-(v + 1000));
        b->has = true;
    } else if (v == -999) {
        b->has = false;
    } else {
        b->num = (float)v;
        b->var = -1;
        b->has = true;
    }
    pop_close();
    render();
}

static void step_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    blk_t *b = &K.b[K.edit];
    if (b->var >= 0) b->var = -1;
    b->num += (float)d;
    if (b->num < 0) b->num = 0;
    b->has = true;
    popup_value(K.edit);
}

static void pop_btn(lv_obj_t *p, const char *t, lv_event_cb_t cb, int v, uint32_t color)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_height(b, 56);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_t *l = tg_label(b, t, aos_font_body, AOS_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
}

static void popup_value(int i)
{
    K.edit = i;
    blk_t *b = &K.b[i];
    char v[24];
    fmt_value(b, v, sizeof v, true);
    char title[64];
    snprintf(title, sizeof title, "%s", b->has ? v : _("(sin número)"));
    lv_obj_t *p = pop_new(title);
    pop_btn(p, "-10", step_cb, -10, 0x2c3440);
    pop_btn(p, "-1", step_cb, -1, 0x2c3440);
    pop_btn(p, "+1", step_cb, 1, 0x2c3440);
    pop_btn(p, "+10", step_cb, 10, 0x2c3440);
    static const int GRID[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12 };
    static const int ANG[] = { 30, 45, 60, 72, 90, 120, 144, 50, 100, 150 };
    bool angle = b->kind == B_CMD && (b->op == TG_O_RIGHT || b->op == TG_O_LEFT);
    char t[16];
    if (K.paper && !angle && b->kind == B_CMD) {
        for (int k = 7; k < 10; k++) {
            snprintf(t, sizeof t, "%d", ANG[k]);
            pop_btn(p, t, num_cb, ANG[k], 0x34515e);
        }
    }
    if (angle) {
        for (int k = 0; k < 7; k++) {
            snprintf(t, sizeof t, "%d", ANG[k]);
            pop_btn(p, t, num_cb, ANG[k], 0x34515e);
        }
    } else {
        for (size_t k = 0; k < sizeof(GRID) / sizeof(GRID[0]); k++) {
            snprintf(t, sizeof t, "%d", GRID[k]);
            pop_btn(p, t, num_cb, GRID[k], 0x34515e);
        }
    }
    if (K.world >= 6) {
        for (int k = 0; k < NVAR; k++) pop_btn(p, en() ? VAR_EN[k] : VAR[k], num_cb, -1000 - k, 0x6b5a10);
    }
    if (b->kind == B_CMD && tg_orders[b->op].amin == 0) pop_btn(p, _("sin número"), num_cb, -999, 0x3a3f47);
    pop_btn(p, _("Listo"), pop_close_cb, 0, 0xc77b10);
}

static void sensor_cb(lv_event_t *e)
{
    int o = (int)(intptr_t)lv_event_get_user_data(e);
    blk_t *b = &K.b[K.edit];
    if (o < 0) b->neg = !b->neg;
    else b->op = (uint8_t)o;
    pop_close();
    render();
}

static void popup_sensor(int i)
{
    K.edit = i;
    lv_obj_t *p = pop_new(_("¿Qué pregunta?"));
    for (int o = 0; o < TG_O_N; o++) {
        if (!tg_orders[o].sense || !allowed(o)) continue;
        pop_btn(p, en() ? tg_orders[o].logo_en : tg_orders[o].logo_es, sensor_cb, o,
                K.b[i].op == o ? 0x7a3fb0 : 0x3d3550);
    }
    pop_btn(p, K.b[i].neg ? _("quitar «no»") : _("poner «no»"), sensor_cb, -1, 0x5a2a2a);
}

/* ------------------------------------------------------------ palette */

typedef struct {
    uint8_t kind, op;
} pal_t;

static pal_t s_pal[40];

static int first_sensor(void)
{
    for (int o = 0; o < TG_O_N; o++) {
        if (tg_orders[o].sense && allowed(o)) return o;
    }
    return TG_S_PATH_AHEAD;
}

static void pal_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    pal_t it = s_pal[k];
    blk_t b;
    memset(&b, 0, sizeof b);
    b.var = -1;
    b.kind = it.kind;
    b.op = it.op;
    tg_audio_play(SND_TAP);
    switch (it.kind) {
    case B_CMD:
        if (it.op == TG_O_FORWARD || it.op == TG_O_BACK) {
            b.has = true;
            b.num = K.paper ? 100 : 1;
        } else if ((it.op == TG_O_RIGHT || it.op == TG_O_LEFT) && K.paper) {
            b.has = true;
            b.num = 90;
        } else if (it.op == TG_O_SAY) {
            b.has = true;
            b.var = K.world >= 6 ? 0 : -1;
        } else if (it.op == TG_O_COLOR) {
            b.has = true;
            b.num = 1;
        }
        put(b, false);
        break;
    case B_REPEAT:
        b.has = true;
        b.num = 4;
        put(b, true);
        break;
    case B_IF:
    case B_WHILE:
        b.op = (uint8_t)first_sensor();
        b.neg = it.kind == B_WHILE && allowed(TG_S_AT_GOAL);
        if (b.neg) b.op = TG_S_AT_GOAL;
        put(b, true);
        break;
    case B_ELSE: {
        /* an "else" goes before the end of the "if" we are in */
        for (int i = K.sel; i >= 0; i--) {
            if (K.b[i].kind == B_IF) {
                int z = closer(i);
                bool has_else = false;
                for (int j = i + 1; j < z; j++) has_else = has_else || (K.b[j].kind == B_ELSE && opener(z) == i);
                if (z > 0 && !has_else) {
                    K.sel = z - 1;
                    put(b, false);
                }
                return;
            }
        }
        return;
    }
    case B_TO: {
        int used = 0;
        for (int i = 0; i < K.n; i++) {
            if (K.b[i].kind == B_TO) used |= 1 << K.b[i].proc;
        }
        b.proc = (used & 1) ? ((used & 2) ? 2 : 1) : 0;
        b.param = K.world >= 7;
        put(b, true);
        break;
    }
    case B_CALL: {
        int last = 0;
        bool param = false;
        for (int i = 0; i < K.n; i++) {
            if (K.b[i].kind == B_TO) {
                last = K.b[i].proc;
                param = K.b[i].param;
            }
        }
        b.proc = (uint8_t)last;
        if (param) {
            b.has = true;
            b.num = 1;
        }
        put(b, false);
        break;
    }
    case B_SET:
    case B_ADD:
        b.has = true;
        b.num = it.kind == B_SET ? 0 : 1;
        put(b, false);
        break;
    }
}

static void pal_chip(pal_t it, const char *text, uint32_t color)
{
    if (K.npal >= 40) return;
    s_pal[K.npal] = it;
    lv_obj_t *b = lv_button_create(K.pal);
    lv_obj_set_height(b, 52);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_t *l = tg_label(b, text, aos_font_small, lv_color_hex(0x10140c));
    lv_obj_center(l);
    lv_obj_add_event_cb(b, pal_cb, LV_EVENT_CLICKED, (void *)(intptr_t)K.npal);
    K.npal++;
}

static void build_palette(void)
{
    K.npal = 0;
    for (int o = 0; o < TG_O_N; o++) {
        if (tg_orders[o].sense || !allowed(o)) continue;
        if (o == TG_O_LEFT && allowed(TG_O_RIGHT) && !K.paper) continue;   /* one "turn": touch to flip */
        pal_t it = { B_CMD, (uint8_t)o };
        const char *name = en() ? tg_orders[o].logo_en : tg_orders[o].logo_es;
        if (o == TG_O_RIGHT && !K.paper) name = en() ? "turn" : "girar";
        blk_t tmp = { 0 };
        tmp.kind = B_CMD;
        tmp.op = (uint8_t)o;
        pal_chip(it, name, color_of(&tmp));
    }
    int w = K.world;
    if (w >= 2) pal_chip((pal_t){ B_REPEAT, 0 }, en() ? "repeat" : _("repetir"), 0xff9f0a);
    if (w >= 3) {
        pal_chip((pal_t){ B_TO, 0 }, en() ? "new trick" : _("truco nuevo"), 0x64b5ff);
        pal_chip((pal_t){ B_CALL, 0 }, en() ? "use trick" : _("usar truco"), 0x64b5ff);
    }
    if (w >= 4 && first_sensor() >= 0) {
        pal_chip((pal_t){ B_IF, 0 }, en() ? "if" : "si", 0xc48cff);
        pal_chip((pal_t){ B_ELSE, 0 }, en() ? "else" : "sino", 0xc48cff);
    }
    if (w >= 5) pal_chip((pal_t){ B_WHILE, 0 }, en() ? "while" : "mientras", 0xc48cff);
    if (w >= 6) {
        pal_chip((pal_t){ B_SET, 0 }, en() ? "count =" : "cuenta =", 0xffd60a);
        pal_chip((pal_t){ B_ADD, 0 }, en() ? "count +=" : "cuenta +=", 0xffd60a);
    }
}

static lv_obj_t *tool(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, uint32_t color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 56, 52);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = tg_label(b, sym, &aos_sym_28, AOS_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* ------------------------------------------------------------- setup */

void tg_blocks_open(lv_obj_t *scr, const tg_level_t *lv, int world, const char *logo)
{
    tg_blocks_close();
    K.lv = lv;
    K.world = world;
    K.paper = (lv->goals & TG_G_DRAW) != 0;
    K.mark = -1;
    read_logo(logo);
    K.sel = K.n - 1;

    K.box = lv_obj_create(scr);
    lv_obj_remove_style_all(K.box);
    lv_obj_set_flex_flow(K.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(K.box, 8, 0);
    lv_obj_remove_flag(K.box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mid = lv_obj_create(K.box);
    lv_obj_remove_style_all(mid);
    lv_obj_set_width(mid, LV_PCT(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mid, 8, 0);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    K.list = lv_obj_create(mid);
    lv_obj_remove_style_all(K.list);
    lv_obj_set_height(K.list, LV_PCT(100));
    lv_obj_set_flex_grow(K.list, 1);
    lv_obj_set_style_bg_color(K.list, lv_color_hex(0x1b2027), 0);
    lv_obj_set_style_bg_opa(K.list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(K.list, 16, 0);
    lv_obj_set_style_pad_all(K.list, 10, 0);
    lv_obj_set_style_pad_row(K.list, 6, 0);
    lv_obj_set_flex_flow(K.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(K.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(K.list, LV_DIR_VER);

    K.tools = lv_obj_create(mid);
    lv_obj_remove_style_all(K.tools);
    lv_obj_set_size(K.tools, 56, LV_PCT(100));
    lv_obj_set_flex_flow(K.tools, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(K.tools, 8, 0);
    lv_obj_remove_flag(K.tools, LV_OBJ_FLAG_SCROLLABLE);
    tool(K.tools, AOS_SYM_ARROW_UP, top_cb, 0x2c3440);
    tool(K.tools, AOS_SYM_ARROW_UP_BOLD, up_cb, 0x2c3440);
    tool(K.tools, AOS_SYM_ARROW_DOWN_BOLD, down_cb, 0x2c3440);
    tool(K.tools, AOS_SYM_DELETE, del_cb, 0x6a2a2a);

    K.pal = lv_obj_create(K.box);
    lv_obj_remove_style_all(K.pal);
    lv_obj_set_size(K.pal, LV_PCT(100), 124);
    lv_obj_set_flex_flow(K.pal, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(K.pal, 8, 0);
    lv_obj_set_style_pad_column(K.pal, 8, 0);
    lv_obj_add_flag(K.pal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(K.pal, LV_DIR_VER);
    build_palette();
}

void tg_blocks_place(int x, int y, int w, int h)
{
    if (!K.box) return;
    lv_obj_set_pos(K.box, x, y);
    lv_obj_set_size(K.box, w, h);
    lv_obj_update_layout(K.box);
    render();
}

void tg_blocks_close(void)
{
    pop_close();
    if (K.box) lv_obj_delete(K.box);
    memset(&K, 0, sizeof K);
}

/* the gamepad: up and down choose the block, left and right the piece of
 * the palette, A puts it, B deletes, Y (shoulder L is speed) changes it */
void tg_blocks_pad(aos_pad_t *p)
{
    if (!K.box) return;
    if (K.pop) {
        if (aos_pad_pressed(p, AOS_PAD_B) || aos_pad_pressed(p, AOS_PAD_A)) pop_close_cb(NULL);
        return;
    }
    if (aos_pad_repeat(p, AOS_PAD_UP) && K.sel >= 0) {
        K.sel--;
        render();
    }
    if (aos_pad_repeat(p, AOS_PAD_DOWN) && K.sel < K.n - 1) {
        K.sel++;
        render();
    }
    if (K.npal) {
        uint32_t c = lv_obj_get_child_count(K.pal);
        if (aos_pad_repeat(p, AOS_PAD_RIGHT)) K.pad_pal = (K.pad_pal + 1) % (int)c;
        if (aos_pad_repeat(p, AOS_PAD_LEFT)) K.pad_pal = (K.pad_pal + (int)c - 1) % (int)c;
        for (uint32_t i = 0; i < c; i++) {
            lv_obj_t *o = lv_obj_get_child(K.pal, (int32_t)i);
            lv_obj_set_style_outline_width(o, (int)i == K.pad_pal ? 4 : 0, 0);
            lv_obj_set_style_outline_color(o, lv_color_white(), 0);
        }
        if (aos_pad_pressed(p, AOS_PAD_A)) lv_obj_send_event(lv_obj_get_child(K.pal, K.pad_pal), LV_EVENT_CLICKED, NULL);
    }
    if (aos_pad_pressed(p, AOS_PAD_B)) del_cb(NULL);
}
