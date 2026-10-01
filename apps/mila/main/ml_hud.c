/*
 * MILA - what is drawn over a frame (see ml_hud.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "ml_hud.h"
#include "ml_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WHITE   0xFFFF
#define GOLD    0xFE48         /* ~ #FFC844 */
#define INK     0x0000

void ml_hud_free(ml_hud_t *h)
{
    for (int i = 0; i < 12; i++) {
        free(h->dig[i].a);
        free(h->sdig[i].a);
    }
    for (int i = 0; i < MSG_N; i++) free(h->msg[i].a);
    for (int i = 0; i < SYM_N; i++) free(h->sym[i].a);
    free(h->title.a);
    free(h->sub.a);
    for (int i = 0; i < ICO_N; i++) ml_anim_free(&h->ico[i]);
    memset(h, 0, sizeof(*h));
}

static int digit_idx(char c)
{
    return c == ':' ? 10 : c == '/' ? 11 : c - '0';
}

int ml_hud_number_w(const ml_hud_t *h, int n, bool big)
{
    char b[12];
    int len = 0;
    if (n < 0) n = 0;
    do {
        b[len++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && len < 10);
    int w = 0;
    for (int i = 0; i < len; i++) w += (big ? h->dig : h->sdig)[digit_idx(b[i])].w - 2;
    return w;
}

int ml_hud_number(const ml_hud_t *h, ml_img_t *im, int n, int x, int y, bool big, uint16_t c, int alpha)
{
    char b[12];
    int len = 0;
    if (n < 0) n = 0;
    do {
        b[len++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && len < 10);
    int x0 = x;
    for (int i = len - 1; i >= 0; i--) {
        const ml_mask_t *m = &(big ? h->dig : h->sdig)[digit_idx(b[i])];
        ml_mask_draw(im, m, x, y, c, alpha);
        x += m->w - 2;
    }
    return x - x0;
}

void ml_hud_pill(ml_img_t *im, int x, int y, int w, int hgt, uint16_t c, int alpha)
{
    ml_rrect(im, x, y, w, hgt, hgt / 2, c, alpha);
}

void ml_hud_mask_centered(ml_img_t *im, const ml_mask_t *m, int cx, int cy, uint16_t c, int alpha)
{
    if (!m->a) return;
    ml_mask_draw(im, m, cx - m->w / 2, cy - m->h / 2, c, alpha);
}

void ml_hud_button(ml_img_t *im, const ml_mask_t *glyph, int cx, int cy, int r, bool lit)
{
    ml_disc(im, cx * 16, cy * 16, r * 16, lit ? ml_rgb(255, 200, 70) : ml_rgb(20, 16, 30), lit ? 230 : 170);
    ml_hud_mask_centered(im, glyph, cx, cy, lit ? ml_rgb(40, 20, 0) : WHITE, 255);
}

void ml_hud_icons_load(ml_hud_t *h)
{
    static const char *const n[ICO_N] = { "icon_play", "icon_shop", "icon_gear", "icon_link", "icon_undo",
                                          "icon_restart", "icon_home", "icon_coin", "icon_star" };
    for (int i = 0; i < ICO_N; i++)
        if (ml_art_has(n[i])) ml_art_load(n[i], &h->ico[i]);
}

bool ml_hud_icon(ml_img_t *im, const ml_hud_t *h, int ico, int cx, int cy, int alpha)
{
    const ml_anim_t *a = &h->ico[ico];
    if (!a->n || a->fmt != ML_PX_IMG) return false;
    const ml_spr_t *s = &a->f[0];
    int x0s = cx - s->w / 2, y0s = cy - s->h / 2;
    int r0 = im->cy0 - y0s, r1 = im->cy1 - y0s;
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    for (int r = r0; r < r1; r++) {
        int a0, a1;
        const uint8_t *p = ml_spr_row(s, r, &a0, &a1);
        int c0 = x0s + a0, c1 = x0s + a1;
        if (c0 < im->cx0) { p += (size_t)(im->cx0 - c0) * 3; c0 = im->cx0; }
        if (c1 > im->cx1) c1 = im->cx1;
        uint16_t *dst = im->px + (size_t)(y0s + r) * im->w;
        for (int x = c0; x < c1; x++, p += 3) {
            int al = p[2] * alpha >> 8;
            if (al) dst[x] = ml_blend(dst[x], (uint16_t)(p[0] | (p[1] << 8)), al);
        }
    }
    return true;
}

void ml_hud_button_ico(ml_img_t *im, const ml_hud_t *h, int ico, const ml_mask_t *glyph, int cx, int cy, int r, bool lit)
{
    ml_disc(im, cx * 16, cy * 16, r * 16, lit ? ml_rgb(255, 200, 70) : ml_rgb(20, 16, 30), lit ? 230 : 170);
    if (!ml_hud_icon(im, h, ico, cx, cy, 255)) ml_hud_mask_centered(im, glyph, cx, cy, lit ? ml_rgb(40, 20, 0) : WHITE, 255);
}

void ml_hud_star(ml_img_t *im, int cx, int cy, int r, uint16_t c)
{
    /* a star from discs is too blobby: scanline fill of the polygon */
    float px[10], py[10];
    for (int i = 0; i < 10; i++) {
        float ang = -1.5708f + i * 0.6283f;
        float rr = (i & 1) ? r * 0.45f : (float)r;
        px[i] = cx + rr * cosf(ang);
        py[i] = cy + rr * sinf(ang);
    }
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < im->cy0 || y >= im->cy1) continue;
        float xs[10];
        int n = 0;
        float fy = y + 0.5f;
        for (int i = 0, j = 9; i < 10; j = i++) {
            if ((py[i] > fy) != (py[j] > fy)) xs[n++] = px[i] + (fy - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
        }
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++)
                if (xs[b] < xs[a]) {
                    float t = xs[a];
                    xs[a] = xs[b];
                    xs[b] = t;
                }
        for (int k = 0; k + 1 < n; k += 2) {
            int x0 = (int)(xs[k] + 0.5f), x1 = (int)(xs[k + 1] + 0.5f);
            if (x0 < im->cx0) x0 = im->cx0;
            if (x1 > im->cx1) x1 = im->cx1;
            uint16_t *row = im->px + (size_t)y * im->w;
            for (int x = x0; x < x1; x++) row[x] = c;
        }
    }
}

/* the pills at the top: where they go on the screen as it is */
typedef struct {
    int tx, ty, tw;             /* the targets' pill                       */
    int mx, my, mw;             /* moves / par                             */
    int rx, ry;                 /* the race's pill (left edge, top)       */
    int ph;                     /* a pill's height                         */
} hud_lay_t;

static int targets_w(int n) { return n * 24 + 24; }

static void lay(const ml_hud_t *h, const ml_hud_level_t *s, hud_lay_t *L)
{
    bool wide = ML_W > ML_H;
    L->ph = h->sdig[0].h + 18;
    if (L->ph < 44) L->ph = 44;
    int wm = ml_hud_number_w(h, s->moves, false), wp = ml_hud_number_w(h, s->par, false);
    int ws = h->sdig[11].w - 2;
    L->mw = wm + ws + wp + 32;
    L->mx = ML_W - 20 - L->mw;
    L->my = 32;
    L->tw = targets_w(s->targets);
    if (wide) {
        /* lying down: the right-hand corner, one under the other */
        L->tx = ML_W - 20 - L->tw;
        L->ty = L->my + L->ph + 12;
    } else {
        L->tx = ML_W / 2 - L->tw / 2;
        L->ty = L->my;
    }
    L->rx = wide ? ML_W - 20 : ML_W / 2;         /* right edge lying down, centre upright */
    L->ry = (wide ? L->ty : L->my) + L->ph + 12;
}

void ml_hud_level(const ml_hud_t *h, ml_img_t *im, const ml_hud_level_t *s)
{
    hud_lay_t L;
    lay(h, s, &L);
    /* pause, top-left */
    ml_hud_button(im, &h->sym[SYM_PAUSE], HUD_PAUSE_X, HUD_PAUSE_Y, HUD_PAUSE_R, false);
    /* the things on their targets */
    if (s->targets > 0 && s->targets <= 12) {
        ml_hud_pill(im, L.tx, L.ty, L.tw, L.ph, INK, 130);
        int cy = L.ty + L.ph / 2;
        for (int i = 0; i < s->targets; i++) {
            bool on = i < s->on;
            ml_disc(im, (L.tx + 24 + i * 24) * 16, cy * 16, 8 * 16, on ? ml_rgb(255, 205, 70) : ml_rgb(255, 255, 255),
                    on ? 255 : 80);
        }
    }
    /* moves / par, top-right */
    int ws = h->sdig[11].w - 2;
    int x = L.mx, y = L.my;
    ml_hud_pill(im, x, y, L.mw, L.ph, INK, 140);
    int ty = y + L.ph / 2 - h->sdig[0].h / 2;
    x += 16;
    x += ml_hud_number(h, im, s->moves, x, ty, false, s->moves <= s->par ? WHITE : ml_rgb(255, 190, 160), 255);
    ml_mask_draw(im, &h->sdig[11], x, ty, ml_rgb(200, 200, 210), 200);
    x += ws;
    ml_hud_number(h, im, s->par, x, ty, false, ml_rgb(255, 205, 70), 255);
    /* a race: the friend's things on targets and moves */
    if (s->race && s->targets > 0 && s->targets <= 12) {
        int gap = 18, w = s->targets * gap + 60 + ml_hud_number_w(h, s->rival_moves, false);
        int x0 = ML_W > ML_H ? L.rx - w : L.rx - w / 2;
        int cy = L.ry + L.ph / 2;
        ml_hud_pill(im, x0, L.ry, w, L.ph, ml_rgb(60, 20, 50), 170);
        ml_hud_mask_centered(im, &h->sym[SYM_FRIEND], x0 + 22, cy, ml_rgb(255, 170, 200), 255);
        for (int i = 0; i < s->targets; i++)
            ml_disc(im, (x0 + 48 + i * gap) * 16, cy * 16, 6 * 16, i < s->rival_on ? ml_rgb(255, 150, 190) : 0xFFFF,
                    i < s->rival_on ? 255 : 70);
        ml_hud_number(h, im, s->rival_moves, x0 + 44 + s->targets * gap, cy - h->sdig[0].h / 2, false,
                      ml_rgb(255, 200, 220), 230);
    }
    /* undo, restart */
    ml_hud_button_ico(im, h, ICO_UNDO, &h->sym[SYM_UNDO], HUD_UNDO_X, HUD_UNDO_Y, HUD_BTN_R, s->undo_flash > 0);
    ml_hud_button_ico(im, h, ICO_RESTART, &h->sym[SYM_RESTART], HUD_RESTART_X, HUD_RESTART_Y, HUD_BTN_R,
                      s->restart_flash > 0);
    if (s->msg_t >= 0 && h->msg[MSG_WELL].a) {
        float t = s->msg_t;
        int a = t < 0.2f ? (int)(t / 0.2f * 255) : 255;
        const ml_mask_t *m = &h->msg[MSG_WELL];
        int cy = ML_H / 2 - (ML_W > ML_H ? 150 : 220);
        ml_hud_pill(im, ML_W / 2 - m->w / 2 - 28, cy - m->h / 2 - 12, m->w + 56, m->h + 24, INK, a * 170 >> 8);
        ml_hud_mask_centered(im, m, ML_W / 2, cy, ml_rgb(255, 215, 90), a);
    }
}

void ml_hud_level_boxes(const ml_hud_t *h, const ml_hud_level_t *s, ml_dmg_t *d)
{
    hud_lay_t L;
    lay(h, s, &L);
    int r = HUD_PAUSE_R + 2;
    ml_dmg_rect(d, HUD_PAUSE_X - r, HUD_PAUSE_Y - r, 2 * r, 2 * r);
    /* the pills: from the widest they can be (the numbers change width) */
    ml_dmg_rect(d, L.tx - 4, L.ty - 4, L.tw + 8, L.ph + 8);
    ml_dmg_rect(d, L.mx - 60, L.my - 4, L.mw + 64, L.ph + 8);
    if (s->race) ml_dmg_rect(d, ML_W > ML_H ? L.rx - 420 : L.rx - 210, L.ry - 4, 420, L.ph + 8);
    r = HUD_BTN_R + 2;
    ml_dmg_rect(d, HUD_UNDO_X - r, HUD_UNDO_Y - r, 2 * r, 2 * r);
    ml_dmg_rect(d, HUD_RESTART_X - r, HUD_RESTART_Y - r, 2 * r, 2 * r);
    if (s->msg_t >= 0 && h->msg[MSG_WELL].a) {
        const ml_mask_t *m = &h->msg[MSG_WELL];
        int cy = ML_H / 2 - (ML_W > ML_H ? 150 : 220);
        ml_dmg_rect(d, ML_W / 2 - m->w / 2 - 30, cy - m->h / 2 - 14, m->w + 60, m->h + 28);
    }
}

static void hint_geom(const ml_hud_t *h, int *x, int *y, int *w, int *hh)
{
    const ml_mask_t *m = &h->msg[MSG_TAP];
    int cy = ML_H - (ML_W > ML_H ? 44 : 70);
    *x = ML_W / 2 - m->w / 2 - 22;
    *y = cy - m->h / 2 - 10;
    *w = m->w + 44;
    *hh = m->h + 20;
}

void ml_hud_hint_box(const ml_hud_t *h, ml_dmg_t *d)
{
    int x, y, w, hh;
    hint_geom(h, &x, &y, &w, &hh);
    ml_dmg_rect(d, x, y, w, hh);
}

void ml_hud_overview(const ml_hud_t *h, ml_img_t *im, int par, float t, bool hint)
{
    if (h->sub.a) ml_hud_mask_centered(im, &h->sub, ML_W / 2, 40, ml_rgb(235, 220, 200), 255);
    if (h->title.a) ml_hud_mask_centered(im, &h->title, ML_W / 2, 84, WHITE, 255);
    if (hint && h->msg[MSG_TAP].a) {
        int a = 150 + (int)(90 * sinf(t * 3.0f));
        const ml_mask_t *m = &h->msg[MSG_TAP];
        int x, y, w, hh;
        hint_geom(h, &x, &y, &w, &hh);
        ml_hud_pill(im, x, y, w, hh, WHITE, 40);
        ml_hud_mask_centered(im, m, ML_W / 2, y + hh / 2, WHITE, a);
    }
    /* the par, small, under the title */
    const ml_mask_t *pm = &h->msg[MSG_PAR];
    if (pm->a && par > 0) {
        int wn = ml_hud_number_w(h, par, false);
        int w = pm->w + 8 + wn;
        int x = ML_W / 2 - w / 2, y = 116;
        ml_mask_draw(im, pm, x, y, ml_rgb(255, 205, 70), 220);
        ml_hud_number(h, im, par, x + pm->w + 8, y + (pm->h - h->sdig[0].h) / 2, false, ml_rgb(255, 205, 70), 220);
    }
}
