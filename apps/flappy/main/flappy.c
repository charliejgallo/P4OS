/*
 * P4OS - Flappy (from AmoledOS), an example dynamic app with some substance
 * to it.
 *
 * The same source builds two ways:
 *
 *   .so for the board       cd apps/flappy && idf.py so
 *   built-in simulator app  the simulator builds it (AOS_SIM_BUILTIN)
 *
 * It uses only LVGL and aos_hal.h: no ESP-IDF. That is why it runs on both
 * sides.
 *
 * P4OS: the watch's field was 368x362; here it is the whole 720x1280 panel
 * with the sky running under the status bar (AOS_APP_FLAG_UNDER_BAR), and
 * upright only. The geometry grew by ~1.65 and the physics with it, per
 * frame, so the TIMING is the watch's: a flap climbs for the same 14 frames
 * and a pipe arrives every 1.3 s. The field is relatively taller than the
 * watch's, though, so a gap never lands more than MAX_DELTA from the
 * previous one: without that the next gap could be a whole screen away,
 * which no amount of tapping reaches.
 *
 * Each pipe is ONE transparent column with its four pieces (two bodies, two
 * caps) inside: a frame moves three objects, not twelve, and the pieces only
 * change size when the column is recycled at the right edge.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Game parameters, for a 720x1280 field at 50 fps.
 * -------------------------------------------------------------------------- */
#define FRAME_MS        20
#define GROUND_H        120
#define GRASS_H         18

#define BIRD_SIZE       56
#define BIRD_X          170
#define BIRD_INSET      5          /* the hitbox forgives the round corners */

#define GRAVITY         0.90f      /* px per frame squared */
#define FLAP_IMPULSE    -12.4f
#define MAX_FALL        16.0f

#define PIPE_W          104
#define CAP_H           40
#define CAP_OVER        8          /* how much the cap sticks out each side */
#define COL_W           (PIPE_W + 2 * CAP_OVER)
#define PIPE_MARGIN     130        /* minimum margin above and below the gap */
#define MAX_DELTA       340        /* furthest a gap may be from the previous one */

/* Difficulty: it can be overridden from the compiler (-DPIPE_GAP=...) to test
 * variants without touching the source. */
#ifndef PIPE_GAP
#define PIPE_GAP        270
#endif
#ifndef PIPE_SPEED
#define PIPE_SPEED      5
#endif
#ifndef PIPE_SPACING
#define PIPE_SPACING    330        /* horizontal distance between pipes */
#endif

#define PIPE_COUNT      3
#define STARS           26
#define DEAD_MS         500        /* deaf after dying, so the same frantic
                                    * tap does not skip the result */

#define COL_SKY_TOP     0x0B1B2B
#define COL_SKY_BOT     0x1A4A5C
#define COL_PIPE        0x30D158
#define COL_PIPE_HI     0x5CE07E
#define COL_PIPE_DARK   0x1E8E3E
#define COL_GROUND      0x3A2E1F
#define COL_GRASS       0x2FA84F
#define COL_BIRD        0xFFD60A

typedef enum {
    STATE_READY = 0,
    STATE_PLAYING,
    STATE_DEAD,
} game_state_t;

typedef struct {
    lv_obj_t *col;          /* transparent column, the only thing that moves */
    lv_obj_t *top, *top_cap;
    lv_obj_t *bottom, *bottom_cap;
    int32_t   x;            /* left edge of the BODY */
    int32_t   gap_y;        /* centre of the gap */
    bool      scored;
} pipe_t;

typedef struct {
    lv_obj_t   *field;      /* playing area, without the ground */
    lv_obj_t   *bird;
    lv_obj_t   *wing;
    lv_obj_t   *score_label;
    lv_obj_t   *overlay;
    lv_obj_t   *overlay_title;
    lv_obj_t   *overlay_score;
    lv_obj_t   *overlay_detail;
    lv_timer_t *timer;

    pipe_t      pipes[PIPE_COUNT];
    float       bird_y;
    float       bird_v;
    int32_t     field_w;
    int32_t     field_h;
    int32_t     last_gap;
    int         wing_t;     /* frames left of the wing's down stroke */

    game_state_t state;
    uint32_t     dead_ms;
    int          score;
    int          best;
    uint32_t     rng;
} flappy_t;

/* --------------------------------------------------------------------------
 * Random numbers of our own: the libc's rand() would force the firmware to
 * export it, and this way the game is also reproducible if debugging is
 * needed.
 * -------------------------------------------------------------------------- */
static uint32_t xorshift(flappy_t *game)
{
    uint32_t x = game->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->rng = x;
    return x;
}

static int32_t random_gap_y(flappy_t *game)
{
#ifdef FLAPPY_FIXED_GAP
    /* The gap always at the same height: useful for testing the score with a
     * bot tapping at a constant rate, which otherwise dodges nothing. */
    (void)game;
    return FLAPPY_FIXED_GAP;
#else
    int32_t lo = PIPE_MARGIN + PIPE_GAP / 2;
    int32_t hi = game->field_h - PIPE_MARGIN - PIPE_GAP / 2;
    if (game->last_gap > 0) {
        if (lo < game->last_gap - MAX_DELTA) {
            lo = game->last_gap - MAX_DELTA;
        }
        if (hi > game->last_gap + MAX_DELTA) {
            hi = game->last_gap + MAX_DELTA;
        }
    }
    if (hi <= lo) {
        hi = lo + 1;
    }
    game->last_gap = lo + (int32_t)(xorshift(game) % (uint32_t)(hi - lo));
    return game->last_gap;
#endif
}

/* -------------------------------------------------------------------------- */

static lv_obj_t *block(lv_obj_t *parent, uint32_t color, uint32_t hi, uint32_t border_color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(hi), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_color), 0);
    lv_obj_set_style_border_width(obj, 4, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* Only when the gap changes: sizes and heights of the four pieces. */
static void pipe_shape(flappy_t *game, pipe_t *pipe)
{
    int32_t top_h    = pipe->gap_y - PIPE_GAP / 2;
    int32_t bottom_y = pipe->gap_y + PIPE_GAP / 2;

    /* the body starts above the field so its rounded end never shows */
    lv_obj_set_pos(pipe->top, CAP_OVER, -16);
    lv_obj_set_size(pipe->top, PIPE_W, top_h - CAP_H + 16 + 4);
    lv_obj_set_pos(pipe->top_cap, 0, top_h - CAP_H);

    lv_obj_set_pos(pipe->bottom_cap, 0, bottom_y);
    lv_obj_set_pos(pipe->bottom, CAP_OVER, bottom_y + CAP_H - 4);
    lv_obj_set_size(pipe->bottom, PIPE_W, game->field_h - bottom_y - CAP_H + 20);
}

static void pipe_move(pipe_t *pipe)
{
    lv_obj_set_x(pipe->col, pipe->x - CAP_OVER);
}

static void pipes_reset(flappy_t *game)
{
    game->last_gap = 0;
    for (int i = 0; i < PIPE_COUNT; i++) {
        pipe_t *pipe = &game->pipes[i];
        pipe->x      = game->field_w + 80 + i * PIPE_SPACING;
        pipe->gap_y  = random_gap_y(game);
        pipe->scored = false;
        pipe_shape(game, pipe);
        pipe_move(pipe);
    }
}

static void bird_apply(flappy_t *game)
{
    lv_obj_set_y(game->bird, (int32_t)game->bird_y);

    /* tilt according to the vertical velocity: it climbs pointing up and dives
     * on the way down. Purely cosmetic but it changes the feel enormously.
     * The factor is the watch's 40 scaled down with the physics, so the same
     * speed in frames gives the same angle. */
    int32_t angle = (int32_t)(game->bird_v * 24.0f);
    if (angle < -300) angle = -300;
    if (angle > 800)  angle = 800;
    lv_obj_set_style_transform_rotation(game->bird, angle, 0);
}

static void wing_set(flappy_t *game, bool down)
{
    lv_obj_set_y(game->wing, down ? 30 : 18);
}

static void overlay_show(flappy_t *game, const char *title, const char *score,
                         const char *detail)
{
    lv_label_set_text(game->overlay_title, title);
    if (score) {
        lv_label_set_text(game->overlay_score, score);
        lv_obj_remove_flag(game->overlay_score, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(game->overlay_score, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(game->overlay_detail, detail);
    lv_obj_remove_flag(game->overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(game->overlay);
}

static void game_reset(flappy_t *game)
{
    game->state   = STATE_READY;
    game->score   = 0;
    game->bird_y  = (float)(game->field_h / 2 - BIRD_SIZE / 2);
    game->bird_v  = 0.0f;

    pipes_reset(game);
    bird_apply(game);
    lv_label_set_text(game->score_label, "0");
    lv_obj_add_flag(game->score_label, LV_OBJ_FLAG_HIDDEN);

    char detail[80];
    snprintf(detail, sizeof(detail), _("Tocá para empezar\nRécord  %d"), game->best);
    overlay_show(game, "Flappy", NULL, detail);
}

static void game_over(flappy_t *game)
{
#ifdef FLAPPY_DEBUG
    printf("[flappy] muerte con y=%.1f (campo %d) puntos=%d\n",
           (double)game->bird_y, (int)game->field_h, game->score);
#endif
    game->state   = STATE_DEAD;
    game->dead_ms = lv_tick_get();
    aos_hal_beep(220, 180);

    char score[16];
    snprintf(score, sizeof(score), "%d", game->score);

    char detail[80];
    if (game->score > game->best) {
        game->best = game->score;
        aos_hal_pref_set_i32("flappy_best", (int32_t)game->best);
        snprintf(detail, sizeof(detail), "%s", _("¡Nuevo récord!"));
    } else {
        snprintf(detail, sizeof(detail), _("Récord  %d"), game->best);
    }
    overlay_show(game, _("Perdiste"), score, detail);
}

static bool bird_hits_pipe(const flappy_t *game, const pipe_t *pipe)
{
    const int32_t bird_left   = BIRD_X + BIRD_INSET;
    const int32_t bird_right  = BIRD_X + BIRD_SIZE - BIRD_INSET;
    const int32_t bird_top    = (int32_t)game->bird_y + BIRD_INSET;
    const int32_t bird_bottom = (int32_t)game->bird_y + BIRD_SIZE - BIRD_INSET;

    if (bird_right < pipe->x - CAP_OVER || bird_left > pipe->x + PIPE_W + CAP_OVER) {
        return false;
    }
    return bird_top < pipe->gap_y - PIPE_GAP / 2 ||
           bird_bottom > pipe->gap_y + PIPE_GAP / 2;
}

static void step(lv_timer_t *timer)
{
    flappy_t *game = (flappy_t *)lv_timer_get_user_data(timer);

    if (game->state != STATE_PLAYING) {
        return;
    }

#ifdef FLAPPY_DEBUG
    {
        static int frames; static uint64_t t0;
        if (t0 == 0) t0 = aos_hal_uptime_ms();
        if (++frames % 25 == 0) {
            uint64_t dt = aos_hal_uptime_ms() - t0;
            printf("[flappy] %d frames %.1f ms/f y=%.1f v=%.1f | tubos x/gap: "
                   "%d/%d %d/%d %d/%d | campo %d\n",
                   frames, (double)dt / frames,
                   (double)game->bird_y, (double)game->bird_v,
                   (int)game->pipes[0].x, (int)game->pipes[0].gap_y,
                   (int)game->pipes[1].x, (int)game->pipes[1].gap_y,
                   (int)game->pipes[2].x, (int)game->pipes[2].gap_y,
                   (int)game->field_h);
        }
    }
#endif

    if (game->wing_t > 0 && --game->wing_t == 0) {
        wing_set(game, false);
    }

    /* the bird's physics */
    game->bird_v += GRAVITY;
    if (game->bird_v > MAX_FALL) {
        game->bird_v = MAX_FALL;
    }
    game->bird_y += game->bird_v;

    if (game->bird_y < 0.0f) {
        game->bird_y = 0.0f;
        game->bird_v = 0.0f;
    }
    bird_apply(game);

    if (game->bird_y + BIRD_SIZE >= (float)game->field_h) {
        game->bird_y = (float)(game->field_h - BIRD_SIZE);
        bird_apply(game);
        game_over(game);
        return;
    }

    /* pipes */
    for (int i = 0; i < PIPE_COUNT; i++) {
        pipe_t *pipe = &game->pipes[i];
        pipe->x -= PIPE_SPEED;

        if (pipe->x + PIPE_W + CAP_OVER < 0) {
            /* the pipe is recycled at the end of the row */
            int32_t rightmost = pipe->x;
            for (int j = 0; j < PIPE_COUNT; j++) {
                if (game->pipes[j].x > rightmost) {
                    rightmost = game->pipes[j].x;
                }
            }
            pipe->x      = rightmost + PIPE_SPACING;
            pipe->gap_y  = random_gap_y(game);
            pipe->scored = false;
            pipe_shape(game, pipe);
        }

        pipe_move(pipe);

        if (!pipe->scored && pipe->x + PIPE_W < BIRD_X) {
            pipe->scored = true;
            game->score++;
            char buf[12];
            snprintf(buf, sizeof(buf), "%d", game->score);
            lv_label_set_text(game->score_label, buf);
            aos_hal_beep(1800, 25);
        }

        if (bird_hits_pipe(game, pipe)) {
            game_over(game);
            return;
        }
    }
}

static void flap(flappy_t *game, int beep_ms)
{
    game->bird_v = FLAP_IMPULSE;
    game->wing_t = 6;
    wing_set(game, true);
    aos_hal_beep(1200, beep_ms);
}

static void tap_cb(lv_event_t *event)
{
    flappy_t *game = (flappy_t *)lv_event_get_user_data(event);

    switch (game->state) {
    case STATE_READY:
        lv_obj_add_flag(game->overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(game->score_label, LV_OBJ_FLAG_HIDDEN);
        game->state = STATE_PLAYING;
        flap(game, 20);
        break;

    case STATE_PLAYING:
        flap(game, 15);
        break;

    case STATE_DEAD:
        if ((uint32_t)(lv_tick_get() - game->dead_ms) >= DEAD_MS) {
            game_reset(game);
        }
        break;
    }
}

/* -------------------------------------------------------------------------- */

static lv_obj_t *deco(lv_obj_t *parent, int32_t w, int32_t h, uint32_t color, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void build_bird(flappy_t *game)
{
    game->bird = deco(game->field, BIRD_SIZE, BIRD_SIZE, COL_BIRD, LV_RADIUS_CIRCLE);
    lv_obj_set_x(game->bird, BIRD_X);
    lv_obj_set_style_border_width(game->bird, 3, 0);
    lv_obj_set_style_border_color(game->bird, lv_color_hex(0xC89000), 0);
    lv_obj_set_style_transform_pivot_x(game->bird, BIRD_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(game->bird, BIRD_SIZE / 2, 0);

    /* belly */
    lv_obj_t *belly = deco(game->bird, 30, 20, 0xFFF1A8, LV_RADIUS_CIRCLE);
    lv_obj_align(belly, LV_ALIGN_BOTTOM_MID, 4, -5);

    /* eye: white with a pupil looking ahead */
    lv_obj_t *eye = deco(game->bird, 18, 18, 0xFFFFFF, LV_RADIUS_CIRCLE);
    lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -7, 9);
    lv_obj_t *pupil = deco(eye, 8, 8, 0x1C1C1E, LV_RADIUS_CIRCLE);
    lv_obj_align(pupil, LV_ALIGN_RIGHT_MID, -2, 0);

    /* beak */
    lv_obj_t *beak = deco(game->bird, 20, 11, 0xFF9F0A, 5);
    lv_obj_align(beak, LV_ALIGN_RIGHT_MID, 10, 6);

    /* wing: drops for a few frames on every flap */
    game->wing = deco(game->bird, 22, 14, 0xFFB800, LV_RADIUS_CIRCLE);
    lv_obj_set_x(game->wing, 6);
    wing_set(game, false);
}

static void *flappy_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    flappy_t *game = lv_malloc_zeroed(sizeof(flappy_t));
    if (!game) {
        return NULL;
    }

    /* seed: the uptime is enough and it does not depend on the libc */
    game->rng = (uint32_t)aos_hal_uptime_ms() | 1u;

    int32_t best = 0;
    if (aos_hal_pref_get_i32("flappy_best", &best)) {
        game->best = (int)best;
    }

    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_SKY_TOP), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(COL_SKY_BOT), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_update_layout(root);
    int32_t rw = lv_obj_get_width(root);
    int32_t rh = lv_obj_get_height(root);
    if (rw <= 0 || rh <= 0) {        /* in case the layout has not run yet */
        rw = AOS_SCREEN_W;
        rh = AOS_SCREEN_H;
    }
    game->field_w = rw;
    game->field_h = rh - GROUND_H;

    /* the night: a moon and a few stars, still, so they cost nothing after
     * the first frame. A fixed seed so the sky is always the same. */
    {
        lv_obj_t *moon = deco(root, 96, 96, 0xF2EFD8, LV_RADIUS_CIRCLE);
        lv_obj_set_pos(moon, rw - 96 - 72, 150);
        lv_obj_set_style_shadow_color(moon, lv_color_hex(0xF2EFD8), 0);
        lv_obj_set_style_shadow_width(moon, 60, 0);
        lv_obj_set_style_shadow_opa(moon, LV_OPA_30, 0);
        lv_obj_t *bite = deco(moon, 30, 30, 0xDCD8BC, LV_RADIUS_CIRCLE);
        lv_obj_set_pos(bite, 20, 44);

        uint32_t save = game->rng;
        game->rng = 0x9E3779B9u;
        for (int i = 0; i < STARS; i++) {
            int32_t d = 3 + (int32_t)(xorshift(game) % 4u);
            lv_obj_t *star = deco(root, d, d, 0xFFFFFF, LV_RADIUS_CIRCLE);
            lv_obj_set_style_bg_opa(star, (lv_opa_t)(90 + xorshift(game) % 140u), 0);
            lv_obj_set_pos(star, (int32_t)(xorshift(game) % (uint32_t)rw),
                           60 + (int32_t)(xorshift(game) % (uint32_t)(game->field_h * 2 / 3)));
        }
        game->rng = save;
    }

    /* playing field: it clips the pipes so they are not drawn over the ground */
    game->field = lv_obj_create(root);
    lv_obj_remove_style_all(game->field);
    lv_obj_set_size(game->field, rw, game->field_h);
    lv_obj_set_pos(game->field, 0, 0);
    lv_obj_remove_flag(game->field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(game->field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(game->field, true, 0);

    for (int i = 0; i < PIPE_COUNT; i++) {
        pipe_t *p = &game->pipes[i];
        p->col = lv_obj_create(game->field);
        lv_obj_remove_style_all(p->col);
        lv_obj_set_size(p->col, COL_W, game->field_h);
        lv_obj_set_y(p->col, 0);
        lv_obj_remove_flag(p->col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(p->col, LV_OBJ_FLAG_CLICKABLE);

        p->top        = block(p->col, COL_PIPE, COL_PIPE_HI, COL_PIPE_DARK);
        p->bottom     = block(p->col, COL_PIPE, COL_PIPE_HI, COL_PIPE_DARK);
        p->top_cap    = block(p->col, COL_PIPE, COL_PIPE_HI, COL_PIPE_DARK);
        p->bottom_cap = block(p->col, COL_PIPE, COL_PIPE_HI, COL_PIPE_DARK);
        lv_obj_set_size(p->top_cap, COL_W, CAP_H);
        lv_obj_set_size(p->bottom_cap, COL_W, CAP_H);
    }

    build_bird(game);

    /* ground, with its strip of grass */
    lv_obj_t *ground = deco(root, rw, GROUND_H, COL_GROUND, 0);
    lv_obj_set_pos(ground, 0, game->field_h);
    lv_obj_t *grass = deco(ground, rw, GRASS_H, COL_GRASS, 0);
    lv_obj_set_pos(grass, 0, 0);
    lv_obj_set_style_border_color(grass, lv_color_hex(0x1E7A38), 0);
    lv_obj_set_style_border_width(grass, 4, 0);
    lv_obj_set_style_border_side(grass, LV_BORDER_SIDE_BOTTOM, 0);

    /* score, under the status bar */
    game->score_label = lv_label_create(root);
    lv_label_set_text(game->score_label, "0");
    lv_obj_set_style_text_color(game->score_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(game->score_label, &aos_inter_num_96, 0);
    lv_obj_align(game->score_label, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_remove_flag(game->score_label, LV_OBJ_FLAG_CLICKABLE);

    /* start and game over card */
    game->overlay = lv_obj_create(root);
    lv_obj_remove_style_all(game->overlay);
    /* below the bird's starting height, so the bird waits in plain sight
     * above the card that says to tap */
    lv_obj_set_size(game->overlay, 500, 340);
    lv_obj_align(game->overlay, LV_ALIGN_CENTER, 0, 200);
    lv_obj_set_style_bg_color(game->overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(game->overlay, LV_OPA_70, 0);
    lv_obj_set_style_radius(game->overlay, 40, 0);
    lv_obj_remove_flag(game->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(game->overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(game->overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(game->overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(game->overlay, 12, 0);

    game->overlay_title = lv_label_create(game->overlay);
    lv_obj_set_style_text_color(game->overlay_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(game->overlay_title, aos_font_large, 0);

    game->overlay_score = lv_label_create(game->overlay);
    lv_obj_set_style_text_color(game->overlay_score, lv_color_hex(COL_BIRD), 0);
    lv_obj_set_style_text_font(game->overlay_score, &aos_inter_num_96, 0);

    game->overlay_detail = lv_label_create(game->overlay);
    lv_obj_set_style_text_color(game->overlay_detail, lv_color_hex(0xAEAEB2), 0);
    lv_obj_set_style_text_font(game->overlay_detail, aos_font_body, 0);
    lv_obj_set_style_text_align(game->overlay_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(game->overlay_detail, 8, 0);

    /* A transparent layer above everything to receive the touches. In LVGL 9
     * every lv_obj is born clickable, so the playing field, the pipes and the
     * ground would eat the touch before it reached the root. */
    lv_obj_t *touch = lv_obj_create(root);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, tap_cb, LV_EVENT_PRESSED, game);
    lv_obj_move_foreground(touch);

    game_reset(game);
    game->timer = lv_timer_create(step, FRAME_MS, game);
    return game;
}

static void flappy_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    flappy_t *game = (flappy_t *)inst;
    if (!game) {
        return;
    }
    if (game->timer) {
        lv_timer_delete(game->timer);
    }
    lv_free(game);
}

static void flappy_hide(aos_app_t *self, void *inst)
{
    (void)self;
    flappy_t *game = (flappy_t *)inst;
    if (game && game->state == STATE_PLAYING) {
        game->state   = STATE_DEAD;   /* leaving mid-game should not count */
        game->dead_ms = lv_tick_get();
        overlay_show(game, _("Pausa"), NULL, _("Tocá para reiniciar"));
    }
}

static bool flappy_init(aos_app_t *app)
{
    app->desc.id      = "demo.flappy";
    app->desc.name    = "Flappy";
    app->desc.icon    = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_BIRD;
    app->desc.color_a = 0x30D158;
    app->desc.color_b = 0x0E7A32;
    app->desc.order   = 150;
    app->desc.flags   = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_PORTRAIT |
                        AOS_APP_FLAG_UNDER_BAR;

    app->create  = flappy_create;
    app->destroy = flappy_destroy;
    app->hide    = flappy_hide;
    return true;
}

AOS_APP_ENTRY(flappy_init);
