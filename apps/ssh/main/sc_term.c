/*
 * P4OS - SSH client: the terminal.
 *
 * What xterm does that a shell, top, htop, vim, nano, less and mc use: the
 * cursor and its moves, erasing, inserting and deleting characters and
 * lines, a scroll region, 16, 256 and 24-bit colours (the last brought to
 * the nearest of the 256), bold, dim, underline and reverse, the alternate
 * screen (1049), application cursor keys, bracketed paste, auto-wrap,
 * tab stops, DEC's line-drawing set, the window title (OSC 0/2), and the
 * answers to "where is the cursor" and "what are you". The mouse modes,
 * blinking, double width and the like are taken and ignored.
 *
 * Cells are a code point and its colours; what the font has no glyph for is
 * the view's business. A character of two columns (CJK, emoji) takes one
 * here: the font has none of them anyway.
 *
 * The screen and the alternate one are rows x cols; what scrolls off the
 * top of the main screen (and only with the scroll region at the top) goes
 * to a ring of SC_SCROLLBACK lines, SC_COLS_MAX wide, in PSRAM.
 */
#include "sc.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { P_GROUND, P_ESC, P_ESC_INTER, P_CSI, P_OSC, P_OSC_ESC, P_STR, P_STR_ESC, P_CHARSET };

#define NPAR 16

typedef struct {
    int cx, cy;
    sc_cell_t pen;
    bool origin, g0_line;
} saved_t;

struct sc_term {
    void *mx;
    int cols, rows;
    sc_cell_t *main, *alt, *scr;
    bool alt_on;
    sc_cell_t *sb;
    int sb_head, sb_count;

    int cx, cy;
    bool wrap_next;
    sc_cell_t pen;
    int top, bot;
    bool autowrap, origin, insert, lnm, app_cursor, cursor_on, bpaste;
    bool g0_line, g1_line, shift_out;
    saved_t saved, saved_alt;
    bool tabs[SC_COLS_MAX];

    int st;
    int par[NPAR], npar;
    bool par_seen;
    char priv, inter;
    char osc[96];
    int oscn;
    uint32_t u8cp;
    int u8need;
    uint32_t last_ch;

    uint32_t dirty[(SC_ROWS_MAX + 31) / 32];
    uint8_t reply[96];
    size_t replyn;
    uint32_t bells;
    char title[64];
};

/* ---- cells and rows ---- */

static sc_cell_t blank(const sc_term_t *t)
{
    sc_cell_t c = { 0, SC_DEF, t->pen.bg, 0 };     /* xterm erases with the background */
    return c;
}

static sc_cell_t *row(sc_term_t *t, int y) { return t->scr + (size_t)y * (size_t)t->cols; }

static void dirty_row(sc_term_t *t, int y)
{
    if (y >= 0 && y < SC_ROWS_MAX) t->dirty[y >> 5] |= 1u << (y & 31);
}

static void dirty_all(sc_term_t *t) { memset(t->dirty, 0xFF, sizeof t->dirty); }

static void dirty_rows(sc_term_t *t, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) dirty_row(t, y);
}

static void fill(sc_term_t *t, int y, int x0, int x1)
{
    if (y < 0 || y >= t->rows) return;
    if (x0 < 0) x0 = 0;
    if (x1 > t->cols) x1 = t->cols;
    sc_cell_t b = blank(t), *r = row(t, y);
    for (int x = x0; x < x1; x++) r[x] = b;
    dirty_row(t, y);
}

static void reply(sc_term_t *t, const char *s)
{
    size_t n = strlen(s);
    if (t->replyn + n <= sizeof t->reply) {
        memcpy(t->reply + t->replyn, s, n);
        t->replyn += n;
    }
}

/* ---- scrolling ---- */

static void to_scrollback(sc_term_t *t, int y)
{
    if (!t->sb) return;
    int idx;
    if (t->sb_count < SC_SCROLLBACK) idx = (t->sb_head + t->sb_count++) % SC_SCROLLBACK;
    else {
        idx = t->sb_head;
        t->sb_head = (t->sb_head + 1) % SC_SCROLLBACK;
    }
    sc_cell_t *d = t->sb + (size_t)idx * SC_COLS_MAX;
    int n = t->cols < SC_COLS_MAX ? t->cols : SC_COLS_MAX;
    memcpy(d, row(t, y), (size_t)n * sizeof *d);
    sc_cell_t b = { 0, SC_DEF, SC_DEF, 0 };
    for (int x = n; x < SC_COLS_MAX; x++) d[x] = b;
}

static void scroll_up(sc_term_t *t, int top, int bot, int n)
{
    if (n <= 0 || top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    if (top == 0 && !t->alt_on)
        for (int i = 0; i < n; i++) to_scrollback(t, i);
    size_t w = (size_t)t->cols * sizeof(sc_cell_t);
    memmove(row(t, top), row(t, top + n), (size_t)(bot - top + 1 - n) * w);
    for (int y = bot - n + 1; y <= bot; y++) fill(t, y, 0, t->cols);
    dirty_rows(t, top, bot);
}

static void scroll_down(sc_term_t *t, int top, int bot, int n)
{
    if (n <= 0 || top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    size_t w = (size_t)t->cols * sizeof(sc_cell_t);
    memmove(row(t, top + n), row(t, top), (size_t)(bot - top + 1 - n) * w);
    for (int y = top; y < top + n; y++) fill(t, y, 0, t->cols);
    dirty_rows(t, top, bot);
}

static void index_down(sc_term_t *t)
{
    if (t->cy == t->bot) scroll_up(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void index_up(sc_term_t *t)
{
    if (t->cy == t->top) scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0) t->cy--;
}

/* ---- the cursor ---- */

static void clamp(sc_term_t *t)
{
    int y0 = t->origin ? t->top : 0, y1 = t->origin ? t->bot : t->rows - 1;
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < y0) t->cy = y0;
    if (t->cy > y1) t->cy = y1;
    t->wrap_next = false;
}

static void move_to(sc_term_t *t, int x, int y)
{
    dirty_row(t, t->cy);
    t->cx = x;
    t->cy = y + (t->origin ? t->top : 0);
    clamp(t);
    dirty_row(t, t->cy);
}

static void save_cursor(sc_term_t *t)
{
    saved_t *s = t->alt_on ? &t->saved_alt : &t->saved;
    s->cx = t->cx;
    s->cy = t->cy;
    s->pen = t->pen;
    s->origin = t->origin;
    s->g0_line = t->g0_line;
}

static void restore_cursor(sc_term_t *t)
{
    saved_t *s = t->alt_on ? &t->saved_alt : &t->saved;
    dirty_row(t, t->cy);
    t->cx = s->cx;
    t->cy = s->cy;
    t->pen = s->pen;
    t->origin = s->origin;
    t->g0_line = s->g0_line;
    clamp(t);
    dirty_row(t, t->cy);
}

static void reset_tabs(sc_term_t *t)
{
    for (int x = 0; x < SC_COLS_MAX; x++) t->tabs[x] = x && x % 8 == 0;
}

static void reset(sc_term_t *t)
{
    t->alt_on = false;
    t->scr = t->main;
    t->pen = (sc_cell_t){ 0, SC_DEF, SC_DEF, 0 };
    t->cx = t->cy = 0;
    t->wrap_next = false;
    t->top = 0;
    t->bot = t->rows - 1;
    t->autowrap = t->cursor_on = true;
    t->origin = t->insert = t->lnm = t->app_cursor = t->bpaste = false;
    t->g0_line = t->g1_line = t->shift_out = false;
    reset_tabs(t);
    for (int y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
    t->scr = t->alt;
    for (int y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
    t->scr = t->main;
    save_cursor(t);
    dirty_all(t);
}

/* ---- writing a character ---- */

/* DEC Special Graphics, 0x60-0x7E: the line-drawing set */
static const uint16_t DEC_LINES[31] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C,
    0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264,
    0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

static bool combining(uint32_t c)
{
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F) ||
           (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) || c == 0x200D;
}

static void put(sc_term_t *t, uint32_t c)
{
    if (combining(c)) return;
    bool line = t->shift_out ? t->g1_line : t->g0_line;
    if (line && c >= 0x60 && c <= 0x7E) c = DEC_LINES[c - 0x60];
    if (t->wrap_next && t->autowrap) {
        t->cx = 0;
        index_down(t);
        t->wrap_next = false;
    }
    sc_cell_t *r = row(t, t->cy);
    if (t->insert && t->cx < t->cols - 1)
        memmove(r + t->cx + 1, r + t->cx, (size_t)(t->cols - t->cx - 1) * sizeof *r);
    sc_cell_t cell = t->pen;
    cell.ch = c;
    r[t->cx] = cell;
    dirty_row(t, t->cy);
    t->last_ch = c;
    if (t->cx == t->cols - 1) t->wrap_next = t->autowrap;
    else t->cx++;
}

/* ---- control sequences ---- */

static int par(sc_term_t *t, int i, int def)
{
    return i < t->npar && t->par[i] > 0 ? t->par[i] : def;
}

static uint16_t rgb_to_256(int r, int g, int b)
{
    int v[3] = { r, g, b }, q[3];
    for (int i = 0; i < 3; i++) q[i] = v[i] < 48 ? 0 : v[i] < 115 ? 1 : (v[i] - 35) / 40;
    return (uint16_t)(16 + 36 * q[0] + 6 * q[1] + q[2]);
}

static void sgr(sc_term_t *t)
{
    if (!t->npar) {
        t->pen.attr = 0;
        t->pen.fg = t->pen.bg = SC_DEF;
        return;
    }
    for (int i = 0; i < t->npar; i++) {
        int p = t->par[i];
        if (p == 0) {
            t->pen.attr = 0;
            t->pen.fg = t->pen.bg = SC_DEF;
        } else if (p == 1) t->pen.attr |= SC_A_BOLD;
        else if (p == 2) t->pen.attr |= SC_A_DIM;
        else if (p == 4) t->pen.attr |= SC_A_UNDER;
        else if (p == 7) t->pen.attr |= SC_A_REV;
        else if (p == 8) t->pen.attr |= SC_A_INVIS;
        else if (p == 21 || p == 22) t->pen.attr &= (uint8_t)~(SC_A_BOLD | SC_A_DIM);
        else if (p == 24) t->pen.attr &= (uint8_t)~SC_A_UNDER;
        else if (p == 27) t->pen.attr &= (uint8_t)~SC_A_REV;
        else if (p == 28) t->pen.attr &= (uint8_t)~SC_A_INVIS;
        else if (p >= 30 && p <= 37) t->pen.fg = (uint16_t)(p - 30);
        else if (p == 39) t->pen.fg = SC_DEF;
        else if (p >= 40 && p <= 47) t->pen.bg = (uint16_t)(p - 40);
        else if (p == 49) t->pen.bg = SC_DEF;
        else if (p >= 90 && p <= 97) t->pen.fg = (uint16_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->pen.bg = (uint16_t)(p - 100 + 8);
        else if ((p == 38 || p == 48) && i + 1 < t->npar) {
            uint16_t col = SC_DEF;
            if (t->par[i + 1] == 5 && i + 2 < t->npar) {
                col = (uint16_t)(t->par[i + 2] & 255);
                i += 2;
            } else if (t->par[i + 1] == 2 && i + 4 < t->npar) {
                col = rgb_to_256(t->par[i + 2], t->par[i + 3], t->par[i + 4]);
                i += 4;
            } else {
                i++;
                continue;
            }
            if (p == 38) t->pen.fg = col;
            else t->pen.bg = col;
        }
    }
}

static void use_alt(sc_term_t *t, bool on, bool clear)
{
    if (on == t->alt_on) return;
    t->alt_on = on;
    t->scr = on ? t->alt : t->main;
    if (on && clear)
        for (int y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
    dirty_all(t);
}

static void mode(sc_term_t *t, bool on)
{
    for (int i = 0; i < (t->npar ? t->npar : 1); i++) {
        int p = t->npar ? t->par[i] : 0;
        if (t->priv == '?') {
            switch (p) {
            case 1: t->app_cursor = on; break;
            case 6: t->origin = on; move_to(t, 0, 0); break;
            case 7: t->autowrap = on; break;
            case 25: t->cursor_on = on; dirty_row(t, t->cy); break;
            case 47:
            case 1047: use_alt(t, on, on); break;
            case 1048: if (on) save_cursor(t); else restore_cursor(t); break;
            case 1049:
                if (on) {
                    save_cursor(t);
                    use_alt(t, true, true);
                } else {
                    use_alt(t, false, false);
                    restore_cursor(t);
                }
                break;
            case 2004: t->bpaste = on; break;
            default: break;                 /* mouse, blinking, focus: taken, not done */
            }
        } else if (!t->priv) {
            if (p == 4) t->insert = on;
            else if (p == 20) t->lnm = on;
        }
    }
}

static void erase_display(sc_term_t *t, int how)
{
    if (how == 0) {
        fill(t, t->cy, t->cx, t->cols);
        for (int y = t->cy + 1; y < t->rows; y++) fill(t, y, 0, t->cols);
    } else if (how == 1) {
        fill(t, t->cy, 0, t->cx + 1);
        for (int y = 0; y < t->cy; y++) fill(t, y, 0, t->cols);
    } else if (how == 2) {
        for (int y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
    } else if (how == 3 && !t->alt_on) {
        t->sb_count = t->sb_head = 0;       /* clear's ESC[3J: the scrollback too */
        dirty_all(t);
    }
}

static void csi(sc_term_t *t, char f)
{
    int n = par(t, 0, 1);
    sc_cell_t *r = row(t, t->cy);
    if (t->priv == '>' || t->priv == '=') {
        if (f == 'c' && t->priv == '>') reply(t, "\x1b[>0;276;0c");
        return;                             /* xterm's key options and the like */
    }
    if (t->inter && f != 'q' && f != 'p') return;
    switch (f) {
    case 'A': move_to(t, t->cx, t->cy - (t->origin ? t->top : 0) - n); break;
    case 'B': case 'e': move_to(t, t->cx, t->cy - (t->origin ? t->top : 0) + n); break;
    case 'C': case 'a': move_to(t, t->cx + n, t->cy - (t->origin ? t->top : 0)); break;
    case 'D': move_to(t, t->cx - n, t->cy - (t->origin ? t->top : 0)); break;
    case 'E': move_to(t, 0, t->cy - (t->origin ? t->top : 0) + n); break;
    case 'F': move_to(t, 0, t->cy - (t->origin ? t->top : 0) - n); break;
    case 'G': case '`': move_to(t, n - 1, t->cy - (t->origin ? t->top : 0)); break;
    case 'd': move_to(t, t->cx, n - 1); break;
    case 'H': case 'f': move_to(t, par(t, 1, 1) - 1, n - 1); break;
    case 'J': erase_display(t, t->npar ? t->par[0] : 0); break;
    case 'K': {
        int how = t->npar ? t->par[0] : 0;
        if (how == 0) fill(t, t->cy, t->cx, t->cols);
        else if (how == 1) fill(t, t->cy, 0, t->cx + 1);
        else fill(t, t->cy, 0, t->cols);
        break;
    }
    case 'X': fill(t, t->cy, t->cx, t->cx + n); break;
    case '@':
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(r + t->cx + n, r + t->cx, (size_t)(t->cols - t->cx - n) * sizeof *r);
        fill(t, t->cy, t->cx, t->cx + n);
        break;
    case 'P':
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(r + t->cx, r + t->cx + n, (size_t)(t->cols - t->cx - n) * sizeof *r);
        fill(t, t->cy, t->cols - n, t->cols);
        break;
    case 'L':
        if (t->cy >= t->top && t->cy <= t->bot) scroll_down(t, t->cy, t->bot, n);
        t->cx = 0;
        break;
    case 'M':
        if (t->cy >= t->top && t->cy <= t->bot) scroll_up(t, t->cy, t->bot, n);
        t->cx = 0;
        break;
    case 'S': scroll_up(t, t->top, t->bot, n); break;
    case 'T': scroll_down(t, t->top, t->bot, n); break;
    case 'b':
        for (int i = 0; i < n && i < t->cols && t->last_ch; i++) put(t, t->last_ch);
        break;
    case 'I':
        for (int i = 0; i < n; i++) {
            do t->cx++;
            while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
            if (t->cx >= t->cols) t->cx = t->cols - 1;
        }
        break;
    case 'Z':
        for (int i = 0; i < n && t->cx > 0; i++) {
            do t->cx--;
            while (t->cx > 0 && !t->tabs[t->cx]);
        }
        break;
    case 'g':
        if (!t->npar || !t->par[0]) t->tabs[t->cx < SC_COLS_MAX ? t->cx : 0] = false;
        else if (t->par[0] == 3) memset(t->tabs, 0, sizeof t->tabs);
        break;
    case 'm': sgr(t); break;
    case 'r': {
        int top = par(t, 0, 1) - 1, bot = par(t, 1, t->rows) - 1;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) {
            t->top = top;
            t->bot = bot;
            move_to(t, 0, 0);
        }
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    case 'h': mode(t, true); break;
    case 'l': mode(t, false); break;
    case 'c': reply(t, "\x1b[?1;2c"); break;
    case 'n':
        if (t->priv) break;
        if (n == 5) reply(t, "\x1b[0n");
        else if (n == 6) {
            char b[24];
            snprintf(b, sizeof b, "\x1b[%d;%dR", t->cy + 1 - (t->origin ? t->top : 0), t->cx + 1);
            reply(t, b);
        }
        break;
    default: break;                         /* window ops, cursor shape: taken, not done */
    }
}

static void esc(sc_term_t *t, char c)
{
    switch (c) {
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'D': index_down(t); break;
    case 'E': t->cx = 0; index_down(t); break;
    case 'M': index_up(t); break;
    case 'H': if (t->cx < SC_COLS_MAX) t->tabs[t->cx] = true; break;
    case 'c': reset(t); break;
    default: break;                         /* '=', '>': the keypad's mode */
    }
    t->wrap_next = false;
    dirty_row(t, t->cy);
}

static void osc_done(sc_term_t *t)
{
    t->osc[t->oscn] = 0;
    if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') sc_copy(t->title, sizeof t->title, t->osc + 2);
}

static void control(sc_term_t *t, uint8_t c)
{
    switch (c) {
    case 7: t->bells++; break;
    case 8:
        if (t->cx > 0) t->cx--;
        t->wrap_next = false;
        dirty_row(t, t->cy);
        break;
    case 9:
        do t->cx++;
        while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        dirty_row(t, t->cy);
        break;
    case 10: case 11: case 12:
        dirty_row(t, t->cy);
        index_down(t);
        if (t->lnm) t->cx = 0;
        t->wrap_next = false;
        dirty_row(t, t->cy);
        break;
    case 13: t->cx = 0; t->wrap_next = false; dirty_row(t, t->cy); break;
    case 14: t->shift_out = true; break;
    case 15: t->shift_out = false; break;
    default: break;
    }
}

static void byte(sc_term_t *t, uint8_t b)
{
    /* inside a string (OSC, DCS...) everything is its until BEL or ESC \ */
    if (t->st == P_OSC) {
        if (b == 7) { osc_done(t); t->st = P_GROUND; }
        else if (b == 0x1b) t->st = P_OSC_ESC;
        else if (t->oscn < (int)sizeof t->osc - 1) t->osc[t->oscn++] = (char)b;
        return;
    }
    if (t->st == P_OSC_ESC) {
        osc_done(t);
        t->st = P_GROUND;
        if (b == '\\') return;
    }
    if (t->st == P_STR) {
        if (b == 0x1b) t->st = P_STR_ESC;
        else if (b == 7) t->st = P_GROUND;
        return;
    }
    if (t->st == P_STR_ESC) {
        t->st = b == '\\' ? P_GROUND : P_STR;
        return;
    }

    if (b == 0x1b) {
        t->st = P_ESC;
        t->u8need = 0;
        return;
    }
    if (b < 0x20 || b == 0x7F) {
        if (b != 0x7F) control(t, b);      /* even in the middle of a sequence, as xterm */
        return;
    }
    switch (t->st) {
    case P_ESC:
        if (b == '[') {
            t->st = P_CSI;
            t->npar = 0;
            t->par_seen = false;
            t->priv = t->inter = 0;
            memset(t->par, 0, sizeof t->par);
        } else if (b == ']') {
            t->st = P_OSC;
            t->oscn = 0;
        } else if (b == 'P' || b == 'X' || b == '^' || b == '_') t->st = P_STR;
        else if (b == '(' || b == ')') {
            t->inter = (char)b;
            t->st = P_CHARSET;
        } else if (b >= 0x20 && b <= 0x2F) {
            t->st = P_ESC_INTER;
        } else {
            t->st = P_GROUND;
            esc(t, (char)b);
        }
        return;
    case P_ESC_INTER:
        if (b >= 0x30) t->st = P_GROUND;    /* ESC # 8 and friends: not done */
        return;
    case P_CHARSET:
        if (t->inter == '(') t->g0_line = b == '0';
        else t->g1_line = b == '0';
        t->st = P_GROUND;
        return;
    case P_CSI:
        if (b >= '0' && b <= '9') {
            if (!t->npar) t->npar = 1;
            int *p = &t->par[t->npar - 1];
            if (*p < 100000) *p = *p * 10 + (b - '0');
            t->par_seen = true;
        } else if (b == ';' || b == ':') {
            if (!t->npar) t->npar = 1;
            if (t->npar < NPAR) t->npar++;
        } else if (b >= '<' && b <= '?' && !t->npar && !t->priv) {
            t->priv = (char)b;
        } else if (b >= 0x20 && b <= 0x2F) {
            t->inter = (char)b;
        } else if (b >= 0x40 && b <= 0x7E) {
            t->st = P_GROUND;
            csi(t, (char)b);
        } else t->st = P_GROUND;
        return;
    default:
        break;
    }

    /* text, as UTF-8 */
    if (b < 0x80) {
        t->u8need = 0;
        put(t, b);
    } else if ((b & 0xC0) == 0x80) {
        if (!t->u8need) return;
        t->u8cp = t->u8cp << 6 | (b & 0x3F);
        if (--t->u8need == 0) put(t, t->u8cp);
    } else if ((b & 0xE0) == 0xC0) {
        t->u8cp = b & 0x1F;
        t->u8need = 1;
    } else if ((b & 0xF0) == 0xE0) {
        t->u8cp = b & 0x0F;
        t->u8need = 2;
    } else if ((b & 0xF8) == 0xF0) {
        t->u8cp = b & 0x07;
        t->u8need = 3;
    } else t->u8need = 0;
}

/* ---- the API ---- */

sc_term_t *sc_term_new(int cols, int rows)
{
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    if (cols > SC_COLS_MAX) cols = SC_COLS_MAX;
    if (rows > SC_ROWS_MAX) rows = SC_ROWS_MAX;
    sc_term_t *t = sc_psram(sizeof *t);
    if (!t) return NULL;
    t->mx = aos_hal_mutex_create();
    t->cols = cols;
    t->rows = rows;
    t->main = sc_psram((size_t)cols * (size_t)rows * sizeof(sc_cell_t));
    t->alt = sc_psram((size_t)cols * (size_t)rows * sizeof(sc_cell_t));
    t->sb = sc_psram((size_t)SC_SCROLLBACK * SC_COLS_MAX * sizeof(sc_cell_t));
    if (!t->mx || !t->main || !t->alt) {
        sc_term_free(t);
        return NULL;
    }
    reset(t);
    return t;
}

void sc_term_free(sc_term_t *t)
{
    if (!t) return;
    free(t->main);
    free(t->alt);
    free(t->sb);
    free(t);                    /* the mutex: the HAL has no delete; it is small */
}

void sc_term_lock(sc_term_t *t) { aos_hal_mutex_lock(t->mx); }
void sc_term_unlock(sc_term_t *t) { aos_hal_mutex_unlock(t->mx); }

void sc_term_feed(sc_term_t *t, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) byte(t, d[i]);
}

void sc_term_resize(sc_term_t *t, int cols, int rows)
{
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    if (cols > SC_COLS_MAX) cols = SC_COLS_MAX;
    if (rows > SC_ROWS_MAX) rows = SC_ROWS_MAX;
    if (cols == t->cols && rows == t->rows) return;
    sc_cell_t *nm = sc_psram((size_t)cols * (size_t)rows * sizeof(sc_cell_t));
    sc_cell_t *na = sc_psram((size_t)cols * (size_t)rows * sizeof(sc_cell_t));
    if (!nm || !na) {
        free(nm);
        free(na);
        return;
    }
    /* keep the lines around the cursor: when there are fewer rows, the top
     * ones go to the scrollback, as a terminal window made shorter does */
    bool was_alt = t->alt_on;
    int shift = t->cy >= rows ? t->cy - rows + 1 : 0;
    t->scr = t->main;
    t->alt_on = false;
    for (int y = 0; y < shift; y++) to_scrollback(t, y);
    sc_cell_t b = { 0, SC_DEF, SC_DEF, 0 };
    for (int s = 0; s < 2; s++) {
        sc_cell_t *from = s ? t->alt : t->main, *to = s ? na : nm;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                int sy = y + shift;
                to[(size_t)y * (size_t)cols + (size_t)x] =
                    sy < t->rows && x < t->cols ? from[(size_t)sy * (size_t)t->cols + (size_t)x] : b;
            }
    }
    free(t->main);
    free(t->alt);
    t->main = nm;
    t->alt = na;
    t->cols = cols;
    t->rows = rows;
    t->alt_on = was_alt;
    t->scr = was_alt ? t->alt : t->main;
    t->cy -= shift;
    t->top = 0;
    t->bot = rows - 1;
    t->saved.cy = t->saved.cy - shift < 0 ? 0 : t->saved.cy - shift;
    t->saved_alt.cy = t->saved_alt.cy < rows ? t->saved_alt.cy : rows - 1;
    if (t->saved.cy >= rows) t->saved.cy = rows - 1;
    if (t->saved.cx >= cols) t->saved.cx = cols - 1;
    if (t->saved_alt.cx >= cols) t->saved_alt.cx = cols - 1;
    clamp(t);
    dirty_all(t);
}

void sc_term_size(sc_term_t *t, int *cols, int *rows)
{
    *cols = t->cols;
    *rows = t->rows;
}

const sc_cell_t *sc_term_row(sc_term_t *t, int y, int back)
{
    if (y < 0 || y >= t->rows) return NULL;
    int line = y - back;
    if (line >= 0) return row(t, line);
    if (t->alt_on || !t->sb) return NULL;
    int idx = t->sb_count + line;
    if (idx < 0) return NULL;
    return t->sb + (size_t)((t->sb_head + idx) % SC_SCROLLBACK) * SC_COLS_MAX;
}

int sc_term_scrollback(sc_term_t *t) { return t->alt_on ? 0 : t->sb_count; }

bool sc_term_cursor(sc_term_t *t, int *x, int *y)
{
    *x = t->cx;
    *y = t->cy;
    return t->cursor_on;
}

bool sc_term_take_dirty(sc_term_t *t, uint32_t dirty[(SC_ROWS_MAX + 31) / 32])
{
    bool any = false;
    for (size_t i = 0; i < (SC_ROWS_MAX + 31) / 32; i++) {
        dirty[i] = t->dirty[i];
        any |= dirty[i] != 0;
        t->dirty[i] = 0;
    }
    return any;
}

size_t sc_term_take_reply(sc_term_t *t, uint8_t *out, size_t cap)
{
    size_t n = t->replyn < cap ? t->replyn : cap;
    memcpy(out, t->reply, n);
    memmove(t->reply, t->reply + n, t->replyn - n);
    t->replyn -= n;
    return n;
}

bool sc_term_app_cursor(sc_term_t *t) { return t->app_cursor; }
bool sc_term_bracketed_paste(sc_term_t *t) { return t->bpaste; }
uint32_t sc_term_bells(sc_term_t *t) { return t->bells; }
void sc_term_title(sc_term_t *t, char *out, size_t n) { sc_copy(out, n, t->title); }
