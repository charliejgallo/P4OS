/*
 * CHATARRA - the interface
 *
 * The HUD, the dialogue panel, the menu, the workshop, the items, the robot's
 * data card, the shop and the title screen. All drawn onto the same canvas
 * with the same primitives as the map and the combat: there is not a single
 * LVGL object in the whole game.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE ARE NO WIDGETS
 * ---------------------------------------------------------------------------
 *
 * The temptation is to build the menus with LVGL labels and buttons, which is
 * shorter to write. On the watch two measurements said no - every LVGL object
 * was internal RAM, and rebuilding a twenty-row list cost 111-124 ms with the
 * LVGL thread blocked - and the third reason survives the move to the P4: the
 * canvas font and the canvas art are one style, and a menu made of the OS's
 * widgets would be a different game's menu. Drawing on the canvas, a whole
 * menu is a rectangle and six lines of text, and costs the same as a frame of
 * the map.
 *
 * ---------------------------------------------------------------------------
 * WHERE THINGS GO (P4OS)
 * ---------------------------------------------------------------------------
 *
 * In UI units, inside `ch_lay` (chatarra.h): the screen area is SW x SH -
 * 180x258 standing up, 250x180 lying down - and the HUD is beside it, never
 * under it. The watch drew every menu into a 184x168 box with a dead strip
 * below; here the lists grow rows to fill what they are given, the header
 * carries a back button, and nothing touchable goes into the lowest 9 units
 * of the glass, where the system's home swipe starts.
 */
#include "chatarra.h"

#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Drawing helpers
 * -------------------------------------------------------------------------- */

/* THE PANEL, IN SCRAP METAL.
 *
 * On the watch a panel was a dark box with a one-pixel frame, because one
 * pixel was two real ones and that was all the detail there was room for.
 * Here a UI unit is four real pixels, so the frame is drawn at the canvas's
 * own resolution: a lit edge along the top, a shaded one along the bottom,
 * a slightly lighter plate in the middle and a rivet in each corner - which
 * is what a panel in a game called Chatarra ought to be made of. */
void ch_panel(ch_buf_t *b0, int x, int y, int w, int h, uint16_t borde)
{
    ch_buf_t nb = ch_nativo(b0);
    ch_buf_t *b = &nb;
    int k = b0->s;
    int X = ch_fx(b0, x), Y = ch_fy(b0, y), W = w * k, H = h * k;
    uint16_t fondo = ch_rgb(0x141824);

    ch_round(b, X, Y, W, H, 2 * k, borde);
    ch_round(b, X + 1, Y + 1, W - 2, H - 2, 2 * k - 1, ch_rgb(0x05060C));
    ch_vgrad(b, X + 2, Y + 3, W - 4, Y + H - 4, ch_tone(fondo, 2),
             ch_tone(fondo, -3));
    ch_hline(b, X + 2 * k, Y + 2, W - 4 * k, ch_tone(borde, 5));
    ch_hline(b, X + 2 * k, Y + H - 3, W - 4 * k, ch_rgb(0x0A0C14));
    /* the rivets, lit on top like the robots' */
    if (w >= 24 && h >= 16) {
        int rx[2] = { X + 3 * k, X + W - 3 * k - 2 };
        int ry[2] = { Y + 3 * k, Y + H - 3 * k - 2 };
        for (int i = 0; i < 4; i++) {
            int px = rx[i & 1], py = ry[i >> 1];
            ch_rect(b, px, py, 3, 3, ch_tone(borde, -4));
            ch_rect(b, px, py, 2, 2, borde);
            ch_rect(b, px, py, 1, 1, ch_tone(borde, 8));
        }
    }
}

/* A BUTTON, A ROW, A TILE: the same plate with less ceremony. `fondo` is the
 * face, `borde` the frame; the face is lit from above and the foot is
 * shaded, at the canvas's resolution. */
void ch_caja(ch_buf_t *b0, int x, int y, int w, int h, uint16_t fondo,
             uint16_t borde)
{
    ch_buf_t nb = ch_nativo(b0);
    ch_buf_t *b = &nb;
    int k = b0->s;
    int X = ch_fx(b0, x), Y = ch_fy(b0, y), W = w * k, H = h * k;

    if (W < 4 || H < 4) return;
    ch_round(b, X, Y, W, H, k + 1, borde);
    ch_vgrad(b, X + 1, Y + 1, W - 2, Y + H - 2, ch_tone(fondo, 3),
             ch_tone(fondo, -3));
    ch_hline(b, X + k, Y + 1, W - 2 * k, ch_tone(fondo, 6));
    ch_hline(b, X + k, Y + H - 2, W - 2 * k, ch_tone(fondo, -6));
}

void ch_barra(ch_buf_t *b, int x, int y, int w, int v, int vmax, uint16_t c)
{
    int lleno;

    if (vmax < 1) vmax = 1;
    if (v < 0) v = 0;
    if (v > vmax) v = vmax;
    lleno = (w - 2) * v / vmax;

    ch_rect(b, x, y, w, 6, ch_rgb(0x05060C));
    ch_rect(b, x + 1, y + 1, w - 2, 4, ch_rgb(0x2B3145));
    if (lleno > 0) {
        /* Red when little is left: it is the only information you have to be
         * able to read out of the corner of your eye while playing. */
        uint16_t col = c;
        if (v * 5 <= vmax)      col = ch_rgb(0xFF4A3D);
        else if (v * 5 <= vmax * 2) col = ch_rgb(0xFF9F0A);
        ch_rect(b, x + 1, y + 1, lleno, 4, col);
        /* the gloss: half a unit, which only exists at the canvas's scale */
        {
            ch_buf_t nb = ch_nativo(b);
            ch_rect(&nb, ch_fx(b, x + 1), ch_fy(b, y + 1), lleno * b->s,
                    b->s > 1 ? b->s / 2 : 1, ch_tone(col, 6));
            ch_rect(&nb, ch_fx(b, x + 1), ch_fy(b, y + 5) - 1, lleno * b->s,
                    1, ch_tone(col, -5));
        }
    }
}

/* Breaks the text into lines of at most 'ancho' characters, honouring any
 * breaks it already carries. Returns how many lines came out. */
int ch_wrap(const char *s, int ancho, char dst[][30], int max)
{
    int n = 0, k = 0;

    if (ancho > 29) ancho = 29;
    dst[0][0] = 0;

    while (*s && n < max) {
        if (*s == '\n') {
            dst[n][k] = 0;
            n++; k = 0;
            if (n < max) dst[n][0] = 0;
            s++;
            continue;
        }
        if (k >= ancho) {
            /* search backwards for the last space so words are not split */
            int corte = k;
            while (corte > 0 && dst[n][corte - 1] != ' ') corte--;
            if (corte == 0) corte = k;      /* one very long word             */
            else            s -= (k - corte);
            dst[n][corte] = 0;
            n++; k = 0;
            if (n < max) dst[n][0] = 0;
            while (*s == ' ') s++;
            continue;
        }
        dst[n][k++] = *s++;
    }
    if (n < max) { dst[n][k] = 0; if (k) n++; }
    return n;
}

/* --------------------------------------------------------------------------
 * The HUD
 *
 * The one thing you have to be able to look at without stopping playing:
 * where you are, how much you have, and how your robot is. On the watch it
 * lived in the strip the touch panel could not reach; here it is touchable,
 * and it carries the MENU button and the zoom.
 *
 *   standing up   a strip under the map, 180 x 62: the numbers on the left,
 *                 MENU and the zoom on the right;
 *   lying down    a column beside the map, 70 x 180, the same things one
 *                 under the other.
 * -------------------------------------------------------------------------- */

/* THE BAR, WITH A BODY.
 *
 * The flat one was four pixels of solid colour in a groove. It reads, but it
 * reads like a progress bar in a dialogue box: at a glance you cannot tell
 * half from a third, and half from a third is the whole decision of whether
 * to drink the oil now or after the next hit. This one is framed, lit along
 * the top and shaded along the bottom so it has a shape, and notched at every
 * quarter so the eye measures instead of guessing. */
static void barra_hud(ch_buf_t *b, int x, int y, int w, int h, int v, int vmax,
                      uint32_t base)
{
    int lleno;

    if (vmax < 1) vmax = 1;
    if (v < 0) v = 0;
    if (v > vmax) v = vmax;
    lleno = (w - 2) * v / vmax;

    ch_rect(b, x, y, w, h, ch_rgb(0x05060C));
    ch_rect(b, x + 1, y + 1, w - 2, h - 2, ch_rgb(0x1A2133));

    if (lleno > 0) {
        /* Red when little is left: it is the only information you have to be
         * able to read out of the corner of your eye while playing. */
        uint16_t col = ch_rgb(base);
        if (v * 5 <= vmax)          col = ch_rgb(0xFF4A3D);
        else if (v * 5 <= vmax * 2) col = ch_rgb(0xFF9F0A);
        ch_vgrad(b, x + 1, y + 1, lleno, y + h - 2, ch_tone(col, 4),
                 ch_tone(col, -4));
        ch_rect(b, x + 1, y + 1, lleno, 1, ch_tone(col,  6));
    }
    for (int k = 1; k < 4; k++) {                   /* las marcas del cuarto */
        ch_rect(b, x + 1 + (w - 2) * k / 4, y + 1, 1, h - 2, ch_rgb(0x05060C));
    }
    ch_frame(b, x, y, w, h, ch_rgb(0x3D465F));
}

/* The HUD's buttons, in UI units. The zoom's two only mean something on the
 * map; elsewhere they are drawn grey and do nothing. */
enum { HB_MENU = 0, HB_MENOS, HB_MAS, HB_N };

static void hud_boton_caja(int i, int *x, int *y, int *w, int *h)
{
    const ch_lay_t *l = &ch_lay;

    if (l->horiz) {
        int by = l->hh - l->pie - 50;
        if (i == HB_MENU) { *x = l->hx + 5; *y = by; *w = l->hw - 10; *h = 26; }
        else {
            *w = (l->hw - 14) / 2; *h = 20; *y = by + 29;
            *x = l->hx + 5 + (i == HB_MAS ? *w + 4 : 0);
        }
    } else {
        int bx = l->hw - 46;
        if (i == HB_MENU) { *x = bx; *y = l->hy + 5; *w = 42; *h = 26; }
        else {
            *w = 19; *h = 18; *y = l->hy + 34;
            *x = bx + (i == HB_MAS ? 23 : 0);
        }
    }
}

static int hud_boton_en(int bx, int by)
{
    for (int i = 0; i < HB_N; i++) {
        int x, y, w, h;
        hud_boton_caja(i, &x, &y, &w, &h);
        /* the targets are a little bigger than the drawing, as the OS's are */
        if (bx >= x - 2 && bx < x + w + 2 && by >= y - 2 && by < y + h + 2) {
            return i;
        }
    }
    return -1;
}

/* A magnifying glass with a sign in it: minus or plus. */
static void lupa(ch_buf_t *b0, int x, int y, int w, int h, bool mas, uint16_t c)
{
    ch_buf_t nb = ch_nativo(b0);
    int k = b0->s;
    int cx = ch_fx(b0, x) + (w * k) / 2 - k, cy = ch_fy(b0, y) + (h * k) / 2 - k;
    int r = 5 * k / 2 + 2;

    ch_ring(&nb, cx, cy, r, c);
    ch_ring(&nb, cx, cy, r - 1, c);
    for (int i = 0; i < 2 * k + 2; i++) {
        ch_rect(&nb, cx + r - 1 + i, cy + r - 1 + i, 3, 2, c);
    }
    ch_rect(&nb, cx - r / 2, cy - 1, r + 1, 2, c);
    if (mas) ch_rect(&nb, cx - 1, cy - r / 2, 2, r + 1, c);
}

static void hud_botones(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    bool mapa = g->modo == MODO_MAPA;
    int x, y, w, h;

    hud_boton_caja(HB_MENU, &x, &y, &w, &h);
    bool menu = g->modo == MODO_MAPA || g->modo == MODO_DIALOGO;
    ch_caja(b, x, y, w, h, ch_rgb(menu ? 0x2A3350 : 0x3A2A22),
            ch_rgb(0x8A93AB));
    ch_ui_icono(b, x + 3, y + (h - 12) / 2, IC_MOCHILA, 1);
    ch_text_sh(b, x + 16, y + h / 2 - 3,
               menu ? _("MENU") : _("MAPA"),
               ch_rgb(0xFFFFFF), ch_rgb(0x05060C));

    for (int i = HB_MENOS; i <= HB_MAS; i++) {
        bool puede = mapa && (i == HB_MAS ? g->zoom < ZOOM_MAX : g->zoom > ZOOM_MIN);
        hud_boton_caja(i, &x, &y, &w, &h);
        ch_caja(b, x, y, w, h, ch_rgb(puede ? 0x232B41 : 0x141824),
                ch_rgb(puede ? 0x606B85 : 0x2B3145));
        lupa(b, x, y, w, h, i == HB_MAS,
             ch_rgb(puede ? 0xD5DCEB : 0x3D465F));
    }
}

void ch_ui_hud(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    const ch_lay_t *l = &ch_lay;
    const ch_room_t *r = &ch_salas[g->s.sala % ch_nsalas];
    const ch_robot_t *yo = &g->s.yo;
    int X = l->hx, Y = l->hy;
    char t[40];

    ch_rect(b, X, Y, l->hw, l->hh, ch_rgb(0x0B0D14));
    if (l->horiz) {
        ch_vgrad(b, X + 2, Y, l->hw - 2, Y + 30, ch_rgb(0x161C2E), ch_rgb(0x0B0D14));
        ch_vline(b, X, Y, l->hh, ch_rgb(0x3D465F));
        ch_vline(b, X + 1, Y, l->hh, ch_rgb(0x1A2133));
    } else {
        ch_vgrad(b, X, Y + 2, l->hw, Y + 14, ch_rgb(0x161C2E), ch_rgb(0x0B0D14));
        ch_hline(b, X, Y,     l->hw, ch_rgb(0x3D465F));
        ch_hline(b, X, Y + 1, l->hw, ch_rgb(0x1A2133));
    }

    if (l->horiz) {
        /* --- the column --------------------------------------------------
         * Eleven characters to a line: the room's name takes two lines, and
         * the notice replaces it while it lasts. */
        char lin[2][30];
        int n = ch_wrap(g->aviso_t ? g->aviso : _(r->nombre), 11, lin, 2);
        int x = X + 5;
        for (int i = 0; i < n; i++) {
            ch_text(b, x, Y + 5 + i * 9, lin[i],
                    g->aviso_t ? ch_rgb(0xFFE45E) : ch_rgb(0xD5DCEB));
        }
        snprintf(t, sizeof(t), _("%dC"), g->s.creditos);
        ch_text(b, x, Y + 25, t, ch_rgb(0xFFE45E));

        ch_text(b, x, Y + 38, _("VID"), ch_rgb(0x8A93AB));
        snprintf(t, sizeof(t), "%d/%d", yo->vida, yo->vida_max);
        ch_text(b, X + l->hw - 5 - ch_text_w(t), Y + 38, t, ch_rgb(0xD5DCEB));
        barra_hud(b, x, Y + 47, l->hw - 10, 9, yo->vida, yo->vida_max, 0x4ADE80);

        ch_text(b, x, Y + 61, _("ENE"), ch_rgb(0x8A93AB));
        barra_hud(b, x, Y + 70, l->hw - 10, 7, yo->ene, yo->ene_max, 0x4A9DF5);
        {
            uint32_t base = ch_exp_nivel(yo->nivel);
            uint32_t sig  = ch_exp_nivel(yo->nivel + 1);
            uint32_t hoy  = yo->exp > base ? yo->exp - base : 0;
            uint32_t tot  = sig > base ? sig - base : 1;
            ch_text(b, x, Y + 81, _("EXP"), ch_rgb(0x8A93AB));
            barra_hud(b, x, Y + 90, l->hw - 10, 7, (int)hoy, (int)tot, 0xBF5AF2);
        }
        snprintf(t, sizeof(t), "%.11s", ch_robot_nombre(yo));
        ch_text(b, x, Y + 102, t, ch_rgb(0xFFFFFF));
        snprintf(t, sizeof(t), _("N%d"), yo->nivel);
        ch_text(b, x, Y + 111, t, ch_rgb(0xFFE45E));
    } else {
        /* --- the strip ---------------------------------------------------
         * Numbers in the left 130 units, the buttons in the right 46. */
        int x = X + 5, der = X + l->hw - 50;

        snprintf(t, sizeof(t), "%.21s", g->aviso_t ? g->aviso : _(r->nombre));
        ch_text(b, x, Y + 5, t, g->aviso_t ? ch_rgb(0xFFE45E) : ch_rgb(0xD5DCEB));

        ch_text(b, x, Y + 18, _("VID"), ch_rgb(0x8A93AB));
        barra_hud(b, x + 21, Y + 16, 58, 11, yo->vida, yo->vida_max, 0x4ADE80);
        snprintf(t, sizeof(t), "%d/%d", yo->vida, yo->vida_max);
        ch_text(b, der - ch_text_w(t), Y + 18, t, ch_rgb(0xD5DCEB));

        ch_text(b, x, Y + 32, _("ENE"), ch_rgb(0x8A93AB));
        barra_hud(b, x + 21, Y + 30, 36, 11, yo->ene, yo->ene_max, 0x4A9DF5);
        {
            uint32_t base = ch_exp_nivel(yo->nivel);
            uint32_t sig  = ch_exp_nivel(yo->nivel + 1);
            uint32_t hoy  = yo->exp > base ? yo->exp - base : 0;
            uint32_t tot  = sig > base ? sig - base : 1;
            ch_text(b, x + 62, Y + 32, _("EXP"), ch_rgb(0x8A93AB));
            barra_hud(b, x + 82, Y + 30, der - x - 82, 11, (int)hoy, (int)tot,
                      0xBF5AF2);
        }
        snprintf(t, sizeof(t), _("%s  N%d"), ch_robot_nombre(yo), yo->nivel);
        ch_text(b, x, Y + 45, t, ch_rgb(0xFFFFFF));
        snprintf(t, sizeof(t), _("%dC"), g->s.creditos);
        ch_text(b, der - ch_text_w(t), Y + 45, t, ch_rgb(0xFFE45E));
    }

    hud_botones(g);

    /* and the copy to what is on screen: nothing covers the HUD, so it does
     * not need to go through the dirty rectangle list */
    {
        ch_rect_t q = { (int16_t)X, (int16_t)Y, (int16_t)(X + l->hw),
                        (int16_t)(Y + l->hh) };
        ch_restore(&g->fb, &g->bg, &q);
    }
    ch_dirty_add(&g->d_cur, X, Y, l->hw, l->hh);
    g->hud_sucio = 0;
}

/* A tap on the HUD, in any mode that shows it. */
static void hud_toque(ch_t *g, int bx, int by)
{
    switch (hud_boton_en(bx, by)) {
    case HB_MENU:
        if (g->modo == MODO_MAPA) {
            ch_ui_menu(g);
        } else if (g->modo != MODO_FERIA && g->modo != MODO_CABINA &&
                   g->modo != MODO_DIALOGO) {
            /* From any menu the same button goes back to the map. */
            ch_sfx(700, 30);
            g->sel = g->sel2 = g->scroll = 0;
            g->modo = MODO_MAPA;
            g->rehacer_fondo = 1;
        }
        break;
    case HB_MENOS:
        if (g->modo == MODO_MAPA) g->zoom_pide = -1;
        break;
    case HB_MAS:
        if (g->modo == MODO_MAPA) g->zoom_pide = +1;
        break;
    default:
        break;
    }
}

static bool en_hud(int bx, int by)
{
    const ch_lay_t *l = &ch_lay;
    return bx >= l->hx && bx < l->hx + l->hw && by >= l->hy && by < l->hy + l->hh;
}

void ch_ui_aviso(ch_t *g, const char *texto)
{
    snprintf(g->aviso, sizeof(g->aviso), "%s", texto);
    g->aviso_t = 45;
    g->hud_sucio = 1;
}

/* --------------------------------------------------------------------------
 * The dialogue panel
 *
 * Across the bottom of the map, 27 characters wide - every line in
 * ch_zonas.c was cut to that by hand - and five lines a page standing up,
 * four lying down. The map's window stops above it (chatarra.c), so the
 * robot is never drawn over the words.
 * -------------------------------------------------------------------------- */

#define DLG_X   (ch_lay.dx)
#define DLG_Y   (ch_lay.dy)
#define DLG_W   (ch_lay.dw)
#define DLG_H   (ch_lay.dh)
#define DLG_N   (ch_lay.dlg_lineas)

static char s_lineas[24][30];
static int  s_nlineas;

void ch_ui_dialogo(ch_t *g, const char *texto, int ent, int luego)
{
    g->dlg = texto;
    g->dlg_ent = (uint8_t)ent;
    g->dlg_luego = (uint8_t)luego;
    g->dlg_pag = 0;
    g->dlg_chars = 0;
    /* 27 and not 28: the box is 176 units, minus 6 of margin each side is
     * 164, and at 6 per character 27 fit. With 28 the last letter runs off. */
    s_nlineas = ch_wrap(texto, 27, s_lineas, 24);
    g->dlg_pags = (uint8_t)((s_nlineas + DLG_N - 1) / DLG_N);
    if (!g->dlg_pags) g->dlg_pags = 1;
    g->modo = MODO_DIALOGO;
    g->rehacer_fondo = 1;
}

/* The panel's BACKGROUND carries only the box: the text is drawn per frame,
 * because it appears letter by letter. Leaving it in the background would
 * force a rebuild thirty times a second. Under it and around it, the strip
 * the map's window does not cover. */
static void dlg_fondo(ch_t *g)
{
    /* The pages are counted again for the panel it is drawn in: turning the
     * screen mid-dialogue changes five lines a page into four. */
    g->dlg_pags = (uint8_t)((s_nlineas + DLG_N - 1) / DLG_N);
    if (!g->dlg_pags) g->dlg_pags = 1;
    if (g->dlg_pag >= g->dlg_pags) g->dlg_pag = (uint8_t)(g->dlg_pags - 1);
    ch_rect(&g->bg, 0, DLG_Y, SW, SH - DLG_Y, ch_rgb(0x07090F));
    ch_hline(&g->bg, 0, DLG_Y, SW, ch_rgb(0x1A2133));
    ch_panel(&g->bg, DLG_X, DLG_Y + 1, DLG_W, DLG_H - 1, ch_rgb(0x8A93AB));
}

/* How many letters the whole page has, to know when it has finished. */
static int dlg_largo(ch_t *g)
{
    int n = 0;
    for (int i = 0; i < DLG_N; i++) {
        int l = g->dlg_pag * DLG_N + i;
        if (l >= s_nlineas) break;
        n += (int)strlen(s_lineas[l]);
    }
    return n;
}

/* The text, revealing itself. It is drawn onto what is on screen and records
 * its rectangle.
 *
 * It is worth it for a reason that is not decorative: text that appears all at
 * once is read at a glance and you tap without reading. Appearing letter by
 * letter the eye follows it, and besides, the tap that speeds it up is the
 * natural way of saying "I have read it". */
static void dlg_texto(ch_t *g)
{
    ch_buf_t *b = &g->fb;
    int quedan = g->dlg_chars;

    for (int i = 0; i < DLG_N; i++) {
        int l = g->dlg_pag * DLG_N + i;
        int n;
        char corte[30];

        if (l >= s_nlineas) break;
        n = (int)strlen(s_lineas[l]);
        if (quedan <= 0) break;
        if (n > quedan) n = quedan;
        memcpy(corte, s_lineas[l], (size_t)n);
        corte[n] = 0;
        quedan -= (int)strlen(s_lineas[l]);
        ch_text_sh(b, DLG_X + 6, DLG_Y + 7 + i * 12, corte, ch_rgb(0xFFFFFF),
                   ch_rgb(0x05060C));
    }

    /* The little arrow only once the page has finished writing itself: while
     * it writes, there is nothing to confirm yet. */
    if (g->dlg_chars >= dlg_largo(g) && ((g->cuadro >> 3) & 1)) {
        int ax = DLG_X + DLG_W - 11, ay = DLG_Y + DLG_H - 9;
        ch_rect(b, ax,     ay,     5, 2, ch_rgb(0xFFE45E));
        ch_rect(b, ax + 1, ay + 2, 3, 2, ch_rgb(0xFFE45E));
        ch_rect(b, ax + 2, ay + 4, 1, 2, ch_rgb(0xFFE45E));
    }
    ch_dirty_add(&g->d_cur, DLG_X + 4, DLG_Y + 4, DLG_W - 8, DLG_H - 5);
}

static void dlg_avanzar(ch_t *g)
{
    /* The first tap finishes writing the page; the second moves on. It is the
     * genre's convention and it stops an impatient tap eating a text that has
     * not appeared yet. */
    if (g->dlg_chars < dlg_largo(g)) {
        g->dlg_chars = 250;
        return;
    }
    g->dlg_pag++;
    g->dlg_chars = 0;
    if (g->dlg_pag < g->dlg_pags) {
        g->rehacer_fondo = 1;
        return;
    }
    g->modo = g->dlg_luego;
    g->rehacer_fondo = 1;
    /* The boss's dialogue ends in a fight: it is fired on CLOSING it and not
     * on opening it, so it is read before the arena appears. */
    ch_map_dialogo_cerrado(g);
}

/* --------------------------------------------------------------------------
 * The menu: two pages of tiles with an icon
 *
 * It used to be nine rows of 16 px stacked in one column. On a 1.8" screen
 * that is a 172x14 strip per entry and the whole list crossed the touch
 * panel's envelope from end to end: the first row and the last one were the
 * two worst places on the glass. Now it is two pages -what you carry, and
 * what the game is- of tiles at least 40 px tall, all of them between y=36
 * and y=168 of the buffer, which is real 72..336: the middle of the panel.
 *
 * The icons are 12x12 written as text. Two layers -body and detail- because
 * one flat colour at this size reads as a blob, and at x2 (or x3 on the root
 * page) a 12x12 grid is exactly the resolution the rest of the game draws at.
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *fila[12];
    uint32_t    cuerpo, detalle;
} icono_t;


/* Which icon each item wears. The two oils share one and so do the two
 * batteries -they ARE the same thing, bigger- and the errands that are one
 * object each share the toolbox. */
static const uint8_t ICONO_ITEM[ITEMS] = {
    [IT_ACEITE] = IC_ACEITE,    [IT_ACEITE2]  = IC_BARRIL,
    [IT_BATERIA] = IC_BATERIA,  [IT_BATERIA2] = IC_BATERIA,
    [IT_SOLDADOR] = IC_SOLDADOR,[IT_CHIP]     = IC_CHIP,
    [IT_IMAN] = IC_IMAN,        [IT_LLAVE]    = IC_LLAVE,
    [IT_PASE] = IC_PASE,        [IT_TORNILLOS]= IC_TORNILLOS,
    [IT_ANCLA] = IC_ANCLA,      [IT_FUSIBLE]  = IC_HERRAMIENTA,
    [IT_MOLDE] = IC_HERRAMIENTA,[IT_TERMO]    = IC_HERRAMIENTA,
    [IT_CLAVE] = IC_LLAVE,      [IT_ENGRANAJE]= IC_HERRAMIENTA,
    [IT_REPELENTE] = IC_ACEITE, [IT_ORUGAS]   = IC_TRUEQUE,
    [IT_BOLSA] = IC_OBJETOS,
};

static const icono_t ICONOS[NICONOS] = {
    [IC_TALLER] = { {              /* a nut: the town is called Villa Tuerca */
        "....####....", "..########..", ".##########.", "###......###",
        "##...++...##", "##..++++..##", "##..++++..##", "##...++...##",
        "###......###", ".##########.", "..########..", "....####....",
    }, 0xC8CEDC, 0x5A6076 },
    [IC_OBJETOS] = { {                                          /* a bag    */
        "...##..##...", "...##..##...", "..########..", ".##########.",
        "############", "############", "##..++++..##", "##..++++..##",
        "############", ".##########.", "..########..", "............",
    }, 0xB97A3E, 0xFFE45E },
    [IC_EQUIPO] = { {           /* three rows of a roster: portrait and bar */
        "............", ".###.#######", ".#+#.#######", ".###........",
        "............", ".###.#######", ".#+#.#######", ".###........",
        "............", ".###.#######", ".#+#.#######", ".###........",
    }, 0x8FA6C4, 0x6FE3FF },
    [IC_REGISTRO] = { {                               /* a page with lines  */
        ".##########.", ".#........#.", ".#.++++++.#.", ".#........#.",
        ".#.++++++.#.", ".#........#.", ".#.++++++.#.", ".#........#.",
        ".#.++++...#.", ".#........#.", ".##########.", "............",
    }, 0xE8E2D0, 0x8A93AB },
    [IC_MAPA] = { {                                      /* a sheet + pin   */
        "############", "#..........#", "#...####...#", "#..##++##..#",
        "#..##++##..#", "#...####...#", "#....##....#", "#....##....#",
        "#..........#", "#..........#", "############", "............",
    }, 0x6FBF73, 0xFF5E5E },
    [IC_AYUDA] = { {                                     /* a question mark */
        "............", "...######...", "..##++++##..", "..##....##..",
        "........##..", ".......##...", ".....###....", ".....##.....",
        ".....##.....", "............", ".....##.....", "............",
    }, 0xFFE45E, 0xB99A2E },
    [IC_SONIDO] = { {                                        /* a speaker   */
        "............", "......##....", ".....###..+.", "...#####.+..",
        "..######.+.+", "..######+.+.", "..######.+.+", "...#####.+..",
        ".....###..+.", "......##....", "............", "............",
    }, 0xD5DCEB, 0x6FE3FF },
    [IC_GUARDAR] = { {                                       /* a floppy    */
        "############", "#++++++++++#", "#+##....##+#", "#+##....##+#",
        "#+########+#", "#++++++++++#", "#+########+#", "#+#......#+#",
        "#+#......#+#", "#+########+#", "############", "............",
    }, 0x3D465F, 0xD5DCEB },
    [IC_CERRAR] = { {                              /* the way out of a room */
        ".####.......", ".#..........", ".#..........", ".#....##....",
        ".#...+##....", ".#..++######", ".#...+##....", ".#....##....",
        ".#..........", ".#..........", ".####.......", "............",
    }, 0x8A93AB, 0xFFE45E },
    [IC_MOCHILA] = { {                          /* root: your robot and you */
        ".....##.....", ".....##.....", ".##########.", "##........##",
        "#..######..#", "#..#++++#..#", "#..#++++#..#", "#..######..#",
        "##........##", ".##########.", "..##....##..", "..##....##..",
    }, 0x8FA6C4, 0x6FE3FF },

    [IC_ACEITE] = { {                                 /* aceitera con pico y asa */
        "............", ".......kk...", "......koK...", ".....koK....",
        "..kkkkkkk...", ".kwoooooOk..", "koyoooooOk..", "koyoooooOkk.",
        "koyoooooOk.k", ".kOOOOOOOk.k", "..kkkkkkk.kk", "............",
    }, 0xFF9F0A, 0xC05A00 },
    [IC_BATERIA] = { {                                /* bateria con el rayo en el cuerpo */
        "...kk..kk...", "..kGGkkGGk..", ".kkkkkkkkkk.", ".kvVVVVVVvk.",
        ".kvVVwwVVvk.", ".kvVVwVVVvk.", ".kvVwwwwVvk.", ".kvVVVwVVvk.",
        ".kvVVwwVVvk.", ".kvVVVVVVvk.", ".kkkkkkkkkk.", "............",
    }, 0x4ADE80, 0x1E7A3C },
    [IC_SOLDADOR] = { {                               /* soldador con la punta al rojo */
        "..........kk", ".........krk", "........krrk", ".......kroK.",
        "......kroK..", ".....kroK...", "....kGGk....", "...kdGk.....",
        "..kjJk......", ".kjJk.......", "kjJk........", "kkk.........",
    }, 0x99A3BC, 0xFF9F0A },
    [IC_CHIP] = { {                                   /* chip con el dado adentro */
        "..k.k.k.k...", ".kkkkkkkkk..", "kkCCCCCCCCkk", ".kCcccccccCk",
        ".kCcKKKKKcCk", ".kCcKwwwKcCk", ".kCcKwCwKcCk", ".kCcKKKKKcCk",
        ".kCcccccccCk", "kkCCCCCCCCkk", ".kkkkkkkkk..", "..k.k.k.k...",
    }, 0x7BE9FF, 0x18A6D8 },
    [IC_IMAN] = { {                                   /* iman de herradura con los polos */
        ".kkkk..kkkk.", "kmMMmkkmMMmk", "kmMMmkkmMMmk", "kmMMmkkmMMmk",
        "kmMMmkkmMMmk", "kmMMmkkmMMmk", "kmMMmkkmMMmk", "kmMMMMMMMMmk",
        "kwwwwkkGGGGk", "kwwwwkkGGGGk", "kkkkk..kkkkk", "............",
    }, 0xFF6FAE, 0xD5DCEB },
    [IC_LLAVE] = { {                                  /* llave con paleton */
        "...kkkk.....", "..kyYYyk....", ".kyYkkYyk...", ".kyk..kyk...",
        ".kyYkkYyk...", "..kyYYyk....", "...kyYk.....", "...kyYk.....",
        "...kyYkk....", "...kyYk.....", "...kyYkkk...", "...kkkk.....",
    }, 0xFFE45E, 0xE0A800 },
    [IC_PASE] = { {                                   /* pase con foto y banda */
        "kkkkkkkkkkkk", "kCCCCCCCCCCk", "kCkwwkCCCCCk", "kCkwwkCGGGCk",
        "kCkwwkCGGGCk", "kCkkkkCCCCCk", "kCGGGGGGGGCk", "kCCCCCCCCCCk",
        "kCyyyyyyyyCk", "kCyKKKKKKyCk", "kCCCCCCCCCCk", "kkkkkkkkkkkk",
    }, 0x18A6D8, 0xD5DCEB },
    [IC_TORNILLOS] = { {                              /* caja con los tornillos a la vista */
        ".kkkkkkkkkk.", ".kJJJJJJJJk.", ".kjjjjjjjjk.", ".kkkkkkkkkk.",
        "kGdGkGdGkGdk", "kdddkdddkddd", "kGdGkGdGkGdk", "kdddkdddkddd",
        "kGdGkGdGkGdk", ".kjjjjjjjjk.", ".kJJJJJJJJk.", ".kkkkkkkkkk.",
    }, 0x99A3BC, 0x3D465F },
    [IC_ANCLA] = { {                                  /* ancla con cepo y brazos */
        ".....kk.....", "....kuuk....", "....kkkk....", "....kuuk....",
        ".kkkkuukkkk.", ".kUUkuukUUk.", "....kuuk....", "k...kuuk...k",
        "ku..kuuk..uk", "kuukkuukkuuk", ".kuUUUUUUuk.", "..kkkkkkkk..",
    }, 0xC06B2E, 0x6E3A16 },
    [IC_HERRAMIENTA] = { {                            /* caja de herramientas con asa y traba */
        ".....kk.....", "....kGGk....", "..kkkkkkkk..", ".kGGGGGGGGk.",
        "kGdddddddddk", "kGdyyyyyyydk", "kGdyKKKKKydk", "kGdyyyyyyydk",
        "kGdddddddddk", ".kGGGGGGGGk.", "..kkkkkkkk..", "............",
    }, 0xD5DCEB, 0xFFE45E },
    [IC_BARRIL] = { {                                 /* barril de aceite con zunchos */
        "..kkkkkkkk..", ".kOOOOOOOOk.", "kOoooooooOk.", "kOoOOOOOoOk.",
        "kkkkkkkkkkkk", "kOoyyyyyoOk.", "kOoyOOOyoOk.", "kOoyyyyyoOk.",
        "kkkkkkkkkkkk", "kOoooooooOk.", ".kOOOOOOOOk.", "..kkkkkkkk..",
    }, 0xFF9F0A, 0xC05A00 },
    [IC_COMBATE] = { {                              /* dos cunas que chocan */
        "kk........kk", "kGk......kGk", "kGGk....kGGk", "kGGGk..kGGGk",
        "kGGGGkkGGGGk", "kGGGkrrkGGGk", "kGGGkrrkGGGk", "kGGGGkkGGGGk",
        "kGGGk..kGGGk", "kGGk....kGGk", "kGk......kGk", "kk........kk",
    }, 0xD5DCEB, 0xFF4A3D },
    [IC_TRUEQUE] = { {                              /* una flecha para cada lado */
        "....v.......", "...vvv......", "..vvvvv.....", "....v.......",
        "....v.......", "....v..n....", "....v..n....", "....v..n....",
        "....v..n....", ".......n....", "....nnnnn...", ".....nnn....",
    }, 0x4ADE80, 0x2AF0C8 },
    [IC_PIEZA] = { {                                /* una cabeza suelta */
        "............", "..kkkkkkkk..", ".kGGGGGGGGk.", "kGGkGGGGkGGk",
        "kGGkGGGGkGGk", "kGGGGGGGGGGk", "kGGccGGccGGk", "kGGccGGccGGk",
        "kGGGGGGGGGGk", ".kGGGGGGGGk.", "..kkkkkkkk..", "............",
    }, 0x8FA6C4, 0x7BE9FF },
    [IC_COLGAR] = { {                               /* el tubo y la flecha abajo */
        "kkk......kkk", "kBBk....kBBk", "kBBk....kBBk", "kBBkkkkkkBBk",
        "kBBBBBBBBBBk", "kkkkkkkkkkkk", "............", "....rrrr....",
        "....rrrr....", "..rrrrrrrr..", "...rrrrrr...", "....rrrr....",
    }, 0x4A9DF5, 0xFF4A3D },
    [IC_DIARIO] = { {                           /* una libreta con un lazo */
        ".kkkkkkkkkk.", ".kJjjjjjjjJk", "kykjjjjjjjJk", "kykjjyyyyjJk",
        "kykjjjjjjjJk", "kykjjyyyyjJk", "kykjjjjjjjJk", "kykjjyyjjjJk",
        "kykjjjjjjjJk", ".kJjjjjjjjJk", ".kkkkkkkkkk.", "............",
    }, 0x8A5A32, 0xFFE45E },
    [IC_AJUSTES] = { {                                /* root: the settings */
        "............", ".##########.", ".....##.....", ".....##.....",
        ".##########.", "...##.......", "...##.......", ".##########.",
        "........##..", "........##..", ".##########.", "............",
    }, 0xD5DCEB, 0xFFE45E },
};

/* THE 2x ICONS (ASSETS.md): a 24x24 drawing per icon, generated by
 * tools/arte2x_iconos.py. Each art pixel is 'esc' canvas pixels -half a UI
 * unit times esc- so the drawing covers the very 12*esc units the 1x icon
 * covered and nothing around it moves. The shading is in the drawing: no
 * upscaler, no bevel. An icon without one still goes the old way, through
 * the upscaler, below. */
#include "ch_ui2x.inc"

/* TWO COLOURS WAS THE CEILING, and it showed: an item icon came out as a
 * coloured blob with a hole in it. '#' and '+' still mean body and detail -the
 * menu's eleven icons are drawn that way and read fine at three times size-
 * but any other letter is now looked up in the SPRITES' palette, so an icon
 * can have as much detail as the rest of the game's art. Same table, same
 * twelve by twelve, no new format to learn. */
void ch_ui_icono(ch_buf_t *b, int x, int y, int ic, int esc)
{
    const icono_t *o = &ICONOS[ic % NICONOS];
    uint16_t c = ch_rgb(o->cuerpo), d = ch_rgb(o->detalle), p;
    uint16_t px[12 * 12];

    if (ICONOS_HD[ic % NICONOS]) {
        ch_blit2m(b, x, y, ICONOS_HD[ic % NICONOS], 24, esc);
        return;
    }

    /* Into pixels, and through the same upscaler as the world's sprites: an
     * icon at 3 units a pixel is 12 canvas pixels a pixel, and without the
     * corners and the bevel it is a mosaic. */
    for (int fy = 0; fy < 12; fy++) {
        const char *f = o->fila[fy];
        int fx = 0;
        for (; f && fx < 12 && f[fx]; fx++) {
            uint16_t col;
            if (f[fx] == '.')      col = 0;
            else if (f[fx] == '#') col = c;
            else if (f[fx] == '+') col = d;
            else if (ch_pal(f[fx], &p)) col = p;
            else                   col = c;
            px[fy * 12 + fx] = col;
        }
        for (; fx < 12; fx++) px[fy * 12 + fx] = 0;
    }
    ch_blit_px(b, x, y, px, 12, 12, esc, CH_HD_BISEL);
}


/* --------------------------------------------------------------------------
 * Lists
 *
 * A list is a column of rows from `y0` down, as many as fit in the screen
 * area: standing up that is five tall item rows where the watch had three,
 * lying down it is three wider ones. The geometry is set by each screen
 * before drawing it, and the same numbers answer the touch.
 * -------------------------------------------------------------------------- */

static int s_lx = 6, s_lw = 172;
static int s_ly0 = 64, s_lfh = 18, s_lfilas = 6;
#define LX  s_lx
#define LW  s_lw

/* The foot of the screen area: lying down the screen area reaches the bottom
 * of the glass, where the home swipe starts. */
static int pie_util(void)
{
    return SH - (HORIZ ? ch_lay.pie : 2);
}

/* 'filas' is the most the screen wants; it gets as many as fit. */
static void lista_geom(int y0, int fh, int filas)
{
    int caben = (pie_util() - y0) / fh;
    if (filas > caben) filas = caben;
    if (filas < 1) filas = 1;
    s_lx = 6;
    s_lw = SW - 12;
    s_ly0 = y0; s_lfh = fh; s_lfilas = filas;
}

static int fila_en(int bx, int by)
{
    if (bx < LX || bx >= LX + LW) return -1;
    if (by < s_ly0 || by >= s_ly0 + s_lfilas * s_lfh) return -1;
    return (by - s_ly0) / s_lfh;
}

/* THE TALL ROW, the one an item lives in: the icon, the name, and the
 * description wrapped underneath, which is the thing you actually needed in
 * order to choose. */
static void fila_item(ch_t *g, int i, int item, const char *der,
                      uint16_t cder, bool activa)
{
    ch_buf_t *b = &g->bg;
    int y = s_ly0 + i * s_lfh;
    int h = s_lfh - 4;
    const ch_item_t *it = &ch_items[item % ITEMS];
    char lin[2][30];
    int n;

    ch_caja(b, LX, y, LW, h, activa ? ch_rgb(0x1A2133) : ch_rgb(0x141720),
            ch_rgb(0x3D465F));
    ch_rect(b, LX + 4, y + h / 2 - 14, 28, 28, ch_rgb(0x0E111A));
    ch_ui_icono(b, LX + 6, y + h / 2 - 12, ICONO_ITEM[item % ITEMS], 2);

    ch_text(b, LX + 38, y + 6, _(it->nombre),
            activa ? ch_rgb(0xFFFFFF) : ch_rgb(0x606B85));
    n = ch_wrap(_(it->desc), (LW - 44) / CH_FADV, lin, 2);
    for (int k = 0; k < n && k < 2; k++) {
        ch_text(b, LX + 38, y + 18 + k * 10, lin[k],
                activa ? ch_rgb(0x8A93AB) : ch_rgb(0x4A5268));
    }
    if (der && *der) {
        ch_text(b, LX + LW - 6 - ch_text_w(der), y + 6, der,
                activa ? cder : ch_rgb(0x606B85));
    }
}

/* --------------------------------------------------------------------------
 * The header every screen wears
 *
 * A back button on the left - the system's back swipe does the same, but a
 * swipe is not something a screen shows you - the title beside it, and the
 * subtitle under it. Standing up the header is 40 tall and the subtitle has a
 * line of its own; lying down it is 30 and the subtitle goes under the title.
 * The list arrows, when there are any, are at the right of the title.
 * -------------------------------------------------------------------------- */

#define HDR     (HORIZ ? 30 : 40)
#define TIT_X   30                      /* the title, after the back button  */
#define VU_X     4                      /* the back button                   */
#define VU_Y     4
#define VU_W    22
#define VU_H    22

static void boton_volver(ch_buf_t *b)
{
    ch_caja(b, VU_X, VU_Y, VU_W, VU_H, ch_rgb(0x232B41), ch_rgb(0x606B85));
    /* a chevron pointing left, at the canvas's resolution */
    {
        ch_buf_t nb = ch_nativo(b);
        int k = b->s;
        int cx = ch_fx(b, VU_X + VU_W / 2) - k, cy = ch_fy(b, VU_Y + VU_H / 2);
        for (int i = 0; i <= 5 * k; i++) {
            ch_rect(&nb, cx - 2 * k + i, cy - i - k / 2, 2 * k, k, ch_rgb(0xFFFFFF));
            ch_rect(&nb, cx - 2 * k + i, cy + i - k / 2, 2 * k, k, ch_rgb(0xFFFFFF));
        }
    }
}

static bool volver_en(int bx, int by)
{
    return bx >= 0 && bx < VU_X + VU_W + 6 && by >= 0 && by < VU_Y + VU_H + 6;
}

/* The screen area, cleared, with the header: the background every menu
 * screen starts from. */
static void pantalla(ch_buf_t *b)
{
    ch_rect(b, 0, 0, SW, SH, ch_rgb(0x0B0D14));
    ch_vgrad(b, 0, 0, SW, HDR + 8, ch_rgb(0x1B2340), ch_rgb(0x0B0D14));
    /* a faint grid of rivets down the plate, so a long list does not float
     * in a black void */
    for (int y = HDR + 14; y < SH - 4; y += 24) {
        for (int x = 3; x < SW; x += (SW - 6) / 2) {
            ch_rect(b, x, y, 1, 1, ch_rgb(0x1A1F2E));
        }
    }
}

void ch_ui_titulo(ch_t *g, const char *txt, const char *sub)
{
    ch_buf_t *b = &g->bg;

    pantalla(b);
    boton_volver(b);
    ch_text_sh(b, TIT_X, HORIZ ? 6 : 8, txt, ch_rgb(0xFFE45E), ch_rgb(0x05060C));
    if (sub) {
        if (HORIZ) ch_text(b, TIT_X, 17, sub, ch_rgb(0x8A93AB));
        else       ch_text(b, 8, 29, sub, ch_rgb(0x8A93AB));
    }
    ch_hline(b, 6, HDR - 1, SW - 12, ch_rgb(0x3D465F));
}

/* The list's arrows, at the right of the title. Both are touchable. */
#define AR_W     20
#define AR_H     22
#define AR_X    (SW - 2 * AR_W - 8)
#define AR_Y      4

static void flechas(ch_t *g, bool arriba, bool abajo)
{
    ch_buf_t *b = &g->bg;
    ch_buf_t nb = ch_nativo(b);
    int k = b->s;

    if (!arriba && !abajo) return;          /* everything fits: no arrows */
    ch_caja(b, AR_X, AR_Y, AR_W, AR_H, ch_rgb(0x232B41), ch_rgb(0x3D465F));
    ch_caja(b, AR_X + AR_W + 4, AR_Y, AR_W, AR_H, ch_rgb(0x232B41),
            ch_rgb(0x3D465F));

    /* The tip is marked by the NARROWEST row, drawn at the canvas's
     * resolution so the triangles have straight sides. */
    uint16_t ca = arriba ? ch_rgb(0xFFFFFF) : ch_rgb(0x3D465F);
    uint16_t cb = abajo  ? ch_rgb(0xFFFFFF) : ch_rgb(0x3D465F);
    int ax = ch_fx(b, AR_X + AR_W / 2), bx = ch_fx(b, AR_X + AR_W + 4 + AR_W / 2);
    int ay = ch_fy(b, AR_Y + 7), by = ch_fy(b, AR_Y + AR_H - 7);
    for (int i = 0; i < 4 * k; i++) {
        ch_rect(&nb, ax - i, ay + i, 1 + i * 2, 1, ca);       /* up   */
        ch_rect(&nb, bx - i, by - i, 1 + i * 2, 1, cb);       /* down */
    }
}

static int flecha_en(int bx, int by)
{
    if (by < AR_Y - 2 || by >= AR_Y + AR_H + 4) return 0;
    if (bx >= AR_X - 2 && bx < AR_X + AR_W + 2) return -1;
    if (bx >= AR_X + AR_W + 2 && bx < AR_X + 2 * AR_W + 8) return 1;
    return 0;
}

/* The row of text in the middle of an empty list. */
static void vacio(ch_t *g, const char *txt)
{
    ch_text_center(&g->bg, SW / 2, HDR + (SH - HDR) / 2 - 4, txt,
                   ch_rgb(0x8A93AB), ch_rgb(0x05060C));
}

/* --------------------------------------------------------------------------
 * Main menu
 * -------------------------------------------------------------------------- */

/* What each tile of each page is. The root page has two, and everything the
 * player asked for by name -workshop, items, team, records, map- is on the
 * first one: those are the five you open while playing, and the other four
 * are the ones you open once. */
typedef struct { uint8_t icono; const char *txt; uint8_t accion; } baldosa_t;

enum { AC_TALLER = 1, AC_OBJETOS, AC_EQUIPO, AC_REGISTRO, AC_MAPA,
       AC_DIARIO, AC_AYUDA, AC_SONIDO, AC_DIFICULTAD, AC_GUARDAR,
       AC_CERRAR,
       AC_PAG1, AC_PAG2 };

static const baldosa_t PAG_RAIZ[] = {
    { IC_MOCHILA, N_("LO TUYO"),   AC_PAG1 },
    { IC_AJUSTES, N_("EL JUEGO"),  AC_PAG2 },
};
static const baldosa_t PAG_TUYO[] = {
    { IC_TALLER,   N_("TALLER"),   AC_TALLER },
    { IC_OBJETOS,  N_("OBJETOS"),  AC_OBJETOS },
    { IC_EQUIPO,   N_("EQUIPO"),   AC_EQUIPO },
    { IC_REGISTRO, N_("REGISTRO"), AC_REGISTRO },
    { IC_MAPA,     N_("MAPA"),     AC_MAPA },
    { IC_DIARIO,   N_("DIARIO"),   AC_DIARIO },
};
static const baldosa_t PAG_JUEGO[] = {
    { IC_AYUDA,    N_("AYUDA"),      AC_AYUDA },
    { IC_SONIDO,   N_("SONIDO"),     AC_SONIDO },
    { IC_COMBATE,  N_("DIFICULTAD"), AC_DIFICULTAD },
    { IC_GUARDAR,  N_("GUARDAR"),    AC_GUARDAR },
    { IC_CERRAR,   N_("CERRAR"),     AC_CERRAR },
};

static const char *const DIFICULTADES_N[DIFICULTADES] = {
    N_("NORMAL"), N_("FACIL"), N_("DURO"),
};

static const char *const SONIDOS[3] = { N_("MUDO"), N_("EFECTOS"), N_("TODO") };


/* The grid. Everything comes out of these numbers so that moving the grid is
 * moving one line and not nine: the tiles fill the screen area under the
 * header, three to a row lying down and two standing up. */
#define BX0     8
#define BY0     (HDR + 4)
#define BANCHO  (SW - BX0 * 2)
#define BALTO   (pie_util() - BY0)

static const baldosa_t *pagina(const ch_t *g, int *n, int *cols, const char **tit)
{
    switch (g->sel2) {
    case 1: *n = (int)(sizeof(PAG_TUYO)  / sizeof(PAG_TUYO[0]));
            *cols = HORIZ ? 3 : 2; *tit = N_("LO TUYO");  return PAG_TUYO;
    case 2: *n = (int)(sizeof(PAG_JUEGO) / sizeof(PAG_JUEGO[0]));
            *cols = HORIZ ? 3 : 2; *tit = N_("EL JUEGO"); return PAG_JUEGO;
    default: *n = (int)(sizeof(PAG_RAIZ) / sizeof(PAG_RAIZ[0]));
            *cols = HORIZ ? 2 : 1; *tit = N_("MENU");     return PAG_RAIZ;
    }
}

static void baldosa_caja(int i, int n, int cols, int *x, int *y, int *w, int *h)
{
    int filas = (n + cols - 1) / cols;
    int gap = 6;

    *w = (BANCHO - gap * (cols - 1)) / cols;
    *h = (BALTO - gap * (filas - 1)) / filas;
    /* a tile taller than it is wide reads as a column, not as a button */
    if (*h > *w + 20) *h = *w + 20;
    *x = BX0 + (i % cols) * (*w + gap);
    *y = BY0 + (i / cols) * (*h + gap);
}

static int baldosa_en(const ch_t *g, int bx, int by)
{
    int n, cols;
    const char *tit;

    (void)pagina(g, &n, &cols, &tit);
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        baldosa_caja(i, n, cols, &x, &y, &w, &h);
        if (bx >= x && bx < x + w && by >= y && by < y + h) return i;
    }
    return -1;
}

static void menu_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    int n, cols;
    const char *tit;
    const baldosa_t *p = pagina(g, &n, &cols, &tit);

    ch_ui_titulo(g, _(tit), g->sel2 ? _("DESLIZA PARA VOLVER") : NULL);

    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        const char *txt = _(p[i].txt);
        const char *sub = p[i].accion == AC_SONIDO
                        ? _(SONIDOS[ch_sonido_get() % 3])
                        : (p[i].accion == AC_DIFICULTAD
                           ? _(DIFICULTADES_N[ch_dificultad(&g->s) % DIFICULTADES])
                           : NULL);

        baldosa_caja(i, n, cols, &x, &y, &w, &h);
        ch_caja(b, x, y, w, h, ch_rgb(0x1A2133), ch_rgb(0x3D465F));

        if (w >= 150) {                 /* wide: the icon to the left of the text */
            int e = h >= 70 ? 4 : 3;
            int tx = x + 26 + 12 * e;
            /* the label at twice the size when it fits beside the icon */
            int m = ch_text_w(txt) * 2 <= x + w - 6 - tx ? 2 : 1;
            ch_rect(b, x + 10, y + h / 2 - 6 * e - 4, 12 * e + 8, 12 * e + 8,
                    ch_rgb(0x0E111A));
            ch_ui_icono(b, x + 14, y + h / 2 - 6 * e, p[i].icono, e);
            ch_text_big(b, tx, y + h / 2 - 7 * m / 2, txt,
                        ch_rgb(0xFFFFFF), ch_rgb(0x05060C), m);
        } else {                        /* narrow: the icon over the text     */
            /* Centred as ONE block -icon, label and, if there is one, the
             * value- so nothing anchored to the edges overlaps in the
             * middle of a short tile. */
            int e = h >= 64 ? 3 : 2;
            int alto = 12 * e + 6 + 7 + (sub ? 9 : 0);
            int iy = y + (h - alto) / 2;

            ch_ui_icono(b, x + w / 2 - 6 * e, iy, p[i].icono, e);
            ch_text_sh(b, x + (w - ch_text_w(txt)) / 2, iy + 12 * e + 6, txt,
                       ch_rgb(0xFFFFFF), ch_rgb(0x05060C));
            if (sub) {
                ch_text(b, x + (w - ch_text_w(sub)) / 2, iy + 12 * e + 15, sub,
                        ch_rgb(0xFFE45E));
            }
        }
    }
}

void ch_ui_menu(ch_t *g)
{
    g->modo_prev = g->modo;
    g->modo = MODO_MENU;
    g->sel = 0;
    g->sel2 = 0;                    /* always back to the root page */
    g->scroll = 0;
    g->rehacer_fondo = 1;
    ch_sfx(900, 25);
}

/* --------------------------------------------------------------------------
 * Workshop
 *
 * The part you touch is fitted and the one that was on goes to the bag. It is
 * a swap and not an "equip": that way the bag never overflows on a change and
 * a part is never lost by accident.
 * -------------------------------------------------------------------------- */

static const char *const CATS[P_CATS] = { N_("CAB"), N_("TOR"),
                                         N_("BRA"), N_("PIE") };

/* El indice filtrado que comparten el taller, la mochila y la tienda: dice
 * QUE se muestra en cada fila, no lo que se muestra. */
static uint8_t s_lista[ITEMS];
static int     s_nlista;

/* --------------------------------------------------------------------------
 * EL TALLER, EN DOS PARTES
 *
 * Una vista general -el robot lo mas grande que entra, y a la derecha las
 * cuatro categorias con lo que lleva puesto- y, al tocar una, la lista de lo
 * que tenes en la mochila DE ESA CATEGORIA, a pantalla completa. Elegis, se
 * monta y volves a ver el robot cambiado, que es la unica razon por la que uno
 * abre esta pantalla.
 *
 * sel2: 0 = la vista general, 1+cat = la lista de esa categoria.
 * sel:  0 = nada elegido, 1+fila = la fila tocada una vez (la segunda monta).
 *
 * P4OS: the right column and the robot are measured from the screen area.
 * Standing up the robot is drawn at scale 3 with room under it, and the four
 * category buttons are taller; lying down the column is wider.
 * -------------------------------------------------------------------------- */
static int TA_RX, TA_RY, TA_BX, TA_BW, TA_EX, TA_EW, TA_EY, TA_EH, TA_BY, TA_BH;

static void taller_geom(void)
{
    TA_RX = 6;
    TA_BX = HORIZ ? 100 : 90;
    TA_BW = SW - TA_BX - 6;
    TA_EY = HDR + 4;
    TA_EH = HORIZ ? 22 : 26;
    /* The three of the team: over the right column lying down, across the
     * whole width standing up, where the column is too narrow for a name. */
    TA_EX = HORIZ ? TA_BX : 6;
    TA_EW = HORIZ ? TA_BW : SW - 12;
    TA_BY = TA_EY + TA_EH + (HORIZ ? 4 : 8);
    TA_BH = HORIZ ? 27 : 42;
    TA_RY = HORIZ ? HDR + 6 : TA_BY + 6;
}

/* Cual de los robots del equipo se esta armando. Vive fuera de ch_t porque no
 * es estado del juego: al salir del taller no significa nada. */
static uint8_t s_ta_bot;

static int ta_bot_en(int bx, int by)
{
    int w = (TA_EW - 4) / EQUIPO;

    if (by < TA_EY || by >= TA_EY + TA_EH) return -1;
    for (int i = 0; i < EQUIPO; i++) {
        int x = TA_EX + i * (w + 2);
        if (bx >= x && bx < x + w) return i;
    }
    return -1;
}

static int ta_cat_en(int bx, int by)
{
    if (bx < TA_BX || bx >= TA_BX + TA_BW) return -1;
    for (int c = 0; c < P_CATS; c++) {
        int y = TA_BY + c * TA_BH;
        if (by >= y && by < y + TA_BH - 3) return c;
    }
    return -1;
}

/* El robot que se esta armando: el activo, o el de la reserva que elegiste. */
static ch_robot_t *ta_robot(ch_t *g)
{
    ch_robot_t *r = ch_eq(&g->s, s_ta_bot % EQUIPO);
    if (!r) { s_ta_bot = 0; r = &g->s.yo; }
    return r;
}

/* Las piezas de la mochila de una categoria, en s_lista, CON LA PUESTA
 * PRIMERA: la pregunta es "cual de todas", no "cual de las sueltas". */
#define TA_PUESTA  0xFE                 /* la marca de "esta es la que llevas" */

static void ta_juntar(ch_t *g, int cat)
{
    s_nlista = 0;
    s_lista[s_nlista++] = TA_PUESTA;
    for (int i = 0; i < ch_mochila(&g->s); i++) {
        if (g->s.piezas[i] == 0xFF) continue;
        if (PIEZA_CAT(g->s.piezas[i]) != cat) continue;
        s_lista[s_nlista++] = (uint8_t)i;
    }
}

/* Que pieza es una fila de la lista, sea de la mochila o la puesta. */
static uint8_t ta_pieza(ch_t *g, int cat, int fila)
{
    uint8_t e = s_lista[fila % (s_nlista ? s_nlista : 1)];
    return e == TA_PUESTA ? PIEZA_ID(cat, ta_robot(g)->pieza[cat])
                          : g->s.piezas[e];
}

/* The robot on a lit plinth: the same disc the combat puts under it. */
static void pedestal(ch_buf_t *b, int cx, int y_pie, int esc)
{
    ch_glow(b, cx, y_pie - 20 * esc, 18 * esc, ch_rgb(0x2A3350), 9);
    ch_ellipse(b, cx, y_pie + 1, 14 * esc, 3 * esc + 1, ch_rgb(0x10131E));
    ch_ellipse(b, cx, y_pie, 13 * esc, 3 * esc, ch_rgb(0x2B3350));
    ch_ellipse(b, cx, y_pie - 1, 11 * esc, 2 * esc, ch_rgb(0x3A4466));
}

/* The list of one category: its geometry, for the drawing and for the tap. */
static void ta_lista_geom(void)
{
    lista_geom(HDR + (HORIZ ? 24 : 26), 36, 16);
}

static void taller_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    char t[40];

    taller_geom();
    if (g->sel2 == 0) {
        /* --- la vista general ------------------------------------------- */
        ch_robot_t *r = ta_robot(g);
        int sueltas = 0;
        int ew = (TA_EW - 4) / EQUIPO;
        int esc = 3;
        int rcx = TA_RX + (TA_BX - TA_RX - 4) / 2;

        {
            int pct = 0;
            const char *j = ch_robot_juego_nombre(r, &pct);
            if (j) {
                snprintf(t, sizeof(t), _("%s  %s +%d%%"),
                         _(ch_tipo_nombre[r->tipo % TIPOS]), _(j), pct);
            } else {
                snprintf(t, sizeof(t), _("TIPO %s"),
                         _(ch_tipo_nombre[r->tipo % TIPOS]));
            }
            ch_ui_titulo(g, _("TALLER"), t);
        }
        pedestal(b, rcx, TA_RY + 40 * esc - 2, esc);
        ch_robot_draw(b, rcx, TA_RY, r, esc, false, 0, 0);

        /* LOS TRES DEL EQUIPO. El taller armaba SIEMPRE el robot que sale a
         * pelear, asi que las piezas del segundo y del tercero no se podian
         * tocar sin cambiar cual sale primero. Son tres botones. */
        for (int i = 0; i < EQUIPO; i++) {
            const ch_robot_t *q = ch_eq(&g->s, i);
            int x = TA_EX + i * (ew + 2);
            bool aqui = (i == s_ta_bot % EQUIPO);

            ch_caja(b, x, TA_EY, ew, TA_EH, ch_rgb(aqui ? 0x2B3145 : 0x141720),
                    ch_rgb(aqui ? 0xFFE45E : 0x3D465F));
            if (!q) {
                ch_text_center(b, x + ew / 2, TA_EY + TA_EH / 2 - 3, "-",
                               ch_rgb(0x3D465F), ch_rgb(0x05060C));
                continue;
            }
            snprintf(t, sizeof(t), "%.*s", (ew - 6) / CH_FADV, ch_robot_nombre(q));
            ch_text_center(b, x + ew / 2, TA_EY + TA_EH / 2 - 8, t,
                           ch_rgb(aqui ? 0xFFFFFF : 0x606B85), ch_rgb(0x05060C));
            snprintf(t, sizeof(t), _("N%d"), q->nivel);
            ch_text_center(b, x + ew / 2, TA_EY + TA_EH / 2 + 1, t,
                           ch_rgb(aqui ? 0xFFE45E : 0x606B85), ch_rgb(0x05060C));
        }

        for (int c = 0; c < P_CATS; c++) {
            const ch_part_t *p = &ch_partes[PIEZA_ID(c, r->pieza[c])];
            int y = TA_BY + c * TA_BH;
            int n = 0;

            for (int i = 0; i < ch_mochila(&g->s); i++) {
                if (g->s.piezas[i] != 0xFF &&
                    PIEZA_CAT(g->s.piezas[i]) == c) n++;
            }
            ch_caja(b, TA_BX, y, TA_BW, TA_BH - 3, ch_rgb(0x1A2133),
                    ch_rgb(n ? 0x8A93AB : 0x3D465F));
            ch_text(b, TA_BX + 5, y + 4, _(CATS[c]), ch_rgb(0x8A93AB));
            if (n) {
                snprintf(t, sizeof(t), "+%d", n);
                ch_text(b, TA_BX + TA_BW - 5 - ch_text_w(t), y + 4, t,
                        ch_rgb(0x4ADE80));
            }
            snprintf(t, sizeof(t), "%.*s", (TA_BW - 10) / CH_FADV, _(p->nombre));
            ch_text(b, TA_BX + 5, y + 14, t, ch_rgb(0xFFFFFF));
            /* Standing up the button is tall enough for the part's type,
             * which is what the set bonus is made of. */
            if (TA_BH >= 36) {
                ch_text(b, TA_BX + 5, y + 24, _(ch_tipo_nombre[p->tipo % TIPOS]),
                        ch_rgb(ch_tipo_color[p->tipo % TIPOS]));
            }
            sueltas += n;
        }
        /* Y si no hay NADA suelto, decirlo: cuatro botones que se abren a una
         * lista vacia son cuatro caminos a ninguna parte. */
        if (!sueltas) {
            int y = TA_RY + 40 * esc + 10;
            if (y > pie_util() - 20) y = pie_util() - 20;
            ch_text_center(b, rcx, y, _("GANA COMBATES"),
                           ch_rgb(0x606B85), ch_rgb(0x05060C));
            ch_text_center(b, rcx, y + 10, _("PARA PIEZAS"),
                           ch_rgb(0x606B85), ch_rgb(0x05060C));
        }
        return;
    }

    /* --- la lista de una categoria ------------------------------------- */
    {
        int cat = (g->sel2 - 1) % P_CATS;
        int yc = HDR + 2;               /* the comparison, two lines */

        ta_juntar(g, cat);
        ta_lista_geom();
        snprintf(t, sizeof(t), "%s  %s", _("TALLER"), _(CATS[cat]));
        ch_ui_titulo(g, t, HORIZ ? NULL : _("TOCA 2 VECES"));

        flechas(g, g->scroll > 0, g->scroll + s_lfilas < s_nlista);
        if (s_nlista == 1) {
            ch_text(b, 6, yc + 4, _("NO TENES OTRA DE ESTE TIPO"), ch_rgb(0x8A93AB));
        }

        /* Lo que cambiaria, si ya tocaste una fila una vez. */
        if (g->sel) {
            ch_robot_t *r = ta_robot(g);
            ch_robot_t prueba = *r;
            uint8_t id = ta_pieza(g, cat, g->sel - 1);
            char t2[72];

            prueba.pieza[cat] = PIEZA_VAR(id);
            ch_robot_stats(&prueba);
            ch_text(b, 6, yc, _(ch_partes[id].nombre), ch_rgb(0xFFE45E));
            ch_text(b, SW - 6 - ch_text_w(_("TOCA 2X")), yc, _("TOCA 2X"),
                    ch_rgb(0x8A93AB));
            snprintf(t2, sizeof(t2), "PV%d>%d A%d>%d D%d>%d V%d>%d",
                     r->vida_max, prueba.vida_max,
                     r->atk, prueba.atk, r->def, prueba.def,
                     r->vel, prueba.vel);
            ch_text(b, 6, yc + 10, t2, ch_rgb(0xD5DCEB));
            {   /* y en que quedaria el juego, que es media decision */
                int ja = ch_robot_juego(r), jb = ch_robot_juego(&prueba);
                if (ja != jb) {
                    snprintf(t, sizeof(t), _("JUEGO %d>%d"), ja, jb);
                    ch_text(b, SW - 6 - ch_text_w(t), yc + 10, t,
                            jb > ja ? ch_rgb(0x4ADE80) : ch_rgb(0xFF4A3D));
                }
            }
        }

        for (int i = 0; i < s_lfilas; i++) {
            int k = g->scroll + i;
            int y = s_ly0 + i * s_lfh, h = s_lfh - 4;
            uint8_t id;
            const ch_part_t *p, *puesta;

            if (k >= s_nlista) break;
            id = ta_pieza(g, cat, k);
            p = &ch_partes[id];
            puesta = &ch_partes[PIEZA_ID(cat, ta_robot(g)->pieza[cat])];
            bool esta = (s_lista[k] == TA_PUESTA);

            ch_caja(b, LX, y, LW, h,
                    ch_rgb(g->sel == k + 1 ? 0x2A3350
                                           : (esta ? 0x14261C : 0x1A2133)),
                    ch_rgb(g->sel == k + 1 ? 0xFFE45E
                                           : (esta ? 0x4ADE80 : 0x3D465F)));
            ch_rect(b, LX + 3, y + 3, 30, h - 6, ch_rgb(0x0E111A));
            ch_part_draw(b, cat, PIEZA_VAR(id), LX + 18, y + h / 2, 1,
                         (int)ta_robot(g)->skin);
            ch_text(b, LX + 38, y + 5, _(p->nombre), ch_rgb(0xFFFFFF));
            snprintf(t, sizeof(t), _("PV%d E%d"), p->vida, p->energia);
            ch_text(b, LX + 38, y + 17, t, ch_rgb(0x606B85));
            /* EL TIPO DE LA PIEZA, que es lo que arma el juego: sin el, la
             * bonificacion por llevar varias del mismo tipo es un numero que
             * cambia solo y no se sabe por que. */
            {
                const char *tn = _(ch_tipo_nombre[p->tipo % TIPOS]);
                ch_text(b, LX + 38 + ch_text_w(_(p->nombre)) + 6, y + 5, tn,
                        ch_rgb(ch_tipo_color[p->tipo % TIPOS]));
            }
            if (esta) {
                ch_text(b, LX + LW - 5 - ch_text_w(_("PUESTA")), y + 17,
                        _("PUESTA"), ch_rgb(0x4ADE80));
            }

            /* LA DIFERENCIA, NO EL NUMERO PELADO: si es mejor o peor que la
             * que lleva puesta, que es la unica pregunta que uno tiene. */
            if (!esta) {
                static const char *const LET[3] = { "A", "D", "V" };
                int dif[3] = { p->atk - puesta->atk, p->def - puesta->def,
                               p->vel - puesta->vel };
                int x = LX + LW - 5;
                for (int q = 2; q >= 0; q--) {
                    snprintf(t, sizeof(t), "%s%+d", LET[q], dif[q]);
                    x -= ch_text_w(t) + 4;
                    ch_text(b, x, y + 17, t,
                            dif[q] > 0 ? ch_rgb(0x4ADE80)
                                       : (dif[q] < 0 ? ch_rgb(0xFF4A3D)
                                                     : ch_rgb(0x606B85)));
                }
            }
        }
    }
}

static void taller_toque(ch_t *g, int bx, int by)
{
    taller_geom();
    if (g->sel2 == 0) {
        int c = ta_bot_en(bx, by);
        if (c >= 0) {
            if (!ch_eq(&g->s, c)) { ch_sfx(220, 40); return; }
            s_ta_bot = (uint8_t)c;
            ch_sfx(1000, 25);
            g->rehacer_fondo = 1;
            return;
        }
        c = ta_cat_en(bx, by);
        if (c < 0) return;
        g->sel2 = (uint8_t)(c + 1);
        g->sel = 0;
        g->scroll = 0;
        ch_sfx(1000, 25);
        g->rehacer_fondo = 1;
        return;
    }
    {
        int cat = (g->sel2 - 1) % P_CATS;
        int f = flecha_en(bx, by);
        int k;
        uint8_t id, antes;

        ta_juntar(g, cat);
        ta_lista_geom();
        if (f) {
            int nuevo = (int)g->scroll + f;
            if (nuevo >= 0 && nuevo + s_lfilas <= s_nlista) {
                g->scroll = (uint8_t)nuevo;
                g->rehacer_fondo = 1;
            }
            return;
        }
        f = fila_en(bx, by);
        if (f < 0) return;
        k = g->scroll + f;
        if (k >= s_nlista) return;
        if (s_lista[k] == TA_PUESTA) {
            /* Es la que ya llevas: se muestra para comparar, no para montar. */
            g->sel = (uint8_t)(k + 1);
            ch_sfx(900, 20);
            g->rehacer_fondo = 1;
            return;
        }

        /* Primer toque: mostrar en que cambiaria. Segundo: montarla. Montar
         * de una hacia facil errarle a la fila y cambiar el robot sin
         * querer. */
        if (g->sel != (uint8_t)(k + 1)) {
            g->sel = (uint8_t)(k + 1);
            ch_sfx(900, 20);
            g->rehacer_fondo = 1;
            return;
        }
        {
            ch_robot_t *r = ta_robot(g);
            id = g->s.piezas[s_lista[k]];
            antes = PIEZA_ID(cat, r->pieza[cat]);
            r->pieza[cat] = PIEZA_VAR(id);
            g->s.piezas[s_lista[k]] = antes;
            ch_ver(&g->s, id);
            ch_ver(&g->s, antes);
            ch_robot_stats(r);
            if (r->vida > r->vida_max) r->vida = r->vida_max;
        }

        ch_sfx(1200, 40);
        ch_ui_aviso(g, _(ch_partes[id].nombre));
        /* Y de vuelta al robot, que es lo que uno queria ver. */
        g->sel2 = 0;
        g->sel = 0;
        g->scroll = 0;
        g->rehacer_fondo = 1;
        g->hud_sucio = 1;
    }
}

/* --------------------------------------------------------------------------
 * Items
 * -------------------------------------------------------------------------- */

/* The tall rows of the items, the shop, the scrap dealer and the diary. */
#define ITEM_FH     42

static void objetos_fondo(ch_t *g)
{
    char d[12];

    lista_geom(HDR + 2, ITEM_FH, 16);
    ch_ui_titulo(g, _("OBJETOS"), _("TOCA UNO PARA USARLO"));

    s_nlista = 0;
    for (int i = 1; i < ITEMS; i++) {
        if (g->s.obj[i]) s_lista[s_nlista++] = (uint8_t)i;
    }
    if (!s_nlista) {
        vacio(g, _("NO TENES NADA."));
        return;
    }
    flechas(g, g->scroll > 0, g->scroll + s_lfilas < s_nlista);
    for (int i = 0; i < s_lfilas; i++) {
        int k = g->scroll + i;
        if (k >= s_nlista) break;
        snprintf(d, sizeof(d), "x%d", g->s.obj[s_lista[k]]);
        fila_item(g, i, s_lista[k], d, ch_rgb(0xFFE45E), true);
    }
}

static void objetos_toque(ch_t *g, int bx, int by)
{
    int f = flecha_en(bx, by);
    if (f) {
        int s = (int)g->scroll + f;
        if (s >= 0 && s + s_lfilas <= s_nlista) { g->scroll = (uint8_t)s; g->rehacer_fondo = 1; }
        return;
    }
    f = fila_en(bx, by);
    if (f < 0) return;
    int k = g->scroll + f;
    if (k >= s_nlista) return;

    int it = s_lista[k];
    const ch_item_t *d = &ch_items[it];
    char aviso[26];

    switch (it) {
    case IT_REPELENTE:
        g->repele = (uint16_t)d->valor;
        snprintf(aviso, sizeof(aviso), "%s", _("NADA SE TE ACERCA"));
        g->s.obj[it]--;
        break;

    case IT_ORUGAS:
        /* Permanente: la bandera vale una vez y la segunda no se cobra. */
        if (ch_flag(&g->s, F_CFG_ORUGAS)) { ch_sfx(220, 40); return; }
        ch_flag_set(&g->s, F_CFG_ORUGAS);
        snprintf(aviso, sizeof(aviso), "%s", _("AHORA CAMINAS AL DOBLE"));
        g->s.obj[it]--;
        break;

    case IT_BOLSA:
        if (ch_mochila(&g->s) >= CH_MAX_MOCHILA) { ch_sfx(220, 40); return; }
        ch_flag_set(&g->s, ch_flag(&g->s, F_CFG_BOLSA_A) ? F_CFG_BOLSA_B
                                                         : F_CFG_BOLSA_A);
        snprintf(aviso, sizeof(aviso), _("MOCHILA: %d LUGARES"),
                 ch_mochila(&g->s));
        g->s.obj[it]--;
        break;

    case IT_ACEITE: case IT_ACEITE2: {
        if (g->s.yo.vida >= g->s.yo.vida_max) { ch_sfx(220, 40); return; }
        int cura = d->valor;
        if (g->s.yo.vida + cura > g->s.yo.vida_max) cura = g->s.yo.vida_max - g->s.yo.vida;
        g->s.yo.vida = (int16_t)(g->s.yo.vida + cura);
        g->s.obj[it]--;
        snprintf(aviso, sizeof(aviso), _("+%d DE VIDA"), cura);
        break;
    }
    case IT_BATERIA: case IT_BATERIA2: {
        if (g->s.yo.ene >= g->s.yo.ene_max) { ch_sfx(220, 40); return; }
        int c = d->valor;
        if (g->s.yo.ene + c > g->s.yo.ene_max) c = g->s.yo.ene_max - g->s.yo.ene;
        g->s.yo.ene = (int16_t)(g->s.yo.ene + c);
        g->s.obj[it]--;
        snprintf(aviso, sizeof(aviso), _("+%d DE ENERGIA"), c);
        break;
    }
    case IT_CHIP:
        g->s.yo.exp += 300;
        g->s.obj[it]--;
        while (g->s.yo.nivel < 60 && g->s.yo.exp >= ch_exp_nivel(g->s.yo.nivel + 1)) {
            g->s.yo.nivel++;
            ch_robot_curar(&g->s.yo);
            ch_snd_melodia(g, CH_MEL_NIVEL);
        }
        snprintf(aviso, sizeof(aviso), _("NIVEL %d"), g->s.yo.nivel);
        break;
    default:
        ch_sfx(220, 40);
        return;
    }

    ch_sfx(1100, 40);
    ch_ui_aviso(g, aviso);
    g->rehacer_fondo = 1;
    g->hud_sucio = 1;
}


/* --------------------------------------------------------------------------
 * THE TEAM (what used to be the robot's data card)
 *
 * With three robots the card stopped being about one of them. Three tabs at
 * the top choose which robot you are looking at, and the two buttons at the
 * bottom are what you can do with that one.
 *
 * An empty slot is not hidden, it is an offer: ARMAR builds a robot out of the
 * loose parts in the bag. That is the only place in the game that explains why
 * you would keep a spare head.
 *
 * P4OS: measured from the screen area. Standing up the robot is at scale 3
 * and the panel beside it runs down to the buttons; lying down the robot is
 * at scale 2 and the panel is wider.
 * -------------------------------------------------------------------------- */
static int EQ_Y, EQ_H, EQ_W, EQ_RX, EQ_RY, EQ_RE, EQ_PX, EQ_PW, EQ_VY, EQ_VH,
           EQ_CY, EQ_BY, EQ_BH;

static void eq_geom(void)
{
    EQ_Y  = HDR + 2;
    EQ_H  = HORIZ ? 24 : 28;
    EQ_W  = (SW - 8 - 6) / EQUIPO;
    EQ_RE = HORIZ ? 2 : 3;                  /* the robot's scale             */
    EQ_RX = 42;                             /* its centre                    */
    EQ_RY = EQ_Y + EQ_H + (HORIZ ? 4 : 10);
    EQ_PX = HORIZ ? 86 : 84;
    EQ_PW = SW - EQ_PX - 6;
    EQ_VY = EQ_RY;
    EQ_VH = HORIZ ? 14 : 18;
    EQ_CY = EQ_VY + EQ_VH + 3;
    EQ_BH = HORIZ ? 20 : 24;
    EQ_BY = pie_util() - EQ_BH - (HORIZ ? 0 : 4);
}

static int eq_slot_en(int bx, int by)
{
    if (by < EQ_Y || by >= EQ_Y + EQ_H) return -1;
    for (int i = 0; i < EQUIPO; i++) {
        int x = 4 + i * (EQ_W + 3);
        if (bx >= x && bx < x + EQ_W) return i;
    }
    return -1;
}

static int eq_boton_en(int bx, int by)
{
    if (by < EQ_BY || by >= EQ_BY + EQ_BH) return -1;
    return bx < SW / 2 ? 0 : 1;
}

/* Uno de los dos botones del panel: los numeros o los golpes. */
static void eq_vista(ch_t *g, int i, const char *txt, bool sel)
{
    ch_buf_t *b = &g->bg;
    int w = (EQ_PW - 3) / 2;
    int x = EQ_PX + i * (w + 3);

    ch_caja(b, x, EQ_VY, w, EQ_VH, ch_rgb(sel ? 0x2B3145 : 0x141720),
            ch_rgb(sel ? 0xFFE45E : 0x3D465F));
    ch_text_center(b, x + w / 2, EQ_VY + EQ_VH / 2 - 3, txt,
                   ch_rgb(sel ? 0xFFFFFF : 0x606B85), ch_rgb(0x05060C));
}

static int eq_vista_en(int bx, int by)
{
    int w = (EQ_PW - 3) / 2;
    if (by < EQ_VY || by >= EQ_VY + EQ_VH) return -1;
    for (int i = 0; i < 2; i++) {
        int x = EQ_PX + i * (w + 3);
        if (bx >= x && bx < x + w) return i;
    }
    return -1;
}

static void eq_boton(ch_t *g, int i, const char *txt, bool activo)
{
    ch_buf_t *b = &g->bg;
    int x = i ? SW / 2 + 2 : 4;
    int w = SW / 2 - 6;

    ch_caja(b, x, EQ_BY, w, EQ_BH, ch_rgb(activo ? 0x2A3350 : 0x141824),
            ch_rgb(activo ? 0x8A93AB : 0x2B3145));
    ch_text_center(b, x + w / 2, EQ_BY + EQ_BH / 2 - 3, txt,
                   ch_rgb(activo ? 0xFFFFFF : 0x545C70), ch_rgb(0x05060C));
}

static void ficha_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    int sel = g->sel < EQUIPO ? g->sel : 0;
    const ch_robot_t *r = ch_eq(&g->s, sel);
    char t[30];

    eq_geom();
    if (!r) { sel = 0; r = &g->s.yo; g->sel = 0; }

    {
        int pct = 0;
        const char *j = ch_robot_juego_nombre(r, &pct);
        if (j) {
            snprintf(t, sizeof(t), _("%s  %s +%d%%"),
                     _(ch_tipo_nombre[r->tipo % TIPOS]), _(j), pct);
        } else {
            snprintf(t, sizeof(t), _("NIVEL %d   TIPO %s"), r->nivel,
                     _(ch_tipo_nombre[r->tipo % TIPOS]));
        }
    }
    ch_ui_titulo(g, ch_robot_nombre(r), t);

    /* --- the three tabs --------------------------------------------------- */
    for (int i = 0; i < EQUIPO; i++) {
        const ch_robot_t *q = ch_eq(&g->s, i);
        int x = 4 + i * (EQ_W + 3);
        bool aqui = (i == sel);
        int ty = EQ_Y + (EQ_H - 24) / 2;

        ch_caja(b, x, EQ_Y, EQ_W, EQ_H, ch_rgb(aqui ? 0x2A3350 : 0x141824),
                ch_rgb(aqui ? 0xFFE45E : 0x2B3145));
        if (!q) {
            ch_text_center(b, x + EQ_W / 2, EQ_Y + EQ_H / 2 - 3, _("VACIO"),
                           ch_rgb(0x545C70), ch_rgb(0x05060C));
            continue;
        }
        /* Three rows: the name, the level with its state, and the health. */
        snprintf(t, sizeof(t), "%.*s", (EQ_W - 8) / CH_FADV, ch_robot_nombre(q));
        ch_text(b, x + 4, ty + 3, t, ch_rgb(q->vida > 0 ? 0xFFFFFF : 0xE05252));
        snprintf(t, sizeof(t), _("N%d"), q->nivel);
        ch_text(b, x + 4, ty + 12, t, ch_rgb(0xFFE45E));
        {
            const char *e = i == 0 ? _("SALE") : (q->vida <= 0 ? _("ROTO") : "");
            if (e[0]) {
                ch_text(b, x + EQ_W - 4 - ch_text_w(e), ty + 12, e,
                        ch_rgb(i == 0 ? 0x8A93AB : 0xE05252));
            }
        }
        ch_barra(b, x + 4, ty + 20, EQ_W - 8, q->vida, q->vida_max,
                 ch_rgb(q->vida > 0 ? 0x4ADE80 : 0xE05252));
    }

    /* --- the selected robot, whole ---------------------------------------- */
    pedestal(b, EQ_RX, EQ_RY + 40 * EQ_RE - 2, EQ_RE);
    ch_robot_draw(b, EQ_RX, EQ_RY, r, EQ_RE, false, 0, 0);

    /* --- and the panel, which is one of TWO -------------------------------- */
    eq_vista(g, 0, _("NUMEROS"), g->sel2 == 0);
    eq_vista(g, 1, _("GOLPES"),  g->sel2 != 0);
    ch_panel(b, EQ_PX, EQ_CY, EQ_PW, EQ_BY - EQ_CY - 4, ch_rgb(0x3D465F));

    if (g->sel2 == 0) {
        static const char *const ET[5] = { N_("VIDA"), N_("ENER"), N_("ATAQ"),
                                           N_("DEFE"), N_("VELO") };
        int val[5] = { r->vida, r->ene, r->atk, r->def, r->vel };
        int tope[5] = { r->vida_max, r->ene_max, 0, 0, 0 };
        uint32_t col[5] = { 0x4ADE80, 0x4A9DF5, 0xFF9F0A, 0x7BE9FF, 0xB072F0 };
        int paso = (EQ_BY - EQ_CY - 14) / 5;

        if (paso > 18) paso = 18;
        for (int i = 0; i < 5; i++) {
            int y = EQ_CY + 7 + i * paso;
            ch_text(b, EQ_PX + 7, y, _(ET[i]), ch_rgb(0x8A93AB));
            if (tope[i]) snprintf(t, sizeof(t), "%d/%d", val[i], tope[i]);
            else         snprintf(t, sizeof(t), "%d", val[i]);
            ch_text(b, EQ_PX + EQ_PW - 7 - ch_text_w(t), y, t, ch_rgb(col[i]));
            /* standing up there is room for a bar under each number */
            if (paso >= 16 && tope[i]) {
                ch_barra(b, EQ_PX + 7, y + 9, EQ_PW - 14, val[i], tope[i],
                         ch_rgb(col[i]));
            }
        }
    } else {
        int paso = (EQ_BY - EQ_CY - 8) / 4;
        if (paso > 30) paso = 30;
        for (int i = 0; i < 4; i++) {
            int y = EQ_CY + 5 + i * paso;
            const ch_move_t *m;
            if (i) ch_hline(b, EQ_PX + 5, y - 3, EQ_PW - 10, ch_rgb(0x232B41));
            if (i >= r->nmov) {
                ch_text(b, EQ_PX + 7, y + 3, "-", ch_rgb(0x3D465F));
                continue;
            }
            m = &ch_moves[r->mov[i] % MOVES];
            ch_text(b, EQ_PX + 7, y, _(m->nombre), ch_rgb(0xFFFFFF));
            ch_text(b, EQ_PX + 7, y + 9, _(ch_tipo_nombre[m->tipo % TIPOS]),
                    ch_rgb(ch_tipo_color[m->tipo % TIPOS]));
            snprintf(t, sizeof(t), _("%d/%dE"), m->poder, m->costo);
            ch_text(b, EQ_PX + EQ_PW - 7 - ch_text_w(t), y + 9, t,
                    ch_rgb(0x8A93AB));
        }
    }

    /* --- what can be done with it ----------------------------------------- */
    if (sel == 0) {
        bool puede = ch_eq_puede_armar(&g->s);
        eq_boton(g, 0, _("ARMAR OTRO"), puede);
        eq_boton(g, 1, _("VOLVER"), true);

    } else {
        eq_boton(g, 0, _("QUE SALGA ESTE"), r->vida > 0);
        eq_boton(g, 1, _("DESARMAR"), true);
    }
}

static void ficha_toque(ch_t *g, int bx, int by)
{
    int i, b;

    eq_geom();
    i = eq_slot_en(bx, by);
    if (i >= 0) {
        if (ch_eq(&g->s, i)) { g->sel = (uint8_t)i; ch_sfx(1000, 20); }
        else if (ch_eq_puede_armar(&g->s)) {
            int slot = ch_eq_armar(&g->s);
            if (slot > 0) {
                g->sel = (uint8_t)slot;
                ch_sfx(1500, 90);
                ch_ui_aviso(g, _("ROBOT ARMADO!"));
                g->quiere_guardar = 1;
            }
        } else {
            ch_sfx(220, 40);
            ch_ui_aviso(g, _("FALTAN PIEZAS SUELTAS"));
        }
        g->rehacer_fondo = 1;
        return;
    }

    {   /* los dos botones del panel: numeros o golpes */
        int v = eq_vista_en(bx, by);
        if (v >= 0) {
            g->sel2 = (uint8_t)v;
            ch_sfx(1000, 20);
            g->rehacer_fondo = 1;
            return;
        }
    }

    b = eq_boton_en(bx, by);
    if (b < 0) return;

    if (g->sel == 0) {
        if (b == 1) { g->sel = 0; g->modo = MODO_MENU; ch_sfx(700, 30); }
        else if (ch_eq_puede_armar(&g->s)) {
            int slot = ch_eq_armar(&g->s);
            if (slot > 0) {
                g->sel = (uint8_t)slot;
                ch_sfx(1500, 90);
                ch_ui_aviso(g, _("ROBOT ARMADO!"));
                g->quiere_guardar = 1;
            }
        } else {
            ch_sfx(220, 40);
            ch_ui_aviso(g, _("FALTAN PIEZAS SUELTAS"));
        }
    } else if (b == 0) {
        const ch_robot_t *r = ch_eq(&g->s, g->sel);
        if (!r || r->vida <= 0) {
            ch_sfx(220, 40);
            ch_ui_aviso(g, _("ESTA ROTO: AL TALLER"));
        } else {
            ch_eq_activar(&g->s, g->sel);
            g->sel = 0;
            ch_sfx(1400, 70);
            ch_ui_aviso(g, _("CAMBIASTE DE ROBOT"));
            g->hud_sucio = 1;
            g->quiere_guardar = 1;
        }
    } else {
        if (ch_eq_desarmar(&g->s, g->sel)) {
            g->sel = 0;
            ch_sfx(500, 90);
            ch_ui_aviso(g, _("PIEZAS A LA MOCHILA"));
            g->quiere_guardar = 1;
        } else {
            ch_sfx(220, 40);
            ch_ui_aviso(g, _("MOCHILA LLENA!"));
        }
    }
    g->rehacer_fondo = 1;
}

/* --------------------------------------------------------------------------
 * Shop
 * -------------------------------------------------------------------------- */

/* EL SURTIDO CRECE CON LA ZONA.
 *
 * Las ocho tiendas vendian exactamente lo mismo, asi que llegar a un pueblo
 * nuevo no cambiaba nada: el aceite puro estaba a la venta en el minuto cinco
 * -con 180 creditos que no tenias- y el iman tambien. Ahora cada zona agrega
 * lo suyo, que es lo que hace que valga la pena entrar a la tienda de un
 * pueblo al que acabas de llegar.
 *
 * Es una tabla de cuantos items del arreglo se ven, no ocho arreglos: el
 * orden ya es de barato a caro. */
static const uint8_t SURTIDO[] = {
    IT_ACEITE, IT_BATERIA, IT_REPELENTE, IT_ACEITE2, IT_BATERIA2, IT_ORUGAS,
    IT_CHIP, IT_IMAN, IT_BOLSA, IT_SOLDADOR,
};
static const uint8_t SURTIDO_ZONA[ZONAS] = { 3, 5, 6, 7, 8, 9, 10, 10 };

static int surtido_n(const ch_t *g)
{
    const ch_room_t *r = &ch_salas[g->s.sala % ch_nsalas];
    int z = r->zona < 1 ? 1 : (r->zona > ZONAS ? ZONAS : r->zona);
    return SURTIDO_ZONA[z - 1];
}
#define NSURTIDO surtido_n(g)

static void tienda_fondo(ch_t *g)
{
    char t[30], d[16];

    lista_geom(HDR + 2, ITEM_FH, 16);
    snprintf(t, sizeof(t), _("TENES %d CREDITOS"), g->s.creditos);
    ch_ui_titulo(g, _("TIENDA"), t);

    flechas(g, g->scroll > 0, g->scroll + s_lfilas < NSURTIDO);
    for (int i = 0; i < s_lfilas; i++) {
        int k = g->scroll + i;
        if (k >= NSURTIDO) break;
        const ch_item_t *it = &ch_items[SURTIDO[k]];
        snprintf(d, sizeof(d), _("%dC x%d"), it->precio, g->s.obj[SURTIDO[k]]);
        /* The description is in every row: buying blind by the name is what
         * made nobody buy anything but oil. */
        fila_item(g, i, SURTIDO[k], d, ch_rgb(0xFFE45E),
                  g->s.creditos >= it->precio);
        /* the row touched once, framed: the second tap buys it */
        if (g->sel == k + 1) {
            ch_frame(&g->bg, LX, s_ly0 + i * s_lfh, LW, s_lfh - 4,
                     ch_rgb(0xFFE45E));
        }
    }
}

static void tienda_toque(ch_t *g, int bx, int by)
{
    int f = flecha_en(bx, by), k;

    if (f) {
        int n = (int)g->scroll + f;
        if (n >= 0 && n + s_lfilas <= NSURTIDO) {
            g->scroll = (uint8_t)n;
            g->sel = 0;
            g->rehacer_fondo = 1;
        }
        return;
    }
    f = fila_en(bx, by);
    if (f < 0) return;
    k = g->scroll + f;
    if (k >= NSURTIDO) return;

    /* The first tap shows what it does; the second buys. A stray tap cannot
     * cost you 400 credits. */
    if (g->sel != k + 1) {
        g->sel = (uint8_t)(k + 1);
        ch_sfx(900, 20);
        g->rehacer_fondo = 1;
        return;
    }

    int it = SURTIDO[k];
    const ch_item_t *d = &ch_items[it];
    if (g->s.creditos < d->precio) { ch_sfx(220, 40); ch_ui_aviso(g, _("NO TE ALCANZA")); g->rehacer_fondo = 1; return; }
    if (g->s.obj[it] >= 99) return;

    g->s.creditos = (uint16_t)(g->s.creditos - d->precio);
    g->s.obj[it]++;
    ch_sfx(1300, 40);
    ch_ui_aviso(g, _("COMPRADO"));
    g->rehacer_fondo = 1;
    g->hud_sucio = 1;
}

/* --------------------------------------------------------------------------
 * The parts register
 *
 * The 64 parts, really drawn and not listed by name. The ones you have not
 * come across yet appear as a SILHOUETTE: you see the shape and nothing else.
 *
 * Four tabs at the top, the card of the selected part in the header, and the
 * grid below taking whatever is left: standing up that is all sixteen of a
 * category on one page, four by four; lying down it is two pages of eight.
 * -------------------------------------------------------------------------- */
static int RG_HDR, RG_TX, RG_TY, RG_TW, RG_TH, RG_GX, RG_GY, RG_CW, RG_CH,
           RG_COLS, RG_FILAS, RG_PAG;

static void rg_geom(void)
{
    RG_HDR = HORIZ ? 40 : 52;
    RG_TX = 6;
    RG_TY = RG_HDR + 2;
    RG_TW = (SW - 12 - 6) / 4;
    RG_TH = HORIZ ? 16 : 20;
    RG_GX = 6;
    RG_GY = RG_TY + RG_TH + 4;
    RG_COLS = 4;
    RG_CW = (SW - 12) / RG_COLS;
    RG_FILAS = (pie_util() - RG_GY) / 44;
    if (RG_FILAS > 4) RG_FILAS = 4;
    if (RG_FILAS < 1) RG_FILAS = 1;
    RG_CH = (pie_util() - RG_GY) / RG_FILAS;
    if (RG_CH > 60) RG_CH = 60;
    RG_PAG = RG_FILAS * RG_COLS;
}

static int rg_paginas(void)
{
    return (PVAR + RG_PAG - 1) / RG_PAG;
}

static int rg_celda(int bx, int by)
{
    int c, f;

    if (bx < RG_GX || by < RG_GY) return -1;
    c = (bx - RG_GX) / RG_CW;
    f = (by - RG_GY) / RG_CH;
    if (c >= RG_COLS || f >= RG_FILAS) return -1;
    return f * RG_COLS + c;
}

static void registro_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    int cat = g->sel2 % P_CATS;
    int pag, n = 0;
    char t[34];

    rg_geom();
    pag = g->scroll % rg_paginas();
    for (int i = 0; i < PIEZAS; i++) if (ch_visto(&g->s, i)) n++;

    pantalla(b);
    boton_volver(b);
    /* The counter goes NEXT TO the title and not in the right-hand corner:
     * that corner belongs to the page arrows. */
    ch_text_sh(b, TIT_X, 8, _("REGISTRO"), ch_rgb(0xFFE45E), ch_rgb(0x05060C));
    snprintf(t, sizeof(t), "%d/%d", n, PIEZAS);
    ch_text(b, TIT_X + ch_text_w(_("REGISTRO")) + 8, 8, t, ch_rgb(0x8A93AB));

    /* The card: the part you are looking at, in the header. */
    {
        int var = pag * RG_PAG + (g->sel % RG_PAG);
        const ch_part_t *p = &ch_partes[PIEZA_ID(cat, var % PVAR)];
        bool visto = ch_visto(&g->s, PIEZA_ID(cat, var % PVAR));
        int y1 = HORIZ ? 18 : 30;

        /* lying down the name shares its row with the back button */
        int nx = HORIZ ? TIT_X : 8;

        if (!visto) {
            ch_text(b, nx, y1, _("SIN DATOS"), ch_rgb(0x606B85));
        } else {
            const char *tn = _(ch_tipo_nombre[p->tipo % TIPOS]);
            ch_text(b, nx, y1, _(p->nombre), ch_rgb(0xFFFFFF));
            snprintf(t, sizeof(t), _("PV%d A%d D%d V%d E%d"),
                     p->vida, p->atk, p->def, p->vel, p->energia);
            ch_text(b, 8, y1 + 11, t, ch_rgb(0x8A93AB));
            ch_text(b, SW - 8 - ch_text_w(tn), y1 + (HORIZ ? 11 : 0), tn,
                    ch_rgb(ch_tipo_color[p->tipo % TIPOS]));
        }
    }
    ch_hline(b, 6, RG_HDR - 1, SW - 12, ch_rgb(0x3D465F));
    if (rg_paginas() > 1) flechas(g, pag > 0, pag + 1 < rg_paginas());

    for (int c = 0; c < P_CATS; c++) {
        int x = RG_TX + c * (RG_TW + 2);
        bool sel = (c == cat);
        ch_caja(b, x, RG_TY, RG_TW, RG_TH, sel ? ch_rgb(0x2B3145) : ch_rgb(0x141720),
                sel ? ch_rgb(0xFFE45E) : ch_rgb(0x3D465F));
        ch_text_center(b, x + RG_TW / 2, RG_TY + RG_TH / 2 - 3, _(CATS[c]),
                       sel ? ch_rgb(0xFFFFFF) : ch_rgb(0x606B85), ch_rgb(0x05060C));
    }

    for (int k = 0; k < RG_PAG; k++) {
        int i = pag * RG_PAG + k;
        int x = RG_GX + (k % RG_COLS) * RG_CW;
        int y = RG_GY + (k / RG_COLS) * RG_CH;
        bool visto, puesta;

        if (i >= PVAR) break;
        visto  = ch_visto(&g->s, PIEZA_ID(cat, i));
        puesta = (g->s.yo.pieza[cat] == i);

        ch_caja(b, x + 1, y + 1, RG_CW - 3, RG_CH - 3, ch_rgb(0x0E111A),
                ch_rgb(0x1A1F2E));
        /* EL CENTRO DE LA CELDA, no su pie: ch_part_draw() ya coloca cada
         * categoria para que quede centrada en el punto que se le da. */
        ch_part_draw(b, cat, i, x + RG_CW / 2 - 1, y + RG_CH / 2, 2,
                     visto ? (int)g->s.yo.skin : -1);
        /* GREEN the one you are wearing, YELLOW the one you are looking at. */
        if (puesta) ch_frame(b, x, y, RG_CW - 1, RG_CH - 1, ch_rgb(0x4ADE80));
        if (k == (g->sel % RG_PAG)) {
            ch_frame(b, x + 1, y + 1, RG_CW - 3, RG_CH - 3, ch_rgb(0xFFE45E));
        }
    }
}

static void registro_toque(ch_t *g, int bx, int by)
{
    int c;

    rg_geom();
    if (by >= RG_TY && by < RG_TY + RG_TH) {
        c = (bx - RG_TX) / (RG_TW + 2);
        if (c >= 0 && c < P_CATS) {
            g->sel2 = (uint8_t)c;
            g->sel = 0;
            ch_sfx(1000, 20);
            g->rehacer_fondo = 1;
        }
        return;
    }
    if (rg_paginas() > 1) {
        /* the page arrows, which is how the other variants are got to */
        int f = flecha_en(bx, by);
        if (f) {
            int p = (int)g->scroll + f;
            if (p >= 0 && p < rg_paginas()) g->scroll = (uint8_t)p;
            g->sel = 0;
            ch_sfx(1000, 20);
            g->rehacer_fondo = 1;
            return;
        }
    }
    c = rg_celda(bx, by);
    if (c >= 0 && (g->scroll % rg_paginas()) * RG_PAG + c < PVAR) {
        g->sel = (uint8_t)c;
        ch_sfx(1100, 20);
        g->rehacer_fondo = 1;
    }
}

/* --------------------------------------------------------------------------
 * The closing screen
 *
 * It appears on beating the champion. It is not a credits list: it is the
 * robot you built, large and in the middle, and the four numbers of what it
 * cost.
 * -------------------------------------------------------------------------- */
static void final_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    char t[30];
    int vistas = 0;
    int esc = HORIZ ? 2 : 3;
    int rcx = HORIZ ? 64 : SW / 2;          /* the robot                     */
    int ry = HORIZ ? 36 : 34;
    /* the panel of numbers: under the robot standing up, beside it lying
     * down, where there is no "under" */
    int px = HORIZ ? 124 : 8, pw = HORIZ ? SW - 130 : SW - 16;
    int py = HORIZ ? 40 : ry + 40 * esc + 18, ph = HORIZ ? 76 : 46;

    for (int i = 0; i < PIEZAS; i++) if (ch_visto(&g->s, i)) vistas++;

    ch_vgrad(b, 0, 0, SW, SH, ch_rgb(0x2B2145), ch_rgb(0x0B0D14));
    {   /* fixed stars, at the canvas's resolution */
        ch_buf_t nb = ch_nativo(b);
        for (int i = 0; i < 90; i++) {
            ch_rect(&nb, (i * 61) % (SW * 2), (i * 37) % (SH * 2), 1, 1,
                    ch_rgb(((i * 7) & 1) ? 0x8A93AB : 0x3D465F));
        }
    }

    ch_text_big(b, SW / 2 - ch_text_w(_("CAMPEON")), 6, _("CAMPEON"),
                ch_rgb(0xFFE45E), ch_rgb(0x8E4630), 2);
    pedestal(b, rcx, ry + 40 * esc - 2, esc);
    ch_robot_draw(b, rcx, ry, &g->s.yo, esc, false, 0, 0);

    ch_text_center(b, rcx, ry + 40 * esc + 5, ch_robot_nombre(&g->s.yo),
                   ch_rgb(0xFFFFFF), ch_rgb(0x05060C));

    ch_panel(b, px, py, pw, ph, ch_rgb(0x8A93AB));
    {
        const char *et[4];
        char v[4][40];
        et[0] = _("NIVEL %d");    snprintf(v[0], sizeof(v[0]), et[0], g->s.yo.nivel);
        et[1] = _("COMBATES %d"); snprintf(v[1], sizeof(v[1]), et[1], g->s.victorias);
        et[2] = _("PIEZAS %d/%d");snprintf(v[2], sizeof(v[2]), et[2], vistas, PIEZAS);
        et[3] = _("PASOS %d");    snprintf(v[3], sizeof(v[3]), et[3], (int)g->s.pasos);
        for (int i = 0; i < 4; i++) {
            if (HORIZ) {
                ch_text(b, px + 8, py + 9 + i * 16, v[i], ch_rgb(0xD5DCEB));
            } else {
                int x = (i & 1) ? px + pw - 6 - ch_text_w(v[i]) : px + 6;
                ch_text(b, x, py + 7 + (i >> 1) * 12, v[i], ch_rgb(0xD5DCEB));
            }
        }
    }
    (void)t;
    ch_text_center(b, SW / 2, HORIZ ? py + ph + 14 : py + 33,
                   _("NI UNA PIEZA TE LA REGALARON"),
                   ch_rgb(0x8A93AB), ch_rgb(0x05060C));
}

/* --------------------------------------------------------------------------
 * The help
 *
 * Four pages. The game has parts, types, workshop, bag, controls and
 * sub-bosses, and all that is explained today in the grandmother's dialogue:
 * if you skipped it or came back two weeks later, there is nowhere to look it
 * up. A help screen is not documentation, it is the difference between a
 * system and a mystery.
 * -------------------------------------------------------------------------- */

static const char *const AYUDA[] = {
    N_("TU ROBOT SON CUATRO PIEZAS:\n"
       "CABEZA, TORSO, BRAZOS Y\n"
       "PIERNAS.\n"
       "\n"
       "CADA UNA APORTA VIDA,\n"
       "ATAQUE, DEFENSA, VELOCIDAD\n"
       "Y ENERGIA, Y TRAE SUS\n"
       "PROPIOS ATAQUES.\n"
       "\n"
       "EL TORSO ADEMAS DECIDE EL\n"
       "TIPO DE TU ROBOT."),
    N_("AL GANAR UN COMBATE PODES\n"
       "ARRANCARLE UNA PIEZA AL\n"
       "RIVAL.\n"
       "\n"
       "VA A LA MOCHILA Y SE MONTA\n"
       "EN EL MENU, EN TALLER.\n"
       "\n"
       "LA QUE TENIAS PUESTA VUELVE\n"
       "A LA MOCHILA: NUNCA SE\n"
       "PIERDE NADA."),
    N_("HAY SEIS TIPOS Y SE GANAN\n"
       "ENTRE ELLOS:\n"
       "\n"
       "IMPACTO > CRIO Y ACIDO\n"
       "PLASMA  > IMPACTO Y VOLT\n"
       "FUEGO   > CRIO Y ACIDO\n"
       "CRIO    > PLASMA Y VOLT\n"
       "VOLT    > FUEGO E IMPACTO\n"
       "ACIDO   > VOLT Y PLASMA\n"
       "\n"
       "PEGAR CON TU PROPIO TIPO\n"
       "HACE MAS DANO."),
    N_("CADA ZONA TIENE UN DUNGEON\n"
       "Y UN SUBJEFE AL FONDO.\n"
       "\n"
       "VENCERLO TE DA UN PASE DE\n"
       "SECTOR, Y ESE PASE ABRE EL\n"
       "CONTROL DE LA ZONA QUE\n"
       "SIGUE.\n"
       "\n"
       "TOCA DONDE QUERES CAMINAR.\n"
       "EL BOTON MENU, O TOCAR TU\n"
       "ROBOT, ABRE ESTE MENU.\n"
       "CON DOS DEDOS ACERCAS O\n"
       "ALEJAS EL MAPA."),
};
#define NAYUDA ((int)(sizeof(AYUDA) / sizeof(AYUDA[0])))
/* --------------------------------------------------------------------------
 * EL DIARIO
 *
 * Los ocho encargos ya existian enteros en los datos: un PNJ con `p3` -la
 * bandera de "traeme esto"- y un cofre en alguna parte cuya `p3` es la misma.
 * Lo unico que faltaba era una pantalla que los junte, porque el juego te
 * contaba el encargo una vez, en un dialogo, y tres pueblos despues no habia
 * forma de acordarse de que buscabas ni donde te lo habian pedido.
 *
 * No lleva tabla nueva: se recorre el mundo. 61 salas por diez entidades es
 * nada, y se hace una vez por fondo, no por cuadro. Y al no haber tabla no
 * puede quedar desincronizada con los encargos de verdad, que es el modo en
 * que estas listas se pudren.
 * -------------------------------------------------------------------------- */
#define DIARIO_MAX  12

static struct { uint8_t sala, item, estado; } s_diario[DIARIO_MAX];
static int s_ndiario;

static void diario_juntar(ch_t *g)
{
    s_ndiario = 0;
    for (int si = 0; si < ch_nsalas && s_ndiario < DIARIO_MAX; si++) {
        const ch_room_t *r = &ch_salas[si];
        for (int i = 0; i < r->nents && s_ndiario < DIARIO_MAX; i++) {
            const ch_ent_t *e = &r->ents[i];
            uint8_t item = 0;

            if (e->tipo != E_PNJ || !e->p3) continue;
            /* que hay que traerle: el cofre cuya bandera es la misma */
            for (int sj = 0; sj < ch_nsalas && !item; sj++) {
                const ch_room_t *q = &ch_salas[sj];
                for (int k = 0; k < q->nents; k++) {
                    if (q->ents[k].tipo == E_COFRE &&
                        q->ents[k].p3 == e->p3) { item = q->ents[k].p1; break; }
                }
            }
            s_diario[s_ndiario].sala = (uint8_t)si;
            s_diario[s_ndiario].item = item;
            s_diario[s_ndiario].estado =
                (e->p2 && ch_flag(&g->s, e->p2)) ? 2
                : (ch_flag(&g->s, e->p3) ? 1 : 0);
            s_ndiario++;
        }
    }
}

/* --------------------------------------------------------------------------
 * EL CHATARRERO
 *
 * Un agujero que abrimos nosotros: entre la feria, los cofres y lo que le
 * arrancas a cada rival, la mochila se llena de piezas PEORES que las puestas
 * y no habia nada que hacer con ellas. Vender cierra el circulo -pelear,
 * arrancar, vender, comprar aceite- y le da una razon mas para volver al
 * pueblo.
 *
 * El precio sale de la pieza y no de una tabla: vida + ataque + defensa +
 * velocidad por cuatro. Una pieza mala vale poco y una buena vale la pena
 * pensarla, que es exactamente la decision que uno quiere que exista.
 * -------------------------------------------------------------------------- */
static int precio_pieza(uint8_t id)
{
    const ch_part_t *p = &ch_partes[id % PIEZAS];
    return (p->vida + p->atk + p->def + p->vel) * 4;
}

static void vender_juntar(ch_t *g)
{
    s_nlista = 0;
    for (int i = 0; i < ch_mochila(&g->s); i++) {
        if (g->s.piezas[i] < PIEZAS) s_lista[s_nlista++] = (uint8_t)i;
    }
}


static void vender_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    char t[40];

    vender_juntar(g);
    lista_geom(HDR + 2, ITEM_FH, 16);
    snprintf(t, sizeof(t), _("TENES %d CREDITOS"), g->s.creditos);
    ch_ui_titulo(g, _("CHATARRERO"), t);

    if (!s_nlista) {
        vacio(g, _("NO TENES PIEZAS SUELTAS"));
        return;
    }
    flechas(g, g->scroll > 0, g->scroll + s_lfilas < s_nlista);

    for (int i = 0; i < s_lfilas; i++) {
        int k = g->scroll + i;
        int y = s_ly0 + i * s_lfh, h = s_lfh - 4;
        uint8_t id;
        const ch_part_t *p;

        if (k >= s_nlista) break;
        id = g->s.piezas[s_lista[k]];
        p = &ch_partes[id];

        ch_caja(b, LX, y, LW, h,
                ch_rgb(g->sel == k + 1 ? 0x2A3350 : 0x1A2133),
                ch_rgb(g->sel == k + 1 ? 0xFFE45E : 0x3D465F));
        ch_rect(b, LX + 3, y + 3, 30, h - 6, ch_rgb(0x0E111A));
        ch_part_draw(b, PIEZA_CAT(id), PIEZA_VAR(id), LX + 18, y + h / 2, 1,
                     (int)g->s.yo.skin);
        ch_text(b, LX + 38, y + 6, _(p->nombre), ch_rgb(0xFFFFFF));
        ch_text(b, LX + 38 + ch_text_w(_(p->nombre)) + 6, y + 6,
                _(ch_tipo_nombre[p->tipo % TIPOS]),
                ch_rgb(ch_tipo_color[p->tipo % TIPOS]));
        snprintf(t, sizeof(t), _("PV%d E%d"), p->vida, p->energia);
        ch_text(b, LX + 38, y + 20, t, ch_rgb(0x606B85));
        snprintf(t, sizeof(t), _("%dC"), precio_pieza(id));
        ch_text(b, LX + LW - 6 - ch_text_w(t), y + 20, t, ch_rgb(0xFFE45E));
        if (g->sel == k + 1) {
            ch_text(b, LX + LW - 6 - ch_text_w(_("TOCA 2X")), y + 6,
                    _("TOCA 2X"), ch_rgb(0x8A93AB));
        }
    }
}

static void vender_toque(ch_t *g, int bx, int by)
{
    int f = flecha_en(bx, by), k;

    if (f) {
        int n = (int)g->scroll + f;
        if (n >= 0 && n + s_lfilas <= s_nlista) {
            g->scroll = (uint8_t)n;
            g->rehacer_fondo = 1;
        }
        return;
    }
    f = fila_en(bx, by);
    if (f < 0) return;
    k = g->scroll + f;
    if (k >= s_nlista) return;

    /* El segundo toque vende: una pieza vendida no se recupera. */
    if (g->sel != (uint8_t)(k + 1)) {
        g->sel = (uint8_t)(k + 1);
        ch_sfx(900, 20);
        g->rehacer_fondo = 1;
        return;
    }
    {
        uint8_t id = g->s.piezas[s_lista[k]];
        int cr = precio_pieza(id);
        char aviso[30];

        g->s.piezas[s_lista[k]] = 0xFF;
        g->s.creditos = (uint16_t)(g->s.creditos + cr > 9999 ? 9999
                                                  : g->s.creditos + cr);
        snprintf(aviso, sizeof(aviso), _("+%dC"), cr);
        ch_ui_aviso(g, aviso);
        ch_sfx(1300, 60);
        g->sel = 0;
        g->scroll = 0;
        g->quiere_guardar = 1;
        g->hud_sucio = 1;
        g->rehacer_fondo = 1;
    }
}

static void diario_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    char t[40];
    int abiertos = 0;

    diario_juntar(g);
    for (int i = 0; i < s_ndiario; i++) if (s_diario[i].estado != 2) abiertos++;

    lista_geom(HDR + 2, ITEM_FH, 16);
    ch_ui_titulo(g, _("DIARIO"), _("LO QUE TE PIDIERON"));
    snprintf(t, sizeof(t), "%d/%d", abiertos, s_ndiario);
    ch_text(b, TIT_X + ch_text_w(_("DIARIO")) + 8, HORIZ ? 6 : 8, t,
            ch_rgb(0x8A93AB));

    if (!s_ndiario) {
        vacio(g, _("NADIE TE PIDIO NADA"));
        return;
    }
    flechas(g, g->scroll > 0, g->scroll + s_lfilas < s_ndiario);

    for (int i = 0; i < s_lfilas; i++) {
        int k = g->scroll + i;
        int y = s_ly0 + i * s_lfh, h = s_lfh - 4;
        int est;
        uint32_t c;

        if (k >= s_ndiario) break;
        est = s_diario[k].estado;
        c = est == 2 ? 0x4ADE80 : (est == 1 ? 0xFFE45E : 0x8A93AB);

        ch_caja(b, LX, y, LW, h, ch_rgb(est == 2 ? 0x14261C : 0x1A2133),
                ch_rgb(est == 2 ? 0x1E7A3C : 0x3D465F));
        ch_rect(b, LX + 4, y + h / 2 - 14, 28, 28, ch_rgb(0x0E111A));
        ch_ui_icono(b, LX + 6, y + h / 2 - 12,
                    s_diario[k].item < ITEMS && ICONO_ITEM[s_diario[k].item]
                        ? ICONO_ITEM[s_diario[k].item] : IC_HERRAMIENTA, 2);

        ch_text(b, LX + 38, y + 6, _(ch_salas[s_diario[k].sala].nombre),
                ch_rgb(0xFFFFFF));
        if (est == 2) {
            snprintf(t, sizeof(t), "%s", _("ENTREGADO"));
        } else if (est == 1) {
            snprintf(t, sizeof(t), "%s", _("LO TENES: VOLVE"));
        } else if (s_diario[k].item < ITEMS) {
            snprintf(t, sizeof(t), _("TRAE: %s"),
                     _(ch_items[s_diario[k].item].nombre));
        } else {
            snprintf(t, sizeof(t), "%s", _("PENDIENTE"));
        }
        {
            char lin[2][30];
            int n = ch_wrap(t, (LW - 44) / CH_FADV, lin, 2);
            for (int q = 0; q < n; q++) {
                ch_text(b, LX + 38, y + 18 + q * 9, lin[q], ch_rgb(c));
            }
        }
    }
}

static void ayuda_fondo(ch_t *g)
{
    char lin[16][30];
    int n, pag = g->sel % NAYUDA;
    int y0 = HDR + 6, paso = HORIZ ? 9 : 12;
    int pista = pie_util() - 12;
    char t[12];

    snprintf(t, sizeof(t), "%d/%d", pag + 1, NAYUDA);
    ch_ui_titulo(g, _("AYUDA"), NULL);
    ch_text(&g->bg, SW - 8 - ch_text_w(t), HORIZ ? 6 : 8, t, ch_rgb(0xFFE45E));

    /* As many lines as the page has, measured from the top, and the hint at
     * the foot: the only thing with a variable height is the gap between. */
    n = ch_wrap(_(AYUDA[pag]), 27, lin, 16);
    ch_panel(&g->bg, 4, y0 - 4, SW - 8, pista - y0 - 2, ch_rgb(0x3D465F));
    for (int i = 0; i < n && y0 + 4 + i * paso + 7 < pista - 6; i++) {
        ch_text(&g->bg, 4 + (SW - 8 - 27 * CH_FADV) / 2, y0 + 4 + i * paso,
                lin[i], ch_rgb(0xD5DCEB));
    }
    ch_text_center(&g->bg, SW / 2, pista + 2, _("TOCA PARA SEGUIR"),
                   ch_rgb(0x606B85), ch_rgb(0x05060C));
}

/* --------------------------------------------------------------------------
 * The world map
 *
 * EL MAPA ES OCHO BOTONES: cada zona es una ficha con su numero, su estado y
 * -si ya estuviste- el viaje. Standing up the eight fit on one screen; lying
 * down they go four at a time and the arrows turn the page.
 * -------------------------------------------------------------------------- */
static int MM_Y0, MM_FH, MM_FILAS, MM_H;

static void mm_geom(void)
{
    MM_Y0 = HDR + 3;
    MM_FH = 31;
    if ((pie_util() - MM_Y0) / MM_FH < ZONAS &&
        (pie_util() - MM_Y0) / 26 >= ZONAS) {
        MM_FH = (pie_util() - MM_Y0) / ZONAS;
    }
    MM_FILAS = (pie_util() - MM_Y0) / MM_FH;
    if (MM_FILAS > ZONAS) MM_FILAS = ZONAS;
    MM_H = MM_FH - 3;
}

static int mm_paginas(void)
{
    return (ZONAS + MM_FILAS - 1) / MM_FILAS;
}

/* VIAJE RAPIDO. Solo a una zona en la que ya estuviste, y siempre a su
 * pueblo: el pueblo es donde estan el taller y la tienda, que es para lo que
 * se vuelve. */
static void mapa_toque(ch_t *g, int bx, int by)
{
    int pag, f;

    mm_geom();
    pag = g->scroll % mm_paginas();
    f = mm_paginas() > 1 ? flecha_en(bx, by) : 0;
    if (f) {
        int p = pag + f;
        if (p >= 0 && p < mm_paginas()) g->scroll = (uint8_t)p;
        ch_sfx(1000, 20);
        g->rehacer_fondo = 1;
        return;
    }
    if (by < MM_Y0 || bx < 6 || bx >= SW - 6) return;
    {
        int i = (by - MM_Y0) / MM_FH;
        int z = pag * MM_FILAS + i;
        const ch_zona_t *zo;

        if (i < 0 || i >= MM_FILAS || z >= ZONAS) return;
        if ((by - MM_Y0) % MM_FH >= MM_H) return;       /* el hueco entre fichas */
        zo = &ch_zonas_tab[z];

        if (!ch_flag(&g->s, zo->visita)) {
            ch_sfx(220, 40);
            ch_ui_aviso(g, _("TODAVIA NO ESTUVISTE AHI"));
            return;
        }
        if (g->s.sala >= zo->sala0 && g->s.sala <= zo->sala1) {
            ch_sfx(220, 40);
            ch_ui_aviso(g, _("YA ESTAS AHI"));
            return;
        }
        ch_sfx(1500, 90);
        ch_ui_aviso(g, _("VIAJANDO..."));
        g->sel = g->sel2 = g->scroll = 0;
        g->modo = MODO_MAPA;
        ch_map_entrar(g, zo->casa, zo->casa_x, zo->casa_y);
        g->quiere_guardar = 1;
        g->rehacer_fondo = 1;
    }
}

static void mapa_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    int aqui = 0, hechas = 0, pag;
    char t[24];

    mm_geom();
    pag = g->scroll % mm_paginas();
    for (int z = 0; z < ZONAS; z++) {
        if (g->s.sala >= ch_zonas_tab[z].sala0 &&
            g->s.sala <= ch_zonas_tab[z].sala1) aqui = z;
        if (ch_flag(&g->s, ch_zonas_tab[z].bandera)) hechas++;
    }

    snprintf(t, sizeof(t), "%d/%d", hechas, ZONAS);
    ch_ui_titulo(g, _("MAPA"), _("TOCA PARA VIAJAR"));
    ch_text(b, TIT_X + ch_text_w(_("MAPA")) + 8, HORIZ ? 6 : 8, t,
            ch_rgb(0x8A93AB));
    if (mm_paginas() > 1) flechas(g, pag > 0, pag + 1 < mm_paginas());

    for (int i = 0; i < MM_FILAS; i++) {
        int z = pag * MM_FILAS + i;
        int y = MM_Y0 + i * MM_FH;
        bool ok, yo, visto;
        uint16_t marco;

        if (z >= ZONAS) break;
        ok    = ch_flag(&g->s, ch_zonas_tab[z].bandera);
        yo    = (z == aqui);
        visto = ch_flag(&g->s, ch_zonas_tab[z].visita);
        marco = yo ? ch_rgb(0xFFE45E)
                   : (visto ? ch_rgb(0x3D465F) : ch_rgb(0x232B41));

        ch_caja(b, 6, y, SW - 12, MM_H,
                ch_rgb(yo ? 0x2A3350 : (visto ? 0x161C2E : 0x101320)), marco);

        /* El numero de zona, en su propia casilla: es el orden del mundo. */
        ch_caja(b, 10, y + 3, 20, MM_H - 6,
                ch_rgb(ok ? 0x1E7A3C : (visto ? 0x232B41 : 0x171B29)),
                ch_rgb(0x3D465F));
        snprintf(t, sizeof(t), "%d", z + 1);
        ch_text_center(b, 20, y + MM_H / 2 - 3, t,
                       ch_rgb(visto ? 0xFFFFFF : 0x545C70), ch_rgb(0x05060C));

        ch_text(b, 36, y + MM_H / 2 - 9, _(ch_zonas_tab[z].nombre),
                ch_rgb(visto ? (yo ? 0xFFFFFF : 0xD5DCEB) : 0x545C70));

        {
            const char *e = !visto ? _("SIN EXPLORAR")
                          : yo      ? _("ESTAS AQUI")
                          : ok      ? _("VENCIDA - VIAJAR")
                                    : _("VIAJAR");
            ch_text(b, 36, y + MM_H / 2 + 2, e,
                    ch_rgb(!visto ? 0x545C70 : yo ? 0xFFE45E
                                  : ok ? 0x4ADE80 : 0x8A93AB));
        }
        /* La flecha de viaje: solo donde se puede viajar. */
        if (visto && !yo) {
            ch_buf_t nb = ch_nativo(b);
            int k = b->s;
            int ax = ch_fx(b, SW - 22), ay = ch_fy(b, y + MM_H / 2);
            for (int q = 0; q < 5 * k; q++) {
                ch_rect(&nb, ax + q, ay - 5 * k + q, 1, 2 * (5 * k - q),
                        ch_rgb(0x4ADE80));
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Title screen
 *
 * The whole canvas, not the screen area: there is no HUD before the game has
 * started. Standing up it is a column - the name, the robot on its plinth,
 * EMPEZAR and how it is played - and lying down the robot goes to the left
 * and the rest to the right.
 * -------------------------------------------------------------------------- */
static int TIT_BX, TIT_BY, TIT_BW, TIT_BH;

static void titulo_geom(void)
{
    TIT_BW = 116;
    TIT_BH = 28;
    if (HORIZ) {
        TIT_BX = 150 + (UW - 150 - TIT_BW) / 2;
        TIT_BY = 92;
    } else {
        TIT_BX = (UW - TIT_BW) / 2;
        TIT_BY = 228;
    }
}

static void titulo_fondo(ch_t *g)
{
    ch_buf_t *b = &g->bg;
    ch_robot_t demo;
    uint32_t semilla = 0xC0FFEEu;
    int horizonte = HORIZ ? 120 : 214;
    int esc = HORIZ ? 2 : 3;
    int rcx = HORIZ ? 76 : UW / 2;
    int ry = horizonte - 40 * esc + 4;
    int tx = HORIZ ? 150 : 0, tw = HORIZ ? UW - 150 : UW;   /* the text column */
    int ty = HORIZ ? 18 : 18;

    titulo_geom();
    ch_vgrad(b, 0, 0, UW, horizonte, ch_rgb(0x0B0D14), ch_rgb(0x2B2145));
    ch_vgrad(b, 0, horizonte, UW, UH - 1, ch_rgb(0x2B2145), ch_rgb(0x120E1E));

    /* fixed stars, at the canvas's resolution: they come from an implicit
     * table (the coordinate) and not from a roll, so the background can be
     * repainted identically */
    {
        ch_buf_t nb = ch_nativo(b);
        for (int i = 0; i < 140; i++) {
            int x = (i * 61) % (UW * 2), y = (i * 37) % ((horizonte - 20) * 2);
            uint16_t c = ch_rgb(((i * 7) & 1) ? 0x8A93AB : 0x3D465F);
            ch_rect(&nb, x, y, 1, 1, c);
            if ((i % 17) == 0) {        /* a few bigger ones, with a cross */
                ch_rect(&nb, x - 1, y, 3, 1, c);
                ch_rect(&nb, x, y - 1, 1, 3, c);
            }
        }
    }
    /* a skyline of scrap on the horizon: the game's name, drawn */
    {
        uint32_t r = 0xA11CEu;
        for (int x = -4; x < UW; ) {
            int w = 6 + ch_rnd(&r, 12), h = 6 + ch_rnd(&r, 20);
            ch_rect(b, x, horizonte - h, w, h, ch_rgb(0x1A1430));
            ch_rect(b, x, horizonte - h, w, 1, ch_rgb(0x2E2450));
            x += w + ch_rnd(&r, 4);
        }
    }
    ch_rect(b, 0, horizonte, UW, 1, ch_rgb(0x4A3A78));

    ch_robot_random(&demo, &semilla, 20, 6);
    demo.skin = 2;
    pedestal(b, rcx, horizonte + 2, esc);
    ch_robot_draw(b, rcx, ry, &demo, esc, false, 0, 0);

    /* The name, big, and what it is. */
    {
        const char *nom = "CHATARRA";
        int m = HORIZ ? 2 : 3;
        int w = ch_text_w(nom) * m;
        ch_text_big(b, tx + (tw - w) / 2, HORIZ ? ty : ty, nom,
                    ch_rgb(0xFFE45E), ch_rgb(0x8E4630), m);
        ch_text_center(b, tx + tw / 2, ty + 7 * m + 6, _("RPG DE ROBOTS"),
                       ch_rgb(0x8A93AB), ch_rgb(0x05060C));
    }

    ch_snd_melodia(g, CH_MEL_TITULO);
    ch_caja(b, TIT_BX, TIT_BY, TIT_BW, TIT_BH, ch_rgb(0x2A3350), ch_rgb(0xFFE45E));
    ch_text_big(b, TIT_BX + (TIT_BW - ch_text_w(_("EMPEZAR")) * 2) / 2,
                TIT_BY + TIT_BH / 2 - 7, _("EMPEZAR"), ch_rgb(0xFFFFFF),
                ch_rgb(0x05060C), 2);

    /* How it is played, in three lines: none of it can be guessed. */
    {
        int y = HORIZ ? TIT_BY + TIT_BH + 12 : UH - ch_lay.pie - 40;
        ch_text_center(b, tx + tw / 2, y, _("TOCA DONDE QUERES CAMINAR"),
                       ch_rgb(0x8A93AB), ch_rgb(0x05060C));
        ch_text_center(b, tx + tw / 2, y + 11, _("DOS DEDOS: ACERCAR Y ALEJAR"),
                       ch_rgb(0x606B85), ch_rgb(0x05060C));
        ch_text_center(b, tx + tw / 2, y + 22, _("EL BOTON MENU, O TU ROBOT"),
                       ch_rgb(0x606B85), ch_rgb(0x05060C));
    }
}

/* --------------------------------------------------------------------------
 * The dispatcher
 * -------------------------------------------------------------------------- */

/* THE WORLD'S BACKGROUND IS KEPT WHILE NOTHING IN IT CHANGED.
 *
 * On the watch every rebuild repainted the room, because the room was a
 * sixth of a frame. At zoom 4 it is 480 thousand canvas pixels through the
 * upscaler, and a dialogue turns its page with a rebuild: repainting the room
 * for that would be paying for the one thing that did not move. So the room
 * is drawn again only when something it shows can have changed - the room,
 * the zoom, a flag (a chest opened, a control lifted) or the hour, which
 * tints it. */
static uint32_t firma_mundo(const ch_t *g)
{
    uint32_t h = 2166136261u;
    h = (h ^ g->s.sala) * 16777619u;
    h = (h ^ g->zoom) * 16777619u;
    h = (h ^ (uint32_t)ch_hora(&g->s)) * 16777619u;
    for (unsigned i = 0; i < sizeof(g->s.bandera); i++) {
        h = (h ^ g->s.bandera[i]) * 16777619u;
    }
    return h | 1u;                      /* 0 means "never drawn"           */
}

static void mundo_fondo(ch_t *g)
{
    uint32_t f = firma_mundo(g);
    if (f == g->mundo_firma) return;
    ch_map_fondo(g);
    g->mundo_firma = f;
}

void ch_ui_fondo(ch_t *g)
{
    switch (g->modo) {
    case MODO_TITULO:  titulo_fondo(g);  break;
    case MODO_MENU:    menu_fondo(g);    break;
    case MODO_TALLER:  taller_fondo(g);  break;
    case MODO_OBJETOS: objetos_fondo(g); break;
    case MODO_FICHA:   ficha_fondo(g);   break;
    case MODO_TIENDA:  tienda_fondo(g);  break;
    case MODO_REGISTRO: registro_fondo(g); break;
    case MODO_MAPAMUNDI: mapa_fondo(g); break;
    case MODO_DIARIO:  diario_fondo(g); break;
    case MODO_VENDER:  vender_fondo(g); break;
    case MODO_FERIA:   ch_fe_fondo(g);  break;
    case MODO_AYUDA:   ayuda_fondo(g); break;
    case MODO_FINAL:   final_fondo(g); break;
    case MODO_CABINA:  ch_lk_fondo(g); break;
    case MODO_DIALOGO: mundo_fondo(g); dlg_fondo(g); break;
    default:           mundo_fondo(g);  break;
    }
}

/* What moves on the UI layer. The world's movers are drawn by chatarra.c
 * into the world layer (ch_map_dibujar), not here. */
void ch_ui_dibujar(ch_t *g)
{
    if (g->modo == MODO_DIALOGO) {
        if (g->dlg_chars < 250) g->dlg_chars = (uint8_t)(g->dlg_chars + 2);
        dlg_texto(g);
    }
    if (g->modo == MODO_CABINA) ch_lk_dibujar(g);
}

/* --------------------------------------------------------------------------
 * The touch
 * -------------------------------------------------------------------------- */

/* The screens that wear ch_ui_titulo()'s back button. */
static bool con_volver(const ch_t *g)
{
    switch (g->modo) {
    case MODO_MENU: case MODO_TALLER: case MODO_OBJETOS: case MODO_FICHA:
    case MODO_TIENDA: case MODO_REGISTRO: case MODO_MAPAMUNDI:
    case MODO_DIARIO: case MODO_VENDER: case MODO_AYUDA: case MODO_CABINA:
        return true;
    default:
        return false;
    }
}

void ch_ui_toque(ch_t *g, int bx, int by)
{
    /* The HUD answers in every mode that shows it; the dialogue is the one
     * that swallows every tap, the HUD's included, to turn its page. */
    if (g->modo != MODO_TITULO && g->modo != MODO_COMBATE &&
        g->modo != MODO_DIALOGO && en_hud(bx, by)) {
        hud_toque(g, bx, by);
        return;
    }
    if (con_volver(g) && volver_en(bx, by)) {
        ch_sfx(700, 25);
        (void)ch_ui_atras(g);
        return;
    }

    switch (g->modo) {
    case MODO_TITULO:
        titulo_geom();
        if (by >= TIT_BY - 4 && by < TIT_BY + TIT_BH + 4 &&
            bx >= TIT_BX - 4 && bx < TIT_BX + TIT_BW + 4) {
            ch_sfx(1200, 60);
            /* Sin parar nada: ch_map_entrar() pone el tema de la zona, y
             * pararlo aca dejaba un silencio de un cuadro en el medio. */
            g->modo = MODO_MAPA;
            g->mel_mapa = 0;
            ch_map_entrar(g, g->s.sala, g->s.x, g->s.y);
        }
        break;

    case MODO_DIALOGO:
        dlg_avanzar(g);
        break;

    case MODO_MENU: {
        int n, cols;
        const char *tit;
        const baldosa_t *p = pagina(g, &n, &cols, &tit);
        int f = baldosa_en(g, bx, by);

        if (f < 0) return;
        ch_sfx(1000, 25);
        g->sel = 0;
        g->scroll = 0;
        switch (p[f].accion) {
        case AC_PAG1:    g->sel2 = 1; break;
        case AC_PAG2:    g->sel2 = 2; break;
        case AC_TALLER:  g->modo = MODO_TALLER;  g->sel2 = 0;
                         g->sel = 0; g->scroll = 0; s_ta_bot = 0; break;
        case AC_OBJETOS: g->modo = MODO_OBJETOS; g->sel2 = 0; break;
        case AC_EQUIPO:  g->modo = MODO_FICHA;   g->sel2 = 0; break;
        case AC_REGISTRO:g->modo = MODO_REGISTRO;g->sel2 = 0; break;
        case AC_MAPA:    g->modo = MODO_MAPAMUNDI; g->sel2 = 0; break;
        case AC_DIARIO:  g->modo = MODO_DIARIO;  g->sel2 = 0;
                         g->scroll = 0; break;
        case AC_AYUDA:   g->modo = MODO_AYUDA;   g->sel2 = 0; break;
        case AC_SONIDO:  ch_sonido_set((ch_sonido_get() + 1) % 3); break;
        case AC_DIFICULTAD:
            /* Se puede cambiar cuando uno quiera, a proposito: el que se
             * traba en un jefe a las once de la noche no quiere volver a
             * empezar la partida, quiere pasar de ahi. */
            ch_dificultad_set(&g->s,
                (ch_dificultad(&g->s) + 1) % DIFICULTADES);
            g->quiere_guardar = 1;
            break;
        case AC_GUARDAR: g->quiere_guardar = 1;
                         ch_ui_aviso(g, _("PARTIDA GUARDADA")); break;
        case AC_CERRAR:  g->modo = MODO_MAPA;    g->sel2 = 0; break;
        default: break;
        }
        g->rehacer_fondo = 1;
        break;
    }

    case MODO_FINAL:
        g->modo = MODO_MAPA;
        g->mel_mapa = 0;
        ch_map_musica(g);
        g->rehacer_fondo = 1;
        break;

    case MODO_TALLER:  taller_toque(g, bx, by);  break;
    case MODO_OBJETOS: objetos_toque(g, bx, by); break;
    case MODO_TIENDA:  tienda_toque(g, bx, by);  break;
    case MODO_REGISTRO: registro_toque(g, bx, by); break;
    case MODO_FICHA:   ficha_toque(g, bx, by); break;
    case MODO_MAPAMUNDI: mapa_toque(g, bx, by); break;
    case MODO_DIARIO: {
        int f = flecha_en(bx, by);
        if (f) {
            int n = (int)g->scroll + f;
            if (n >= 0 && n + s_lfilas <= s_ndiario) {
                g->scroll = (uint8_t)n;
                g->rehacer_fondo = 1;
            }
        }
        break;
    }
    case MODO_AYUDA:
        if (++g->sel >= NAYUDA) { g->sel = 0; g->modo = MODO_MENU; }
        ch_sfx(1000, 20);
        g->rehacer_fondo = 1;
        break;

    case MODO_COMBATE: ch_bt_toque(g, bx, by);   break;
    case MODO_CABINA:  ch_lk_toque(g, bx, by);   break;
    case MODO_FERIA:   ch_fe_toque(g, bx, by);  break;
    case MODO_VENDER:  vender_toque(g, bx, by); break;

    default:
        /* The map itself is not here: chatarra.c turns a tap on its window
         * into world units and hands it to ch_map_toque(). */
        break;
    }
}

/* The back gesture, and the header's back button. Returns true if the app
 * consumed it. */
bool ch_ui_atras(ch_t *g)
{
    switch (g->modo) {
    case MODO_TALLER:
        if (g->sel2) { g->sel2 = 0; g->sel = 0; g->scroll = 0;
                       g->rehacer_fondo = 1; return true; }
        g->sel = 0;
        g->modo = MODO_MENU;
        g->rehacer_fondo = 1;
        return true;
    case MODO_OBJETOS:
    case MODO_FICHA:
    case MODO_REGISTRO:
    case MODO_MAPAMUNDI:
    case MODO_DIARIO:
    case MODO_AYUDA:
        g->sel = 0;
        g->scroll = 0;
        g->modo = MODO_MENU;
        g->rehacer_fondo = 1;
        return true;
    case MODO_MENU:
        /* The back gesture climbs ONE step: from a page to the root, and from
         * the root out to the map. Anything else and the two pages would be a
         * trap you can only leave by picking something. */
        if (g->sel2) { g->sel2 = 0; g->rehacer_fondo = 1; return true; }
        g->modo = MODO_MAPA;
        g->rehacer_fondo = 1;
        return true;
    case MODO_TIENDA:
    case MODO_VENDER:           /* al chatarrero se entra desde el mundo */
        g->sel = 0;
        g->scroll = 0;
        g->modo = MODO_MAPA;
        g->rehacer_fondo = 1;
        return true;
    case MODO_FERIA:
        /* Walking away from the belt ends the round: it pays what you had,
         * the same as the clock running out. */
        if (g->fe.resta > 1) g->fe.resta = 1;
        else ch_fe_toque(g, 0, 0);
        return true;
    case MODO_DIALOGO:
        dlg_avanzar(g);
        return true;
    case MODO_COMBATE:
        return ch_bt_atras(g);
    case MODO_CABINA:
        return ch_lk_atras(g);
    default:
        return false;
    }
}
