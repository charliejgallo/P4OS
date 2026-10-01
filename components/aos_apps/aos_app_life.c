/*
 * P4OS - Vida (from AmoledOS)
 *
 * Two cellular automata on the same grid: Conway's Game of Life and
 * Langton's ant. They are together because they share everything expensive
 * -the grid, the upscaling and the drawing- and differ in twenty lines of
 * rules.
 *
 * The watch had a 92x92 grid; here the grid is whatever the screen leaves
 * after the controls, at 4 px a cell: 180x282 upright, 260x178 lying down
 * (about 50,000 cells either way). A turn of the screen keeps the pattern:
 * the new grid takes the centre of the old one.
 *
 * How it is drawn, which is the only decision that matters here (the
 * watch's, measured there and kept):
 *
 *   - One canvas at 1:1 over a buffer we upscale by hand: one write per
 *     cell and a memcpy per repeated row. Stretching a small canvas with
 *     LV_IMAGE_ALIGN_STRETCH made LVGL allocate an alpha plane and blend,
 *     129 ms a frame on the watch; one lv_obj per cell would be 50,000
 *     objects.
 *   - Each cell is 3x3 with a 1 px seam, so the board reads as a grid of
 *     lights and not as a smear.
 *   - No dirty rectangles: in a cellular automaton half the board changes
 *     per frame, so keeping track would cost more than redrawing.
 *   - The controls are NOT over the canvas (a bar underneath, or a column
 *     on the right lying down): the canvas never invalidates them, and the
 *     counter, which does change, is rewritten three times a second.
 *
 * Touching the board paints: drag a finger and live cells are sprayed along
 * the stroke (the ant is moved there instead). The system's back is the left
 * EDGE only, so painting needs no special flag; "Borrar" empties the board
 * and stops, for drawing a pattern of your own before pressing play.
 *
 * The 1.8 MB canvas buffer and the two grids go through malloc(), that is,
 * to PSRAM. The app is KEEP: leaving it pauses the timer and coming back
 * finds the board as it was.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCALE       4           /* px per cell: 3 of light and 1 of seam */
#define AGE_MAX     7           /* different ages Life colours by        */
#define ANT_STEPS   240         /* ant steps per frame                   */
#define C_HINT      lv_color_hex(0x636366)

typedef enum { MODE_LIFE = 0, MODE_ANT } life_mode_t;

enum { SEED_SOUP = 0, SEED_GUNS, SEED_ACORN, SEED_RPENT, SEED_GLIDERS, SEED_COUNT };
static const char *const SEED_NAME[SEED_COUNT] = {
    N_("Sopa"), N_("Cañones"), N_("Bellota"), N_("R-pentominó"), N_("Planeadores"),
};

/* The frame period, not the number of steps. */
static const uint16_t PERIOD_MS[4] = { 200, 100, 50, 25 };
static const char *const SPEED_NAME[4] = { "x1", "x2", "x4", "x8" };

/* -------------------------------------------------------------------------- */
/* State (survives leaving the app and turning the screen)                     */
/* -------------------------------------------------------------------------- */

static struct {
    bool started;
    int gw, gh;                 /* grid, in cells */
    uint8_t *cell, *next;       /* age per cell (0 = dead); ant: 0/1 */

    life_mode_t mode;
    bool running;
    bool auto_reseed;           /* false after "Borrar": the board is yours */
    int  speed;
    int  seed_kind;

    uint32_t gen, pop;
    uint32_t last_pop, same_pop;

    int ant_x, ant_y, ant_dir;  /* dir: 0 up, 1 right, 2 down, 3 left */
    uint32_t rng;
} S;

static struct {
    lv_obj_t *root, *canvas;
    lv_obj_t *lbl_info, *lbl_seed;
    lv_obj_t *btn_play, *g_play, *l_play;
    lv_obj_t *l_speed, *g_mode, *l_mode;
    lv_timer_t *timer;
    uint16_t *big;              /* canvas buffer, RGB565 */
    int32_t cw, ch;             /* canvas, px */
    int32_t W, H;
    bool land;
    uint32_t last_info_ms;
    int last_gx, last_gy;       /* the stroke being painted */
    aos_app_t *self;            /* the runtime's copy: for KEEP_AWAKE */
} U;

static uint16_t s_pal_life[AGE_MAX + 1], s_pal_ant[2], s_ant_color, s_seam;

static uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 19) & 0x1F) << 11 | ((rgb >> 10) & 0x3F) << 5 | ((rgb >> 3) & 0x1F));
}

static void palettes_init(void)
{
    /* by age: the newborn yellow, the old dark green -where something is
     * happening shows at a glance- */
    static const uint32_t LIFE[AGE_MAX + 1] = {
        0x000000, 0xFFF0A0, 0xFFD60A, 0x8CE07A, 0x30D158, 0x24A048, 0x186C34, 0x0E4622,
    };
    for (int i = 0; i <= AGE_MAX; i++) s_pal_life[i] = rgb565(LIFE[i]);
    s_pal_ant[0] = rgb565(0x000000);
    s_pal_ant[1] = rgb565(0xFF9F0A);
    s_ant_color = rgb565(0xFF453A);
    s_seam = rgb565(0x0C0C0E);
}

/* -------------------------------------------------------------------------- */
/* Grid                                                                        */
/* -------------------------------------------------------------------------- */

static uint32_t rnd(void)
{
    uint32_t x = S.rng;         /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    S.rng = x;
    return x;
}

static inline void put(int x, int y, uint8_t v)
{
    if (x >= 0 && x < S.gw && y >= 0 && y < S.gh) S.cell[(size_t)y * S.gw + x] = v;
}

/* (Re)allocates the grid for gw x gh, keeping the centre of the old one. */
static bool grid_resize(int gw, int gh)
{
    if (S.cell && gw == S.gw && gh == S.gh) return true;
    uint8_t *c = calloc((size_t)gw * gh, 1), *n = malloc((size_t)gw * gh);
    if (!c || !n) { free(c); free(n); return false; }
    if (S.cell && (S.gw > S.gh) != (gw > gh)) {
        /* the screen turned: the board turns with it (Life's rules do not
         * care about a transposition), so it fills the new shape instead
         * of leaving two empty bands */
        uint8_t *t = malloc((size_t)S.gw * S.gh);
        if (t) {
            for (int y = 0; y < S.gh; y++)
                for (int x = 0; x < S.gw; x++) t[(size_t)x * S.gh + y] = S.cell[(size_t)y * S.gw + x];
            free(S.cell);
            S.cell = t;
            int w = S.gw; S.gw = S.gh; S.gh = w;
            int ax = S.ant_x; S.ant_x = S.ant_y; S.ant_y = ax;
            S.ant_dir = (5 - S.ant_dir) & 3;    /* up<->left, right<->down */
        }
    }
    if (S.cell) {
        int dx = (gw - S.gw) / 2, dy = (gh - S.gh) / 2;
        for (int y = 0; y < S.gh; y++) {
            int ny = y + dy;
            if (ny < 0 || ny >= gh) continue;
            for (int x = 0; x < S.gw; x++) {
                int nx = x + dx;
                if (nx >= 0 && nx < gw) c[(size_t)ny * gw + nx] = S.cell[(size_t)y * S.gw + x];
            }
        }
        S.ant_x = LV_CLAMP(0, S.ant_x + dx, gw - 1);
        S.ant_y = LV_CLAMP(0, S.ant_y + dy, gh - 1);
    }
    free(S.cell);
    free(S.next);
    S.cell = c;
    S.next = n;
    S.gw = gw;
    S.gh = gh;
    return true;
}

/* A pattern as rows of text, '#' alive: far more readable than coordinates. */
static void stamp(const char *const *rows, int count, int x0, int y0, bool flip)
{
    for (int r = 0; r < count; r++)
        for (int c = 0; rows[r][c]; c++)
            if (rows[r][c] == '#') put(flip ? x0 - c : x0 + c, y0 + r, 1);
}

static const char *const GUN[9] = {
    "........................#...........",
    "......................#.#...........",
    "............##......##............##",
    "...........#...#....##............##",
    "##........#.....#...##..............",
    "##........#...#.##....#.#...........",
    "..........#.....#.......#...........",
    "...........#...#....................",
    "............##......................",
};

static void seed(void)
{
    const int W = S.gw, H = S.gh;
    memset(S.cell, 0, (size_t)W * H);
    S.gen = 0;
    S.same_pop = 0;
    S.last_pop = 0xFFFFFFFFu;
    S.auto_reseed = true;

    if (S.mode == MODE_ANT) {
        S.ant_x = W / 2;
        S.ant_y = H / 2;
        S.ant_dir = 0;
        return;
    }
    switch (S.seed_kind % SEED_COUNT) {
    case SEED_SOUP:
        for (int i = 0; i < W * H; i++) S.cell[i] = (rnd() % 100) < 30 ? 1 : 0;
        break;
    case SEED_GUNS:
        /* Gosper's glider gun (1970), the first pattern proven to grow
         * without limit. Two of them, mirrored, firing into each other. */
        stamp(GUN, 9, 4, H / 4, false);
        stamp(GUN, 9, W - 5, H / 4 + (H > W ? H / 3 : 12), true);
        break;
    case SEED_ACORN: {
        /* seven cells that take 5206 generations to settle */
        static const char *const A[3] = { ".#.....", "...#...", "##..###" };
        stamp(A, 3, W / 2 - 3, H / 2 - 1, false);
        break;
    }
    case SEED_RPENT: {
        /* five cells, 1103 generations of chaos */
        static const char *const R[3] = { ".##", "##.", ".#." };
        stamp(R, 3, W / 2 - 1, H / 2 - 1, false);
        break;
    }
    default: {
        /* a shower of gliders in the four directions */
        static const char *const G[4][3] = {
            { ".#.", "..#", "###" }, { ".#.", "#..", "###" },
            { "###", "..#", ".#." }, { "###", "#..", ".#." },
        };
        int n = W * H / 700;
        for (int i = 0; i < n; i++)
            stamp(G[rnd() % 4], 3, (int)(rnd() % (unsigned)(W - 3)), (int)(rnd() % (unsigned)(H - 3)), false);
        break;
    }
    }
}

/* -------------------------------------------------------------------------- */
/* Rules                                                                       */
/* -------------------------------------------------------------------------- */

static void step_life(void)
{
    const int W = S.gw, H = S.gh;
    const uint8_t *cell = S.cell;
    uint8_t *next = S.next;
    uint32_t pop = 0;

    for (int y = 0; y < H; y++) {
        /* the edges wrap (a torus): what leaves through the top comes back
         * through the bottom */
        const uint8_t *r0 = cell + (size_t)(y ? y - 1 : H - 1) * W;
        const uint8_t *r1 = cell + (size_t)y * W;
        const uint8_t *r2 = cell + (size_t)(y + 1 < H ? y + 1 : 0) * W;
        uint8_t *out = next + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int xm = x ? x - 1 : W - 1, xp = x + 1 < W ? x + 1 : 0;
            int n = (r0[xm] != 0) + (r0[x] != 0) + (r0[xp] != 0) + (r1[xm] != 0) + (r1[xp] != 0) +
                    (r2[xm] != 0) + (r2[x] != 0) + (r2[xp] != 0);
            uint8_t cur = r1[x], v;
            if (cur) v = (n == 2 || n == 3) ? (uint8_t)(cur < AGE_MAX ? cur + 1 : AGE_MAX) : 0;
            else v = n == 3;
            out[x] = v;
            pop += v != 0;
        }
    }
    uint8_t *t = S.cell;
    S.cell = S.next;
    S.next = t;
    S.gen++;
    S.pop = pop;

    /* a population that does not move for 200 generations is a dead board
     * or one full of still lifes: it reseeds itself, because this is meant
     * to be watched -unless the board is the person's own drawing */
    if (pop == S.last_pop) S.same_pop++;
    else { S.same_pop = 0; S.last_pop = pop; }
    if (S.auto_reseed && (pop == 0 || S.same_pop > 200)) {
        S.seed_kind = (S.seed_kind + 1) % SEED_COUNT;
        seed();
    }
}

static void step_ant(int steps)
{
    static const int DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };
    for (int i = 0; i < steps; i++) {
        size_t idx = (size_t)S.ant_y * S.gw + S.ant_x;
        if (S.cell[idx]) { S.ant_dir = (S.ant_dir + 3) & 3; S.cell[idx] = 0; S.pop--; }
        else { S.ant_dir = (S.ant_dir + 1) & 3; S.cell[idx] = 1; S.pop++; }
        S.ant_x += DX[S.ant_dir];
        S.ant_y += DY[S.ant_dir];
        if (S.ant_x < 0) S.ant_x = S.gw - 1;
        if (S.ant_x >= S.gw) S.ant_x = 0;
        if (S.ant_y < 0) S.ant_y = S.gh - 1;
        if (S.ant_y >= S.gh) S.ant_y = 0;
    }
    S.gen += (uint32_t)steps;
}

static void count_pop(void)
{
    uint32_t pop = 0;
    for (int i = 0; i < S.gw * S.gh; i++) pop += S.cell[i] != 0;
    S.pop = pop;
}

static void step(void)
{
    if (S.mode == MODE_ANT) step_ant(ANT_STEPS);
    else step_life();
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

/* One write per cell into the first row of its 4; rows 2 and 3 are a memcpy
 * of it and row 4 is the seam. */
static void expand(void)
{
    if (!U.big) return;
    const uint16_t *pal = S.mode == MODE_ANT ? s_pal_ant : s_pal_life;
    const int32_t DW = U.cw;
    const uint16_t seam = s_seam;
    for (int y = 0; y < S.gh; y++) {
        uint16_t *row = U.big + (size_t)y * SCALE * DW;
        const uint8_t *src = S.cell + (size_t)y * S.gw;
        for (int x = 0; x < S.gw; x++) {
            uint16_t c = pal[src[x]];
            uint16_t *p = row + x * SCALE;
            p[0] = c; p[1] = c; p[2] = c; p[3] = seam;
        }
        for (int32_t x = S.gw * SCALE; x < DW; x++) row[x] = 0;
        memcpy(row + DW, row, (size_t)DW * 2);
        memcpy(row + 2 * DW, row, (size_t)DW * 2);
        uint16_t *s = row + 3 * DW;
        for (int32_t x = 0; x < DW; x++) s[x] = seam;
    }
    for (int32_t y = S.gh * SCALE; y < U.ch; y++) memset(U.big + (size_t)y * DW, 0, (size_t)DW * 2);

    if (S.mode == MODE_ANT) {
        uint16_t *p = U.big + (size_t)S.ant_y * SCALE * DW + (size_t)S.ant_x * SCALE;
        for (int k = 0; k < SCALE - 1; k++)
            for (int j = 0; j < SCALE - 1; j++) p[(size_t)k * DW + j] = s_ant_color;
    }
    lv_obj_invalidate(U.canvas);
}

static void refresh_info(bool force)
{
    uint32_t now = lv_tick_get();
    if (!U.lbl_info || (!force && now - U.last_info_ms < 330)) return;
    U.last_info_ms = now;
    char b[64];
    snprintf(b, sizeof b, _("Generación %lu  ·  %lu celdas"), (unsigned long)S.gen, (unsigned long)S.pop);
    lv_label_set_text(U.lbl_info, b);
}

static void refresh_controls(void)
{
    if (!U.g_play) return;
    lv_label_set_text(U.g_play, S.running ? AOS_SYM_PAUSE : AOS_SYM_PLAY);
    lv_label_set_text(U.l_play, S.running ? _("Pausa") : _("Seguir"));
    lv_obj_set_style_bg_color(U.btn_play, S.running ? AOS_C_CARD2 : lv_color_hex(0x1F7A3A), 0);
    lv_label_set_text(U.l_speed, SPEED_NAME[S.speed]);
    lv_label_set_text(U.g_mode, S.mode == MODE_ANT ? AOS_SYM_LADYBUG : AOS_SYM_GRID);
    lv_label_set_text(U.l_mode, S.mode == MODE_ANT ? _("Hormiga") : _("Vida"));
    if (U.lbl_seed) {
        lv_label_set_text(U.lbl_seed, S.mode == MODE_ANT ? _("La hormiga de Langton") :
                                      S.auto_reseed ? _(SEED_NAME[S.seed_kind]) : _("Tu dibujo"));
    }
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    if (!S.running) return;
    step();
    expand();
    refresh_info(false);
    refresh_controls();     /* the pattern's name changes when it reseeds */
}

/* -------------------------------------------------------------------------- */
/* Controls                                                                    */
/* -------------------------------------------------------------------------- */

/* The screen stays lit only while it runs: stopped, there is nothing to
 * watch. 'self' is the copy the runtime keeps, so touching its flags works. */
static void apply_keep_awake(void)
{
    if (!U.self) return;
    if (S.running) U.self->desc.flags |= AOS_APP_FLAG_KEEP_AWAKE;
    else U.self->desc.flags &= ~(uint32_t)AOS_APP_FLAG_KEEP_AWAKE;
}

static void set_running(bool on)
{
    S.running = on;
    apply_keep_awake();
    refresh_controls();
}

static void play_cb(lv_event_t *e) { (void)e; aos_hal_activity(); set_running(!S.running); }

static void step_cb(lv_event_t *e)
{
    (void)e;
    aos_hal_activity();
    set_running(false);
    step();
    expand();
    refresh_info(true);
}

static void speed_cb(lv_event_t *e)
{
    (void)e;
    S.speed = (S.speed + 1) % 4;
    if (U.timer) lv_timer_set_period(U.timer, PERIOD_MS[S.speed]);
    aos_hal_pref_set_i32("life_speed", S.speed);
    refresh_controls();
}

static void mode_cb(lv_event_t *e)
{
    (void)e;
    S.mode = S.mode == MODE_LIFE ? MODE_ANT : MODE_LIFE;
    aos_hal_pref_set_i32("life_mode", (int32_t)S.mode);
    seed();
    count_pop();
    set_running(true);
    expand();
    refresh_info(true);
}

static void seed_cb(lv_event_t *e)
{
    (void)e;
    if (S.mode == MODE_LIFE) S.seed_kind = (S.seed_kind + 1) % SEED_COUNT;
    seed();
    count_pop();
    set_running(true);
    expand();
    refresh_info(true);
    aos_hal_beep(1300, 20);
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    memset(S.cell, 0, (size_t)S.gw * S.gh);
    S.gen = S.pop = 0;
    S.auto_reseed = false;
    set_running(false);
    expand();
    refresh_info(true);
    aos_ui_toast(_("Dibujá con el dedo y tocá Seguir"), 1800);
}

/* Paints around (gx, gy): a spray of live cells, newborn so they show in
 * yellow; in the ant, the ant goes there. */
static void paint_at(int gx, int gy)
{
    if (S.mode == MODE_ANT) {
        S.ant_x = LV_CLAMP(0, gx, S.gw - 1);
        S.ant_y = LV_CLAMP(0, gy, S.gh - 1);
        return;
    }
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dx * dx + dy * dy <= 5 && (rnd() & 3) != 0) put(gx + dx, gy + dy, 1);
}

static void canvas_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(U.canvas, &a);
    int gx = (p.x - a.x1) / SCALE, gy = (p.y - a.y1) / SCALE;

    if (code == LV_EVENT_PRESSED) {
        U.last_gx = gx;
        U.last_gy = gy;
        if (S.auto_reseed && S.mode == MODE_LIFE && !S.running) S.auto_reseed = false;
    }
    /* along the stroke, not only where each read landed: a fast finger
     * moves several cells between two reads */
    int dx = gx - U.last_gx, dy = gy - U.last_gy;
    int n = LV_MAX(LV_ABS(dx), LV_ABS(dy)) / 2 + 1;
    for (int i = 1; i <= n; i++) paint_at(U.last_gx + dx * i / n, U.last_gy + dy * i / n);
    U.last_gx = gx;
    U.last_gy = gy;
    count_pop();
    expand();
    refresh_info(true);
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *ctl(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg,
                     int32_t w, int32_t h, lv_event_cb_t cb, lv_obj_t **g_out, lv_obj_t **l_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *g = aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_align(g, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *l = aos_label(b, text, aos_font_tiny, AOS_C_DIM);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 28);
    aos_make_decorative(g);
    aos_make_decorative(l);
    if (g_out) *g_out = g;
    if (l_out) *l_out = l;
    return b;
}

static void build_controls(lv_obj_t *bar, int cols, int32_t bw, int32_t bh, int32_t gap, int32_t x0, int32_t y0)
{
    struct { const char *g, *t; lv_event_cb_t cb; } C[6] = {
        { AOS_SYM_PAUSE, "", play_cb }, { AOS_SYM_SKIP_NEXT, N_("Paso"), step_cb },
        { AOS_SYM_SPEEDOMETER, "", speed_cb }, { AOS_SYM_GRID, "", mode_cb },
        { AOS_SYM_SHAPE_OUTLINE, N_("Patrón"), seed_cb }, { AOS_SYM_ERASER, N_("Borrar"), clear_cb },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *g = NULL, *l = NULL;
        lv_obj_t *b = ctl(bar, C[i].g, C[i].t[0] ? _(C[i].t) : "", i == 4 ? lv_color_hex(0x7A4A0A) : AOS_C_CARD2,
                          bw, bh, C[i].cb, &g, &l);
        lv_obj_set_pos(b, x0 + (i % cols) * (bw + gap), y0 + (i / cols) * (bh + gap));
        if (i == 0) { U.btn_play = b; U.g_play = g; U.l_play = l; }
        if (i == 2) U.l_speed = l;
        if (i == 3) { U.g_mode = g; U.l_mode = l; }
    }
}

static bool build(lv_obj_t *root)
{
    if (U.timer) { lv_timer_delete(U.timer); U.timer = NULL; }
    lv_obj_clean(root);
    free(U.big);
    aos_app_t *self = U.self;
    memset(&U, 0, sizeof U);
    U.self = self;
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;

    /* the controls take a strip (upright) or a column (lying down); the
     * canvas gets the rest, in whole cells */
    const int32_t bar = U.land ? 252 : 176;
    U.cw = (U.land ? U.W - bar : U.W) / SCALE * SCALE;
    U.ch = (U.land ? U.H : U.H - bar) / SCALE * SCALE;
    bool fresh = !S.cell;
    U.big = malloc((size_t)U.cw * U.ch * 2);
    if (!U.big || !grid_resize(U.cw / SCALE, U.ch / SCALE)) {
        free(U.big);
        U.big = NULL;
        aos_ui_toast(_("Sin memoria"), 2000);
        return false;
    }
    if (fresh) { seed(); count_pop(); }

    lv_obj_t *page = aos_page(root);
    U.canvas = lv_canvas_create(page);
    lv_canvas_set_buffer(U.canvas, U.big, U.cw, U.ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(U.canvas, U.cw, U.ch);
    lv_obj_set_pos(U.canvas, U.land ? 0 : (U.W - U.cw) / 2, U.land ? (U.H - U.ch) / 2 : 0);
    /* the theme gives everything a radius: on a canvas that means a mask */
    lv_obj_set_style_radius(U.canvas, 0, 0);
    lv_image_set_antialias(U.canvas, false);
    lv_obj_add_flag(U.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(U.canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(U.canvas, canvas_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(U.canvas, canvas_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_t *b = lv_obj_create(page);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x111114), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    if (!U.land) {
        lv_obj_set_size(b, U.W, U.H - U.ch);
        lv_obj_set_pos(b, 0, U.ch);
        U.lbl_seed = aos_label(b, "", aos_font_caption, AOS_C_TEXT);
        lv_obj_set_pos(U.lbl_seed, AOS_UI_PAD, 12);
        U.lbl_info = aos_label(b, "", aos_font_caption, AOS_C_DIM);
        lv_obj_align(U.lbl_info, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 12);
        const int32_t gap = 12, bw = (U.W - 2 * AOS_UI_PAD - 5 * gap) / 6;
        build_controls(b, 6, bw, 104, gap, AOS_UI_PAD, 48);
    } else {
        lv_obj_set_size(b, U.W - U.cw, U.H);
        lv_obj_set_pos(b, U.cw, 0);
        const int32_t pad = 20, gap = 12;
        const int32_t bw = (U.W - U.cw - 2 * pad - gap) / 2;
        U.lbl_seed = aos_label(b, "", aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(U.lbl_seed, U.W - U.cw - 2 * pad);
        lv_label_set_long_mode(U.lbl_seed, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(U.lbl_seed, pad, 24);
        U.lbl_info = aos_label(b, "", aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(U.lbl_info, U.W - U.cw - 2 * pad);
        lv_label_set_long_mode(U.lbl_info, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(U.lbl_info, pad, 64);
        const int32_t bh = 124;
        const int32_t by = U.H - pad - 3 * bh - 2 * gap;
        build_controls(b, 2, bw, bh, gap, pad, by);

        /* the key to the colours, in the room left above the buttons */
        lv_obj_t *k = aos_label(b, _("Edad de las celdas"), aos_font_tiny, AOS_C_DIM);
        lv_obj_set_pos(k, pad, by - 112);
        const int32_t sw = (U.W - U.cw - 2 * pad) / AGE_MAX;
        static const uint32_t AGE_RGB[AGE_MAX] = { 0xFFF0A0, 0xFFD60A, 0x8CE07A, 0x30D158, 0x24A048, 0x186C34, 0x0E4622 };
        for (int i = 0; i < AGE_MAX; i++) {
            lv_obj_t *sq = lv_obj_create(b);
            lv_obj_remove_style_all(sq);
            lv_obj_set_size(sq, sw - 4, 22);
            lv_obj_set_pos(sq, pad + i * sw, by - 84);
            lv_obj_set_style_radius(sq, 5, 0);
            lv_obj_set_style_bg_color(sq, lv_color_hex(AGE_RGB[i]), 0);
            lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
            aos_make_decorative(sq);
        }
        lv_obj_t *l1 = aos_label(b, _("recién nacida"), aos_font_tiny, AOS_C_DIM);
        lv_obj_set_pos(l1, pad, by - 56);
        lv_obj_t *l2 = aos_label(b, _("vieja"), aos_font_tiny, AOS_C_DIM);
        lv_obj_align(l2, LV_ALIGN_TOP_RIGHT, -pad, by - 56);
        lv_obj_t *h = aos_label(b, _("Arrastrá el dedo sobre el tablero para dibujar."), aos_font_tiny, C_HINT);
        lv_obj_set_width(h, U.W - U.cw - 2 * pad);
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(h, pad, 128);
        aos_make_decorative(k);
        aos_make_decorative(l1);
        aos_make_decorative(l2);
        aos_make_decorative(h);
    }

    expand();
    refresh_info(true);
    refresh_controls();
    U.timer = lv_timer_create(frame_cb, PERIOD_MS[S.speed], NULL);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    palettes_init();
    if (!S.started) {
        S.started = true;
        S.rng = (uint32_t)aos_hal_uptime_ms() | 1u;
        int32_t v = 0;
        S.speed = aos_hal_pref_get_i32("life_speed", &v) && v >= 0 && v < 4 ? (int)v : 1;
        if (aos_hal_pref_get_i32("life_mode", &v) && (v == MODE_LIFE || v == MODE_ANT)) S.mode = (life_mode_t)v;
        S.running = true;
    }
    U.self = self;
    if (!build(root)) return NULL;
    apply_keep_awake();
    return &U;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self; (void)inst;
    return build(root);
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (U.timer) lv_timer_delete(U.timer);
    /* the objects first: the canvas points at a buffer about to be freed */
    if (self && self->root) lv_obj_clean(self->root);
    free(U.big);
    memset(&U, 0, sizeof U);
    /* closed for good (the switcher): the board goes too, 50,000 cells */
    free(S.cell);
    free(S.next);
    S.cell = S.next = NULL;
    S.gw = S.gh = 0;
    S.started = false;
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_pause(U.timer);
}

static void show(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_resume(U.timer);
}

void aos_app_life_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id       = "aos.life",
            .name     = "Vida",
            .icon     = AOS_SYM_GRID,
            .icon_vec = AOS_ICON_LIFE,
            .color_a  = 0x30D158,
            .color_b  = 0x0E4622,
            .flags    = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP,
            .order    = 152,
        },
        .create  = create,
        .destroy = destroy,
        .show    = show,
        .hide    = hide,
        .resize  = resize,
    };
}
