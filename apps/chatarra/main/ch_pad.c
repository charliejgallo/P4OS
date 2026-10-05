/*
 * CHATARRA - the gamepad
 *
 * The game was made for a finger and every screen answers taps. The pad does
 * not get a second set of rules: it is turned into taps.
 *
 *   the map        the d-pad walks a cell at a time, A acts on what you
 *                  face, START opens the menu, L and R are the zoom
 *                  (ch_map.c);
 *   a dialogue     A or B turns the page;
 *   the fair       a claw over the belt (ch_feria.c);
 *   everything     a CURSOR over the screen's TARGETS - the rectangles its
 *   else           touch answers to, which the screen itself publishes from
 *                  the geometry it draws with (ch_ui_blancos, ch_bt_blancos,
 *                  ch_lk_blancos). The d-pad moves it to the nearest target in
 *                  that direction, A taps the middle of it, B is the back
 *                  gesture and START the HUD's MENU button. Past the first
 *                  or last row of a list UP and DOWN turn it, as do L and R.
 *
 * A tap of a finger hides the cursor, and the next press of the pad only
 * shows it again, where it was: a player who only touches never sees it, and
 * one who picks the pad up does not act on a button they have not seen.
 */
#include "chatarra.h"

/* Which screen the cursor's index belongs to: when it changes, the cursor
 * starts at the screen's first target. The menu's page and the workshop's list
 * are screens of their own; the team card's two tabs are not. */
static uint32_t clave(const ch_t *g)
{
    uint32_t k = g->modo;

    if (g->modo == MODO_MENU || g->modo == MODO_TALLER) k |= (uint32_t)g->sel2 << 8;
    if (g->modo == MODO_COMBATE) k |= (uint32_t)g->bt.fase << 16;
    if (g->modo == MODO_CABINA) k |= (uint32_t)g->lk.estado << 16;
    return k | (uint32_t)HORIZ << 24;
}

static int blancos(ch_t *g, ch_blanco_t *z)
{
    uint32_t k = clave(g);
    int n;

    if (g->modo == MODO_COMBATE)     n = ch_bt_blancos(g, z, CH_BLANCOS_MAX);
    else if (g->modo == MODO_CABINA) n = ch_lk_blancos(g, z, CH_BLANCOS_MAX);
    else                             n = ch_ui_blancos(g, z, CH_BLANCOS_MAX);

    if (k != g->pad_clave) {
        g->pad_clave = k;
        g->pad_sel = 0;
    }
    /* the list got shorter (a part sold, the last item used) */
    {
        int ult = -1;
        for (int i = 0; i < n; i++) if (z[i].tipo == BL_BOTON) ult = i;
        if (g->pad_sel > ult || (ult >= 0 && z[g->pad_sel].tipo != BL_BOTON)) {
            g->pad_sel = (uint8_t)(ult < 0 ? 0 : ult);
        }
    }
    return n;
}

static void tocar(ch_t *g, const ch_blanco_t *z)
{
    ch_ui_toque(g, z->x + z->w / 2, z->y + z->h / 2);
}

/* The arrow of that kind, if the screen has one: tapped. */
static bool flecha(ch_t *g, const ch_blanco_t *z, int n, int tipo)
{
    for (int i = 0; i < n; i++) {
        if (z[i].tipo == tipo) { tocar(g, &z[i]); return true; }
    }
    return false;
}

/* The nearest target in that direction, straight ahead before off to the side
 * (the same rule as the OS's aos_pad_menu), or -1. */
static int vecina(const ch_blanco_t *z, int n, int sel, uint32_t dir)
{
    int cx = z[sel].x + z[sel].w / 2, cy = z[sel].y + z[sel].h / 2;
    int best = -1, bd = 0;

    for (int i = 0; i < n; i++) {
        int dx, dy, along, across, d;
        if (i == sel || z[i].tipo != BL_BOTON) continue;
        dx = z[i].x + z[i].w / 2 - cx;
        dy = z[i].y + z[i].h / 2 - cy;
        if (dir & CHP_ARRIBA)     { along = -dy; across = dx; }
        else if (dir & CHP_ABAJO) { along = dy;  across = dx; }
        else if (dir & CHP_IZQ)   { along = -dx; across = dy; }
        else                      { along = dx;  across = dy; }
        if (along <= 0) continue;
        if (across < 0) across = -across;
        d = along + 3 * across;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

static void cursor(ch_t *g, uint32_t pressed, uint32_t repeat)
{
    ch_blanco_t z[CH_BLANCOS_MAX];
    int n = blancos(g, z), botones = 0;
    uint32_t dirs = repeat & CHP_DIRS;

    for (int i = 0; i < n; i++) {
        if (z[i].tipo == BL_TODO) {
            /* nothing to point at, so the press is not spent on showing:
             * it taps, and the screen after it shows the cursor at once */
            if (pressed & CHP_A) { g->pad_visto = 1; tocar(g, &z[i]); }
            return;
        }
        if (z[i].tipo == BL_BOTON) botones++;
    }
    if (pressed & CHP_L) { (void)flecha(g, z, n, BL_ARRIBA); return; }
    if (pressed & CHP_R) { (void)flecha(g, z, n, BL_ABAJO);  return; }

    if (!botones) {
        /* a list with nothing to pick, only to read: the d-pad scrolls it */
        if (dirs & CHP_ARRIBA) (void)flecha(g, z, n, BL_ARRIBA);
        else if (dirs & CHP_ABAJO) (void)flecha(g, z, n, BL_ABAJO);
        return;
    }
    if (!dirs && !(pressed & CHP_A)) return;

    if (!g->pad_visto) {
        /* the first press only shows where it is - unless there is only one
         * thing it could be, like the title's button */
        g->pad_visto = 1;
        if (botones == 1 && (pressed & CHP_A)) tocar(g, &z[g->pad_sel]);
        return;
    }
    if (dirs) {
        int v = vecina(z, n, g->pad_sel, dirs);
        if (v >= 0) {
            g->pad_sel = (uint8_t)v;
            ch_sfx(1500, 8);
        } else if (dirs & CHP_ARRIBA) {
            (void)flecha(g, z, n, BL_ARRIBA);
        } else if (dirs & CHP_ABAJO) {
            (void)flecha(g, z, n, BL_ABAJO);
        }
        return;
    }
    tocar(g, &z[g->pad_sel]);
}

void ch_pad(ch_t *g, uint32_t held, uint32_t pressed, uint32_t repeat)
{
    if (!held && !pressed) return;
    /* A screen that changed this frame has not drawn itself yet, and its
     * targets live in the geometry its drawing leaves behind. One frame. */
    if (g->rehacer_fondo) return;

    switch (g->modo) {
    case MODO_MAPA:
        if (held & CHP_DIRS) g->pad_visto = 1;
        if (pressed & CHP_START) { ch_ui_menu(g); return; }
        if (pressed & CHP_L) g->zoom_pide = -1;
        if (pressed & CHP_R) g->zoom_pide = +1;
        ch_map_pad(g, held, pressed);
        return;

    case MODO_DIALOGO:
        if (pressed & (CHP_A | CHP_B)) { g->pad_visto = 1; (void)ch_ui_atras(g); }
        return;

    case MODO_FERIA:
        if (pressed) g->pad_visto = 1;
        if (pressed & CHP_B) { (void)ch_ui_atras(g); return; }
        ch_fe_pad(g, held, pressed, repeat);
        return;

    case MODO_TITULO:
        if (pressed & CHP_START) pressed |= CHP_A;
        break;

    default:
        if (pressed & CHP_START) { ch_ui_boton_menu(g); return; }
        /* Back, and never out of the game: on the screens where the back
         * gesture would leave (the map, a combat with nothing to undo) it
         * answers false and nothing happens. */
        if (pressed & CHP_B) { (void)ch_ui_atras(g); return; }
        break;
    }
    cursor(g, pressed, repeat);
}

/* The cursor: a white frame round the target, two units thick, drawn over
 * whatever moves. Four thin dirty strips, so the frame costs its outline
 * and not the button it frames. */
void ch_pad_dibujar(ch_t *g)
{
    ch_blanco_t z[CH_BLANCOS_MAX];
    const ch_blanco_t *q;
    int n;

    if (!g->pad_visto) return;
    if (g->modo == MODO_MAPA || g->modo == MODO_DIALOGO || g->modo == MODO_FERIA) return;
    n = blancos(g, z);
    if (!n || g->pad_sel >= n || z[g->pad_sel].tipo != BL_BOTON) return;
    q = &z[g->pad_sel];
    ch_frame(&g->fb, q->x - 2, q->y - 2, q->w + 4, q->h + 4, ch_rgb(0xFFFFFF));
    ch_frame(&g->fb, q->x - 1, q->y - 1, q->w + 2, q->h + 2, ch_rgb(0xFFFFFF));
    ch_dirty_add(&g->d_cur, q->x - 2, q->y - 2, q->w + 4, 2);
    ch_dirty_add(&g->d_cur, q->x - 2, q->y + q->h, q->w + 4, 2);
    ch_dirty_add(&g->d_cur, q->x - 2, q->y, 2, q->h);
    ch_dirty_add(&g->d_cur, q->x + q->w, q->y, 2, q->h);
}
