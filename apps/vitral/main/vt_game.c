/*
 * VITRAL - the rules (see vt_game.h)
 */
#include "vt_game.h"

#include <string.h>

/* marks the puzzles' names for the catalogue (the app translates them) */
#ifndef N_
#define N_(s) (s)
#endif

vt_shape_t vt_shapes[PC_COUNT];

/* Each piece in a 5x5 box, centred on (2,2), which is what it turns about. */
static const struct {
    uint8_t n;
    vt_pt_t c[5];
} BASE[PC_COUNT] = {
    [PC_I3] = { 3, { {1,2}, {2,2}, {3,2} } },
    [PC_V3] = { 3, { {2,1}, {2,2}, {3,2} } },
    [PC_I5] = { 5, { {0,2}, {1,2}, {2,2}, {3,2}, {4,2} } },
    [PC_L]  = { 5, { {2,0}, {2,1}, {2,2}, {2,3}, {3,3} } },
    [PC_J]  = { 5, { {2,0}, {2,1}, {2,2}, {2,3}, {1,3} } },
    [PC_P]  = { 5, { {2,1}, {3,1}, {2,2}, {3,2}, {2,3} } },
    [PC_Q]  = { 5, { {1,1}, {2,1}, {1,2}, {2,2}, {2,3} } },
    [PC_T]  = { 5, { {1,1}, {2,1}, {3,1}, {2,2}, {2,3} } },
    [PC_U]  = { 5, { {1,1}, {3,1}, {1,2}, {2,2}, {3,2} } },
    [PC_V]  = { 5, { {1,1}, {1,2}, {1,3}, {2,3}, {3,3} } },
    [PC_W]  = { 5, { {1,1}, {1,2}, {2,2}, {2,3}, {3,3} } },
    [PC_X]  = { 5, { {2,1}, {1,2}, {2,2}, {3,2}, {2,3} } },
    [PC_Y]  = { 5, { {2,0}, {2,1}, {2,2}, {2,3}, {3,1} } },
    [PC_Y2] = { 5, { {2,0}, {2,1}, {2,2}, {2,3}, {1,1} } },
    [PC_N]  = { 5, { {2,0}, {2,1}, {2,2}, {1,2}, {1,3} } },
    [PC_N2] = { 5, { {1,0}, {1,1}, {1,2}, {2,2}, {2,3} } },
    [PC_Z]  = { 5, { {1,1}, {2,1}, {2,2}, {2,3}, {3,3} } },
    [PC_S]  = { 5, { {3,1}, {2,1}, {2,2}, {2,3}, {1,3} } },
    [PC_F]  = { 5, { {2,1}, {3,1}, {1,2}, {2,2}, {2,3} } },
    [PC_F2] = { 5, { {1,1}, {2,1}, {2,2}, {3,2}, {2,3} } },
};

static bool s_shapes_ready;

static void shapes_init(void)
{
    if (s_shapes_ready) return;
    for (int t = 0; t < PC_COUNT; t++) {
        vt_shape_t *s = &vt_shapes[t];
        s->n = BASE[t].n;
        for (int i = 0; i < s->n; i++) s->c[0][i] = BASE[t].c[i];
        for (int r = 1; r < 4; r++) {
            for (int i = 0; i < s->n; i++) {
                /* clockwise with y down: right of centre goes below it */
                s->c[r][i].x = (int8_t)(4 - s->c[r - 1][i].y);
                s->c[r][i].y = s->c[r - 1][i].x;
            }
        }
    }
    s_shapes_ready = true;
}

static uint32_t rnd(vt_game_t *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x ? x : 0x9E3779B9u;
    return g->rng;
}

static int rnd_n(vt_game_t *g, int n)
{
    return (int)(rnd(g) % (uint32_t)n);
}

/* -------------------------------------------------------------------------- */
/* Gravity                                                                     */

uint32_t vt_gravity_ms(int level)
{
    if (level < 1) level = 1;
    float ms = 820.0f;
    for (int i = 1; i < level && i < 40; i++) ms *= 0.83f;
    if (ms < 16.0f) ms = 16.0f;
    return (uint32_t)ms;
}

static uint32_t period(const vt_game_t *g)
{
    uint32_t p = vt_gravity_ms(g->level);
    if (g->soft && p > VT_SOFT_MS) p = VT_SOFT_MS;
    return p;
}

/* -------------------------------------------------------------------------- */
/* Pieces in, cells                                                            */

bool vt_fits(const vt_game_t *g, int type, int rot, int x, int y)
{
    const vt_shape_t *s = &vt_shapes[type];
    for (int i = 0; i < s->n; i++) {
        int cx = x + s->c[rot & 3][i].x, cy = y + s->c[rot & 3][i].y;
        if (cx < 0 || cx >= g->cols || cy >= VT_ROWS) return false;
        if (cy < 0) continue;
        if (g->col[cy][cx] != VT_EMPTY) return false;
    }
    return true;
}

static bool cur_fits(const vt_game_t *g, int dx, int dy, int rot)
{
    return vt_fits(g, g->cur.type, rot, g->cur.x + dx, g->cur.y + dy);
}

/* -------------------------------------------------------------------------- */
/* The bag                                                                     */

/* Which pieces a level of the marathon draws from: the friendlier
 * pentominoes first, the awkward ones later. Each tier adds to the last. */
static const uint8_t TIER0[] = { PC_P, PC_Q, PC_L, PC_J, PC_I5, PC_T };
static const uint8_t TIER1[] = { PC_U, PC_Y, PC_Y2, PC_N, PC_N2, PC_V };
static const uint8_t TIER2[] = { PC_Z, PC_S, PC_F, PC_F2, PC_W };
static const uint8_t TIER3[] = { PC_X };

static int tier_of(const vt_game_t *g)
{
    if (g->mode == VT_MODE_VERSUS || g->mode == VT_MODE_TIME) {
        return g->level >= 4 ? 3 : 2;
    }
    if (g->level >= 7) return 3;
    if (g->level >= 5) return 2;
    if (g->level >= 3) return 1;
    return 0;
}

static void bag_fill(vt_game_t *g)
{
    int n = 0, tier = tier_of(g);
    for (unsigned i = 0; i < sizeof TIER0; i++) g->bag[n++] = TIER0[i];
    if (tier >= 1) for (unsigned i = 0; i < sizeof TIER1; i++) g->bag[n++] = TIER1[i];
    if (tier >= 2) for (unsigned i = 0; i < sizeof TIER2; i++) g->bag[n++] = TIER2[i];
    if (tier >= 3) for (unsigned i = 0; i < sizeof TIER3; i++) g->bag[n++] = TIER3[i];
    /* the small ones are the breathers: a quarter of the bag or so */
    int small = tier == 0 ? 2 : 3;
    for (int k = 0; k < small; k++) {
        g->bag[n++] = PC_I3;
        g->bag[n++] = PC_V3;
    }
    for (int i = n - 1; i > 0; i--) {
        int j = rnd_n(g, i + 1);
        uint8_t t = g->bag[i];
        g->bag[i] = g->bag[j];
        g->bag[j] = t;
    }
    g->bag_n = (uint8_t)n;
    g->bag_i = 0;
}

static vt_next_t draw_next(vt_game_t *g)
{
    vt_next_t nx;
    if (g->mode == VT_MODE_PUZZLE) {
        if (g->pz_i + VT_QUEUE < g->pz_n) {
            return g->pz_seq[g->pz_i + VT_QUEUE];
        }
        nx.type = 0xFF;
        nx.color = 0;
        return nx;
    }
    if (g->bag_i >= g->bag_n) bag_fill(g);
    nx.type = g->bag[g->bag_i++];
    /* one in sixteen is clear glass, which matches any colour */
    nx.color = (rnd(g) & 15) == 0 ? VT_CLEAR : (uint8_t)(1 + rnd_n(g, VT_NCOLORS));
    return nx;
}

static void queue_fill(vt_game_t *g)
{
    for (int i = 0; i < VT_QUEUE; i++) g->queue[i] = draw_next(g);
}

static int max_y(int type, int rot)
{
    int m = 0;
    const vt_shape_t *s = &vt_shapes[type];
    for (int i = 0; i < s->n; i++) if (s->c[rot][i].y > m) m = s->c[rot][i].y;
    return m;
}

static int spawn_x(const vt_game_t *g)
{
    if (g->mode == VT_MODE_PUZZLE) return (g->pz_x0 + g->pz_x1 + 1) / 2 - 2;
    return (g->cols - 5) / 2;
}

/* Puts a piece at the top. false = there is no room: the game is over. */
static bool place_new(vt_game_t *g, vt_next_t nx)
{
    g->cur.type = nx.type;
    g->cur.color = nx.color;
    g->cur.rot = 0;
    g->cur.x = (int8_t)spawn_x(g);
    g->cur.y = (int8_t)(VT_HIDDEN + 1 - max_y(nx.type, 0));
    g->cur.pid = ++g->next_pid ? g->next_pid : ++g->next_pid;
    g->grav_acc = 0;
    g->lock_acc = 0;
    g->lock_resets = 0;
    g->lowest = g->cur.y;
    g->has_cur = true;
    if (cur_fits(g, 0, 0, 0)) return true;
    g->cur.y--;
    if (cur_fits(g, 0, 0, 0)) return true;
    g->has_cur = false;
    return false;
}

static void game_over(vt_game_t *g)
{
    g->state = VT_OVER;
    g->has_cur = false;
    g->ev |= EV_OVER;
}

static void spawn(vt_game_t *g)
{
    vt_next_t nx;
    if (g->mode == VT_MODE_PUZZLE) {
        if (g->pz_i >= g->pz_n) {
            g->state = VT_STUCK;
            g->has_cur = false;
            g->ev |= EV_STUCK;
            return;
        }
        nx = g->pz_seq[g->pz_i];
        for (int i = 0; i < VT_QUEUE - 1; i++) g->queue[i] = g->queue[i + 1];
        g->queue[VT_QUEUE - 1] = draw_next(g);
    } else {
        nx = g->queue[0];
        for (int i = 0; i < VT_QUEUE - 1; i++) g->queue[i] = g->queue[i + 1];
        g->queue[VT_QUEUE - 1] = draw_next(g);
    }
    if (!place_new(g, nx)) {
        game_over(g);
        return;
    }
    g->ev |= EV_SPAWN;
}

/* -------------------------------------------------------------------------- */

void vt_events_clear(vt_game_t *g)
{
    /* fx_rows stay: the rows break a few frames after they were found */
    g->ev = 0;
    g->fx_lock_n = 0;
}

void vt_init(vt_game_t *g, vt_mode_t mode, uint32_t seed, int level)
{
    shapes_init();
    uint8_t *undo = g->undo;
    memset(g, 0, sizeof *g);
    g->undo = undo;
    g->mode = mode;
    g->cols = VT_COLS_MAX;
    g->rng = seed ? seed : 0x2545F491u;
    g->gseed = seed ^ 0xA5A5F00Du;
    if (level < 1) level = 1;
    g->level = g->start_level = (uint8_t)level;
    g->hold.type = 0xFF;
    g->state = VT_PLAY;
    if (mode == VT_MODE_TIME) g->time_limit = VT_TIME_LIMIT;
    if (mode != VT_MODE_PUZZLE) {
        queue_fill(g);
        spawn(g);
    }
}

/* -------------------------------------------------------------------------- */
/* Moving                                                                      */

static void moved_resting(vt_game_t *g)
{
    /* a piece that moves while resting gets its lock time back, a few times */
    if (!cur_fits(g, 0, 1, g->cur.rot) && g->lock_resets < VT_LOCK_RESETS) {
        g->lock_acc = 0;
        g->lock_resets++;
    }
}

bool vt_move(vt_game_t *g, int dx)
{
    if (g->state != VT_PLAY || !g->has_cur || !dx) return false;
    if (!cur_fits(g, dx, 0, g->cur.rot)) return false;
    g->cur.x = (int8_t)(g->cur.x + dx);
    g->ev |= EV_MOVE;
    moved_resting(g);
    return true;
}

bool vt_rotate(vt_game_t *g, int dir)
{
    if (g->state != VT_PLAY || !g->has_cur) return false;
    int nr = (g->cur.rot + (dir > 0 ? 1 : 3)) & 3;
    /* where to try it: in place, a step aside, up a step (off the floor),
     * two aside, and up beside */
    static const int8_t K[][2] = {
        { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { -2, 0 }, { 2, 0 },
        { -1, -1 }, { 1, -1 }, { 0, -2 }, { 0, 1 },
    };
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
        int dx = dir > 0 ? K[i][0] : -K[i][0];
        if (cur_fits(g, dx, K[i][1], nr)) {
            g->cur.x = (int8_t)(g->cur.x + dx);
            g->cur.y = (int8_t)(g->cur.y + K[i][1]);
            g->cur.rot = (uint8_t)nr;
            g->ev |= EV_ROTATE;
            moved_resting(g);
            return true;
        }
    }
    g->ev |= EV_KICKFAIL;
    return false;
}

static bool step_down(vt_game_t *g)
{
    if (!cur_fits(g, 0, 1, g->cur.rot)) return false;
    g->cur.y++;
    if (g->cur.y > g->lowest) {
        g->lowest = g->cur.y;
        g->lock_resets = 0;
    }
    g->lock_acc = 0;
    return true;
}

bool vt_down(vt_game_t *g)
{
    if (g->state != VT_PLAY || !g->has_cur) return false;
    if (!step_down(g)) return false;
    g->grav_acc = 0;
    if (g->mode != VT_MODE_PUZZLE) g->score += 1;
    g->ev |= EV_SOFT;
    return true;
}

void vt_soft(vt_game_t *g, bool on)
{
    g->soft = on;
}

int vt_ghost_y(const vt_game_t *g)
{
    int y = g->cur.y;
    while (vt_fits(g, g->cur.type, g->cur.rot, g->cur.x, y + 1)) y++;
    return y;
}

int vt_fall_frac(const vt_game_t *g)
{
    if (!g->has_cur || g->state != VT_PLAY) return 0;
    if (!cur_fits(g, 0, 1, g->cur.rot)) return 0;
    uint32_t p = period(g);
    if (p <= 20) return 0;              /* faster than the frames: no in-between */
    uint32_t f = g->grav_acc * 256u / p;
    return f > 255 ? 255 : (int)f;
}

/* -------------------------------------------------------------------------- */
/* Rows                                                                        */

static bool row_full(const vt_game_t *g, int y)
{
    for (int x = 0; x < g->cols; x++) if (g->col[y][x] == VT_EMPTY) return false;
    return true;
}

static bool row_empty(const vt_game_t *g, int y)
{
    for (int x = 0; x < g->cols; x++) if (g->col[y][x] != VT_EMPTY) return false;
    return true;
}

/* All the glass of one colour (clear glass goes with any; a row of only
 * clear glass counts too). Rubble and stone never make a vitral. */
static bool row_one_colour(const vt_game_t *g, int y)
{
    int c = 0;
    for (int x = 0; x < g->cols; x++) {
        int v = g->col[y][x];
        if (v == VT_CLEAR) continue;
        if (v < VT_RUBY || v > VT_SAPPHIRE) return false;
        if (!c) c = v;
        else if (v != c) return false;
    }
    return true;
}

static void remove_rows(vt_game_t *g)
{
    /* top to bottom, each one pulls everything above it down one row */
    uint8_t rows[8];
    int n = g->clear_n;
    memcpy(rows, g->clear_row, sizeof rows);
    /* sort ascending (top first) */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && rows[j - 1] > rows[j]; j--) {
            uint8_t t = rows[j];
            rows[j] = rows[j - 1];
            rows[j - 1] = t;
        }
    }
    for (int i = 0; i < n; i++) {
        int r = rows[i];
        for (int y = r; y > 0; y--) {
            memcpy(g->col[y], g->col[y - 1], sizeof g->col[0]);
            memcpy(g->pid[y], g->pid[y - 1], sizeof g->pid[0]);
        }
        memset(g->col[0], 0, sizeof g->col[0]);
        memset(g->pid[0], 0, sizeof g->pid[0]);
    }
}

static const uint16_t LINE_PTS[9] = { 0, 100, 300, 500, 800, 1200, 1600, 2000, 2500 };

/* After a lock: which rows go, with the vitrals and the rows they take. */
static int find_clears(vt_game_t *g)
{
    int n = 0;
    uint8_t take[VT_ROWS];
    memset(take, 0, sizeof take);
    for (int y = 0; y < VT_ROWS; y++) {
        if (!row_full(g, y)) continue;
        take[y] = row_one_colour(g, y) ? 1 : 3;
    }
    /* a vitral takes the row under it, if that one is not going anyway
     * (then the next one down that is not) and has glass in it */
    for (int y = VT_ROWS - 1; y >= 0; y--) {
        if (take[y] != 1) continue;
        for (int b = y + 1; b < VT_ROWS; b++) {
            if (take[b]) continue;
            if (!row_empty(g, b)) take[b] = 2;
            break;
        }
    }
    for (int y = 0; y < VT_ROWS && n < 8; y++) {
        if (!take[y]) continue;
        g->clear_row[n] = (uint8_t)y;
        g->clear_vit[n] = take[y] == 3 ? 0 : take[y];
        n++;
    }
    return n;
}

static void apply_garbage(vt_game_t *g)
{
    int k = g->garbage_in;
    if (!k) return;
    g->garbage_in = 0;
    if (k > 10) k = 10;
    for (int y = 0; y < k; y++) {
        if (!row_empty(g, y)) {
            /* pushed out of the top */
            for (int yy = 0; yy + k < VT_ROWS; yy++) {
                memcpy(g->col[yy], g->col[yy + k], sizeof g->col[0]);
                memcpy(g->pid[yy], g->pid[yy + k], sizeof g->pid[0]);
            }
            game_over(g);
            g->ev |= EV_BOARD;
            return;
        }
    }
    for (int y = 0; y + k < VT_ROWS; y++) {
        memcpy(g->col[y], g->col[y + k], sizeof g->col[0]);
        memcpy(g->pid[y], g->pid[y + k], sizeof g->pid[0]);
    }
    /* the rival's seed decides the hole: the same rubble the same way */
    uint32_t s = g->gseed;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    g->gseed = s;
    int hole = (int)(s % g->cols);
    for (int y = VT_ROWS - k; y < VT_ROWS; y++) {
        for (int x = 0; x < g->cols; x++) {
            g->col[y][x] = x == hole ? VT_EMPTY : VT_FROST;
            g->pid[y][x] = x == hole ? 0 : (uint16_t)(0xF000u + (unsigned)(y * VT_COLS_MAX + x));
        }
    }
    g->ev |= EV_GARBAGE | EV_BOARD;
}

void vt_add_garbage(vt_game_t *g, int rows)
{
    int k = g->garbage_in + rows;
    g->garbage_in = (uint8_t)(k > 20 ? 20 : k);
}

/* -------------------------------------------------------------------------- */
/* Puzzle helpers                                                              */

size_t vt_pz_undo_size(void)
{
    return sizeof(((vt_game_t *)0)->col) + sizeof(((vt_game_t *)0)->pid);
}

static void pz_snapshot(vt_game_t *g)
{
    if (!g->undo || g->undo_n >= VT_PZ_MAX_SEQ) return;
    uint8_t *p = g->undo + (size_t)g->undo_n * vt_pz_undo_size();
    memcpy(p, g->col, sizeof g->col);
    memcpy(p + sizeof g->col, g->pid, sizeof g->pid);
    g->undo_n++;
}

static bool pz_complete(const vt_game_t *g)
{
    for (int y = 0; y < VT_ROWS; y++)
        for (int x = 0; x < g->cols; x++)
            if (g->mask[y][x] && g->col[y][x] == VT_EMPTY) return false;
    return true;
}

bool vt_pz_undo(vt_game_t *g)
{
    if (g->mode != VT_MODE_PUZZLE || !g->undo || !g->undo_n) return false;
    if (g->state == VT_WON || g->state == VT_CLEARING) return false;
    g->undo_n--;
    uint8_t *p = g->undo + (size_t)g->undo_n * vt_pz_undo_size();
    memcpy(g->col, p, sizeof g->col);
    memcpy(g->pid, p + sizeof g->col, sizeof g->pid);
    g->pz_i = g->undo_n;
    for (int i = 0; i < VT_QUEUE; i++) {
        int k = g->pz_i + 1 + i;
        if (k < g->pz_n) g->queue[i] = g->pz_seq[k];
        else g->queue[i].type = 0xFF;
    }
    g->state = VT_PLAY;
    place_new(g, g->pz_seq[g->pz_i]);
    g->ev |= EV_UNDO | EV_BOARD | EV_SPAWN;
    return true;
}

uint32_t vt_pz_par(const vt_game_t *g)
{
    return (uint32_t)g->pz_n * 6000u;
}

/* -------------------------------------------------------------------------- */
/* Locking                                                                     */

static void lock_piece(vt_game_t *g)
{
    const vt_shape_t *s = &vt_shapes[g->cur.type];
    const vt_pt_t *c = s->c[g->cur.rot];

    if (g->mode == VT_MODE_PUZZLE) {
        bool inside = true;
        for (int i = 0; i < s->n; i++) {
            int x = g->cur.x + c[i].x, y = g->cur.y + c[i].y;
            if (y < 0 || !g->mask[y][x]) inside = false;
        }
        if (!inside) {
            /* it does not go there: back to the top, the same piece */
            g->fx_piece = g->cur;
            g->ev |= EV_REJECT;
            vt_next_t nx = { g->cur.type, g->cur.color };
            uint16_t pid = g->cur.pid;
            place_new(g, nx);
            g->cur.pid = pid;
            return;
        }
        pz_snapshot(g);
    }

    bool above = true;
    g->fx_lock_n = 0;
    for (int i = 0; i < s->n; i++) {
        int x = g->cur.x + c[i].x, y = g->cur.y + c[i].y;
        if (y >= VT_HIDDEN) above = false;
        if (y < 0) continue;
        g->col[y][x] = g->cur.color;
        g->pid[y][x] = g->cur.pid;
        g->fx_lock[g->fx_lock_n].x = (int8_t)x;
        g->fx_lock[g->fx_lock_n].y = (int8_t)y;
        g->fx_lock_n++;
    }
    g->fx_lock_color = g->cur.color;
    g->fx_piece = g->cur;
    g->has_cur = false;
    g->hold_used = false;
    g->ev |= EV_LOCK | EV_BOARD;

    if (above) {
        game_over(g);
        return;
    }

    if (g->mode == VT_MODE_PUZZLE) {
        g->pz_i++;
        if (pz_complete(g)) {
            g->state = VT_WON;
            g->ev |= EV_WON;
            return;
        }
        spawn(g);
        return;
    }

    int n = find_clears(g);
    if (!n) {
        g->combo = 0;
        apply_garbage(g);
        if (g->state == VT_PLAY) spawn(g);
        return;
    }

    /* the rows flash first; they break at VT_CLEAR_FLASH_MS */
    g->clear_n = (uint8_t)n;
    g->clear_t = 0;
    g->state = VT_CLEARING;
    int vit = 0, full = 0;
    g->fx_rows_n = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        int y = g->clear_row[i];
        g->fx_rows[i] = (uint8_t)y;
        g->fx_rows_vit[i] = g->clear_vit[i];
        memcpy(g->fx_rows_col[i], g->col[y], sizeof g->col[0]);
        memcpy(g->fx_rows_pid[i], g->pid[y], sizeof g->pid[0]);
        if (g->clear_vit[i] == 1) vit++;
        if (g->clear_vit[i] != 2) full++;
    }
    g->ev |= EV_CLEAR;
    if (vit) g->ev |= EV_VITRAL;

    /* points: the rows by how many went at once, a vitral's share three
     * times, and a little more for each clear in a row */
    uint32_t base = LINE_PTS[n > 8 ? 8 : n];
    uint32_t pts = base + (uint32_t)vit * 2u * base / (uint32_t)n;
    pts += 50u * g->combo;
    pts *= g->level;
    g->score += pts;
    g->fx_points = pts;
    g->fx_cleared = (uint8_t)n;
    g->combo++;
    g->vitrals = (uint16_t)(g->vitrals + vit);

    int old_level = g->level;
    g->lines = (uint16_t)(g->lines + n);
    if (g->mode == VT_MODE_MARATHON || g->mode == VT_MODE_TIME) {
        int lv = g->start_level + g->lines / 10;
        if (lv > 30) lv = 30;
        g->level = (uint8_t)lv;
        /* a new tier of pieces comes in with the next bag */
        if (g->level != old_level) g->ev |= EV_LEVEL;
    }
    if (g->mode == VT_MODE_VERSUS) {
        /* rows sent: one less than cleared, plus one per vitral; what comes
         * in is cancelled first */
        int send = (full > 1 ? full - 1 : 0) + vit;
        if (full >= 5) send++;
        int cancel = send < g->garbage_in ? send : g->garbage_in;
        g->garbage_in = (uint8_t)(g->garbage_in - cancel);
        send -= cancel;
        g->garbage_out = (uint8_t)(g->garbage_out + send);
    }
}

/* -------------------------------------------------------------------------- */

void vt_hard(vt_game_t *g)
{
    if (g->state != VT_PLAY || !g->has_cur) return;
    int from = g->cur.y, to = vt_ghost_y(g);
    g->cur.y = (int8_t)to;
    if (g->mode != VT_MODE_PUZZLE) g->score += 2u * (uint32_t)(to - from);
    g->fx_hard_from = (int8_t)from;
    g->fx_hard_to = (int8_t)to;
    g->ev |= EV_HARD;
    lock_piece(g);
}

bool vt_hold(vt_game_t *g)
{
    if (g->state != VT_PLAY || !g->has_cur || g->hold_used) return false;
    if (g->mode == VT_MODE_PUZZLE) return false;
    vt_next_t was = { g->cur.type, g->cur.color };
    if (g->hold.type == 0xFF) {
        g->hold = was;
        vt_next_t nx = g->queue[0];
        for (int i = 0; i < VT_QUEUE - 1; i++) g->queue[i] = g->queue[i + 1];
        g->queue[VT_QUEUE - 1] = draw_next(g);
        if (!place_new(g, nx)) {
            game_over(g);
            return true;
        }
    } else {
        vt_next_t nx = g->hold;
        g->hold = was;
        if (!place_new(g, nx)) {
            game_over(g);
            return true;
        }
    }
    g->hold_used = true;
    g->ev |= EV_HOLD;
    return true;
}

void vt_tick(vt_game_t *g, uint32_t ms)
{
    if (g->state == VT_OVER || g->state == VT_WON || g->state == VT_STUCK) return;

    g->time_ms += ms;
    if (g->time_limit && g->time_ms >= g->time_limit) {
        g->time_ms = g->time_limit;
        game_over(g);
        return;
    }
    if (g->mode == VT_MODE_VERSUS) {
        int lv = g->start_level + (int)(g->time_ms / 45000u);
        if (lv > 20) lv = 20;
        if (lv != g->level) {
            g->level = (uint8_t)lv;
            g->ev |= EV_LEVEL;
        }
    }

    if (g->state == VT_CLEARING) {
        uint32_t was = g->clear_t;
        g->clear_t += ms;
        if (was < VT_CLEAR_FLASH_MS && g->clear_t >= VT_CLEAR_FLASH_MS) {
            remove_rows(g);
            g->ev |= EV_SHATTER | EV_BOARD;
        }
        if (g->clear_t >= VT_CLEAR_MS) {
            g->state = VT_PLAY;
            g->clear_n = 0;
            apply_garbage(g);
            if (g->state == VT_PLAY) spawn(g);
        }
        return;
    }

    if (!g->has_cur) return;

    if (cur_fits(g, 0, 1, g->cur.rot)) {
        uint32_t p = period(g);
        g->grav_acc += ms;
        int guard = 0;
        while (g->grav_acc >= p && guard++ < 30) {
            g->grav_acc -= p;
            if (!step_down(g)) {
                g->grav_acc = 0;
                break;
            }
            if (g->soft && g->mode != VT_MODE_PUZZLE) g->score += 1;
        }
    } else {
        g->grav_acc = 0;
        g->lock_acc += ms;
        /* holding soft drop on the floor locks a little sooner */
        uint32_t lock = g->soft ? VT_LOCK_MS / 2 : VT_LOCK_MS;
        if (g->lock_acc >= lock) lock_piece(g);
    }
}

/* -------------------------------------------------------------------------- */
/* The puzzles                                                                 */

/* Windows to fill, '#' glass. In every column the glass is one run with
 * stone under it (so every cell can be reached falling straight down);
 * tools/vt_harness.c checks it, and that the generator tiles each. */
static const struct {
    const char *name;
    const char *rows[12];
} PZ_SHAPES[] = {
    { N_("Flecha"), { "....#....", "...###...", "..#####..", ".#######.", "#########",
                  "...###...", "...###...", "...###...", NULL } },
    { N_("Cohete"), { "....##....", "...####...", "...####...", "...####...", "..######..",
                  ".########.", ".##.##.##.", NULL } },
    { N_("Pez"), { ".####...#", "######.##", "#########", "######.##", ".####...#", NULL } },
    { N_("Corona"), { "#...#...#", "##.###.##", "#########", "#########", "#########", NULL } },
    { N_("Montañas"), { "...#........", "..###....#..", ".#####..###.", "############",
                    "############", NULL } },
    { N_("Escalera"), { "........##", "......####", "....######", "..########", "##########",
                    "##########", NULL } },
    { N_("Rombo"), { "....#....", "...###...", "..#####..", ".#######.", "#########", ".#######.",
                 "..#####..", "...###...", "....#....", NULL } },
    { N_("Barco"), { ".....#.....", "....##.....", "...###.....", "..####.....", ".#####.....",
                 "###########", ".#########.", "..#######..", NULL } },
    { N_("Casa"), { "....#....", "...###...", "..#####..", ".#######.", "#########", ".#######.",
                ".#######.", ".##...##.", NULL } },
    { N_("Corazón"), { ".##...##.", "####.####", "#########", "#########", ".#######.", "..#####..",
                   "...###...", "....#....", NULL } },
    { N_("Árbol"), { "....#....", "...###...", "...###...", "..#####..", "..#####..", ".#######.",
                 ".#######.", "#########", "...###...", "...###...", NULL } },
    { N_("Gota"), { "....#....", "...###...", "..#####..", ".#######.", "#########", "#########",
                ".#######.", "..#####..", NULL } },
    { N_("Gato"), { "#........#", "##......##", "##########", "##########", "##########",
                ".########.", "..######..", NULL } },
    { N_("Ventana"), { "..####..", ".######.", "########", "########", "########", "########",
                   "########", "########", NULL } },
    { N_("Torre"), { "#.#.#.#.#", "#########", "#########", ".#######.", ".#######.", ".#######.",
                 ".#######.", ".#######.", NULL } },
};
#define PZ_NSHAPES ((int)(sizeof PZ_SHAPES / sizeof PZ_SHAPES[0]))

/* The tries of the generator that tile each level (from tools/vt_harness.c
 * --seeds): the board does not have to search through the ones that fail. */
static const uint8_t PZ_TRY[VT_PZ_LEVELS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

const char *vt_pz_name(int level)
{
    return PZ_SHAPES[level % PZ_NSHAPES].name;
}

typedef struct {
    vt_game_t *g;
    uint32_t   nodes;
    uint8_t    pool[PC_COUNT];
    int        npool;
    uint8_t    seq[VT_PZ_MAX_SEQ];
    vt_pz_place_t sol[VT_PZ_MAX_SEQ];
    uint8_t    rot_ok[PC_COUNT][4];
} pz_ctx_t;

#define PZ_NODE_LIMIT 30000

static int pz_left(const vt_game_t *g)
{
    int n = 0;
    for (int y = 0; y < VT_ROWS; y++)
        for (int x = 0; x < g->cols; x++)
            if (g->mask[y][x] && g->col[y][x] == VT_EMPTY) n++;
    return n;
}

static bool pz_search(pz_ctx_t *c, int depth)
{
    vt_game_t *g = c->g;
    int ex = -1, ey = -1;
    for (int y = VT_ROWS - 1; y >= 0 && ey < 0; y--) {
        for (int x = 0; x < g->cols; x++) {
            if (g->mask[y][x] && g->col[y][x] == VT_EMPTY) {
                ex = x;
                ey = y;
                break;
            }
        }
    }
    if (ey < 0) {
        g->pz_n = (uint8_t)depth;
        return true;
    }
    if (depth >= VT_PZ_MAX_SEQ) return false;
    if (++c->nodes > PZ_NODE_LIMIT) return false;
    int left = pz_left(g);
    if (left == 1 || left == 2 || left == 4 || left == 7) return false;

    /* the pool in a new order at every step */
    uint8_t order[PC_COUNT];
    int n = c->npool;
    memcpy(order, c->pool, (size_t)n);
    for (int i = n - 1; i > 0; i--) {
        int j = rnd_n(g, i + 1);
        uint8_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int oi = 0; oi < n; oi++) {
        int t = order[oi];
        const vt_shape_t *s = &vt_shapes[t];
        for (int r = 0; r < 4; r++) {
            if (!c->rot_ok[t][r]) continue;
            for (int k = 0; k < s->n; k++) {
                int ox = ex - s->c[r][k].x, oy = ey - s->c[r][k].y;
                bool ok = true;
                for (int i = 0; i < s->n && ok; i++) {
                    int x = ox + s->c[r][i].x, y = oy + s->c[r][i].y;
                    if (x < 0 || x >= g->cols || y < 0 || y >= VT_ROWS) ok = false;
                    else if (!g->mask[y][x] || g->col[y][x] != VT_EMPTY) ok = false;
                }
                if (!ok) continue;
                /* falling straight: nothing empty of the window under it */
                for (int i = 0; i < s->n && ok; i++) {
                    int x = ox + s->c[r][i].x, y = oy + s->c[r][i].y;
                    for (int yy = y + 1; yy < VT_ROWS && ok; yy++) {
                        if (!g->mask[yy][x] || g->col[yy][x] != VT_EMPTY) continue;
                        bool mine = false;
                        for (int j = 0; j < s->n; j++)
                            if (ox + s->c[r][j].x == x && oy + s->c[r][j].y == yy) mine = true;
                        if (!mine) ok = false;
                    }
                }
                if (!ok) continue;
                for (int i = 0; i < s->n; i++) g->col[oy + s->c[r][i].y][ox + s->c[r][i].x] = VT_RUBY;
                c->seq[depth] = (uint8_t)t;
                c->sol[depth].x = (int8_t)ox;
                c->sol[depth].y = (int8_t)oy;
                c->sol[depth].rot = (uint8_t)r;
                if (pz_search(c, depth + 1)) return true;
                for (int i = 0; i < s->n; i++) g->col[oy + s->c[r][i].y][ox + s->c[r][i].x] = VT_EMPTY;
            }
        }
    }
    return false;
}

/* Lays the picture of a level into the well: its window, the stone under
 * and beside it. */
static void pz_lay(vt_game_t *g, int level)
{
    int si = level % PZ_NSHAPES;
    bool flip = level >= PZ_NSHAPES;
    const char *const *rows = PZ_SHAPES[si].rows;
    int h = 0, w = 0;
    while (h < 12 && rows[h]) {
        int l = (int)strlen(rows[h]);
        if (l > w) w = l;
        h++;
    }
    memset(g->col, 0, sizeof g->col);
    memset(g->pid, 0, sizeof g->pid);
    memset(g->mask, 0, sizeof g->mask);
    int x0 = (g->cols - w) / 2, y0 = VT_ROWS - h;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w && rows[r][c]; c++) {
            int x = flip ? x0 + w - 1 - c : x0 + c;
            if (rows[r][c] == '#') g->mask[y0 + r][x] = 1;
        }
    }
    g->pz_x0 = (uint8_t)x0;
    g->pz_x1 = (uint8_t)(x0 + w - 1);
    g->pz_cells = 0;
    for (int x = 0; x < g->cols; x++) {
        bool open = x >= x0 && x < x0 + w;
        int top = VT_ROWS;
        for (int y = 0; y < VT_ROWS; y++) {
            if (g->mask[y][x]) {
                if (top == VT_ROWS) top = y;
                g->pz_cells++;
            }
        }
        for (int y = 0; y < VT_ROWS; y++) {
            bool stone = !open || (y > top && !g->mask[y][x]);
            if (stone) g->col[y][x] = VT_STONE;
        }
    }
}

bool vt_pz_build(vt_game_t *g, int level)
{
    shapes_init();
    if (level < 0) level = 0;
    if (level >= VT_PZ_LEVELS) level = VT_PZ_LEVELS - 1;
    pz_ctx_t *c = NULL;
    static pz_ctx_t ctx;           /* 200 bytes: not worth a malloc */
    c = &ctx;
    memset(c, 0, sizeof *c);
    c->g = g;

    /* the first half with the friendly pentominoes and the small ones; the
     * second half the same windows turned over, with every piece */
    bool hard = level >= PZ_NSHAPES;
    static const uint8_t EASY[] = { PC_I3, PC_V3, PC_P, PC_Q, PC_L, PC_J, PC_I5, PC_T, PC_U, PC_V };
    if (hard) {
        for (int t = 0; t < PC_COUNT; t++) c->pool[c->npool++] = (uint8_t)t;
    } else {
        memcpy(c->pool, EASY, sizeof EASY);
        c->npool = (int)sizeof EASY;
    }
    /* rotations that are the same picture are tried once */
    for (int t = 0; t < PC_COUNT; t++) {
        const vt_shape_t *s = &vt_shapes[t];
        for (int r = 0; r < 4; r++) {
            c->rot_ok[t][r] = 1;
            for (int q = 0; q < r && c->rot_ok[t][r]; q++) {
                /* compare normalised cell sets */
                int mxr = 9, myr = 9, mxq = 9, myq = 9;
                for (int i = 0; i < s->n; i++) {
                    if (s->c[r][i].x < mxr) mxr = s->c[r][i].x;
                    if (s->c[r][i].y < myr) myr = s->c[r][i].y;
                    if (s->c[q][i].x < mxq) mxq = s->c[q][i].x;
                    if (s->c[q][i].y < myq) myq = s->c[q][i].y;
                }
                int same = 0;
                for (int i = 0; i < s->n; i++)
                    for (int j = 0; j < s->n; j++)
                        if (s->c[r][i].x - mxr == s->c[q][j].x - mxq &&
                            s->c[r][i].y - myr == s->c[q][j].y - myq) same++;
                if (same == s->n) c->rot_ok[t][r] = 0;
            }
        }
    }

    bool ok = false;
    for (int k = PZ_TRY[level]; k < 256 && !ok; k++) {
        pz_lay(g, level);
        g->rng = 0x9E3779B1u * (uint32_t)(level + 1) + 0x7F4A7C15u * (uint32_t)(k + 1);
        if (!g->rng) g->rng = 1;
        c->nodes = 0;
        ok = pz_search(c, 0);
        if (ok) g->pz_try = (uint8_t)k;
    }
    pz_lay(g, level);
    if (!ok) return false;

    /* the pieces in the order the generator laid them, each a colour */
    for (int i = 0; i < g->pz_n; i++) {
        g->pz_seq[i].type = c->seq[i];
        g->pz_sol[i] = c->sol[i];
        g->pz_seq[i].color = (uint8_t)(1 + rnd_n(g, VT_NCOLORS));
    }
    g->pz_level = (uint8_t)level;
    g->pz_i = 0;
    g->undo_n = 0;
    g->state = VT_PLAY;
    for (int i = 0; i < VT_QUEUE; i++) {
        int k = 1 + i;
        if (k < g->pz_n) g->queue[i] = g->pz_seq[k];
        else g->queue[i].type = 0xFF;
    }
    place_new(g, g->pz_seq[0]);
    g->ev |= EV_BOARD | EV_SPAWN;
    return true;
}

/* A stack of glass for looking at (the simulator's scenes): the cells from
 * top[x] down in each column, tiled with real pieces by the puzzle's
 * generator, each piece a colour. */
bool vt_scene(vt_game_t *g, const uint8_t top[VT_COLS_MAX], uint32_t seed)
{
    shapes_init();
    static pz_ctx_t ctx;
    pz_ctx_t *c = &ctx;
    memset(c, 0, sizeof *c);
    c->g = g;
    for (int t = 0; t < PC_COUNT; t++) c->pool[c->npool++] = (uint8_t)t;
    for (int t = 0; t < PC_COUNT; t++)
        for (int r = 0; r < 4; r++) c->rot_ok[t][r] = 1;
    uint8_t keep_col[VT_ROWS][VT_COLS_MAX];
    memcpy(keep_col, g->col, sizeof keep_col);
    bool ok = false;
    for (int k = 0; k < 40 && !ok; k++) {
        memcpy(g->col, keep_col, sizeof keep_col);
        memset(g->mask, 0, sizeof g->mask);
        for (int x = 0; x < g->cols; x++)
            for (int y = top[x]; y < VT_ROWS; y++) g->mask[y][x] = 1;
        g->rng = seed * 2654435761u + (uint32_t)k * 97u + 1u;
        c->nodes = 0;
        ok = pz_search(c, 0);
    }
    memset(g->mask, 0, sizeof g->mask);
    memcpy(g->col, keep_col, sizeof keep_col);
    if (!ok) return false;
    for (int i = 0; i < g->pz_n; i++) {
        const vt_shape_t *s = &vt_shapes[c->seq[i]];
        uint8_t color = (uint8_t)(1 + rnd_n(g, VT_NCOLORS));
        uint16_t pid = ++g->next_pid;
        for (int j = 0; j < s->n; j++) {
            int x = c->sol[i].x + s->c[c->sol[i].rot][j].x, y = c->sol[i].y + s->c[c->sol[i].rot][j].y;
            g->col[y][x] = color;
            g->pid[y][x] = pid;
        }
    }
    g->pz_n = 0;
    return true;
}
