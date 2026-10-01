/*
 * ARKANOS - test bench without a screen
 *
 * It compiles the game WITHOUT LVGL and without SDL
 * (ak_play/ak_draw/ak_pixel/ak_level depend on nothing but the translation
 * call and the sound, both stood in for below) and makes it play itself for
 * thousands of frames, in portrait or, with AK_LAND=1, in landscape. It
 * serves two purposes:
 *
 *  1. VERIFYING THE DIRTY LIST. On every frame it draws twice: once by the
 *     fast path (restoring only the recorded rectangles) and once rebuilding
 *     the whole screen from the background. If the two buffers do not come out
 *     identical, somebody moved without recording their rectangle, which is
 *     THE bug of this scheme and on screen looks like a dirty trail.
 *
 *  2. MEASURING how much is saved: what percentage of the screen is upscaled
 *     and invalidated per frame.
 *
 *   cc -O2 -DAOS_SIM -I <main> -I <aos_hal/include> -I <aos_ui/include> \
 *      ak_harness.c ../main/ak_*.c -o /tmp/akh
 *   /tmp/akh [frames] [level] [capture_prefix]
 *   AK_LAND=1 /tmp/akh ...      the landscape layout (426x240)
 *   AK_TURN=1 /tmp/akh ...      turns the screen every 700 frames mid-game
 */
#include "arkanos.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game wraps its visible text in _(), which is aos_tr(). Nothing is
 * translated here and the runtime would drag LVGL along, so the identity
 * stands in - which is what aos_tr() returns with no pack loaded. */
const char *aos_tr(const char *es) { return es; }

/* --- the little of the system the game uses ------------------------------- */

static int s_beeps;

void ak_sfx(int freq_hz, int ms)
{
    (void)freq_hz;
    (void)ms;
    s_beeps++;
}

/* --- image ----------------------------------------------------------------*/

static void dump_ppm(const uint16_t *px, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    /* the canvas as it is, 1:1: on P4OS the OS scales it (aos_retro) */
    fprintf(f, "P6\n%d %d\n255\n", (int)AK_W, (int)AK_H);
    for (int y = 0; y < AK_H; y++) {
        for (int x = 0; x < AK_W; x++) {
            uint16_t c = px[y * AK_W + x];
            unsigned char rgb[3];
            rgb[0] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
            rgb[1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
            rgb[2] = (unsigned char)((c & 0x1F) * 255 / 31);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* --- the loop -------------------------------------------------------------*/

static ak_t g;
static uint16_t fb[AK_PX_MAX];
static uint16_t bg[AK_PX_MAX];
static uint16_t ref[AK_PX_MAX];         /* the "rebuilt whole" version */

int main(int argc, char **argv)
{
    int cuadros = argc > 1 ? atoi(argv[1]) : 3000;
    int nivel   = argc > 2 ? atoi(argv[2]) : 0;
    const char *shots = argc > 3 ? argv[3] : NULL;
    /* AK_TORPE=1 switches the autopilot off now and then, so the ball falls
     * and the lose-a-life and game-over paths are exercised too. */
    int torpe = getenv("AK_TORPE") ? 1 : 0;
    int land = getenv("AK_LAND") ? 1 : 0;
    int turn = getenv("AK_TURN") ? 1 : 0;

    ak_geo_set(land);
    ak_buf_init(&g.fb, fb, AK_W, AK_H);
    ak_buf_init(&g.bg, bg, AK_W, AK_H);
    g.rng = 12345;
    g.autoplay = 1;
    g.show_fps = 1;

    ak_game_start(&g);
    if (nivel) {
        ak_load_level(&g, nivel);
    }

    long area_total = 0, area_max = 0;
    int fallos = 0, niveles = 0, muertes = 0;
    uint8_t nivel_prev = g.level;
    ak_rect_t entera = { 0, 0, AK_W, AK_H };

    for (int f = 0; f < cuadros; f++) {
        if (g.state == ST_OVER || g.state == ST_WIN) {
            /* keep measuring: it restarts and carries on */
            if (g.state == ST_WIN) {
                ak_load_level(&g, ak_level_count());
            } else {
                muertes++;
                ak_game_start(&g);
            }
        }
        if (torpe) {
            g.autoplay = ((f / 300) % 2) ? 0 : 1;
        }
        /* The screen turns now and then, as the app's resize() does it: the
         * new field has to come out as clean as a level just loaded. */
        if (turn && f && (f % 700) == 0) {
            ak_geo_t old = ak_geo;
            land = !land;
            ak_geo_set(land);
            ak_buf_init(&g.fb, fb, AK_W, AK_H);
            ak_buf_init(&g.bg, bg, AK_W, AK_H);
            entera = (ak_rect_t){ 0, 0, AK_W, AK_H };
            ak_relayout(&g, &old);
        }
        /* a finger now and then, for the deck's carriage, which lights */
        g.touching = ((f / 53) % 3) == 0;
        ak_step(&g);

        /* ---- fast path ---- */
        g.d_push = g.d_prev;
        ak_dirty_join(&g.d_push, &g.d_bg);
        if (g.d_push.all) {
            ak_restore(fb, bg, &entera);
            g.hud_dirty = 1;
        } else {
            for (int i = 0; i < g.d_push.n; i++) {
                ak_restore(fb, bg, &g.d_push.r[i]);
            }
        }
        ak_draw_movers(&g);
        bool hud = g.hud_dirty != 0;
        if (hud) {
            ak_draw_hud(&g);
        }
        ak_dirty_join(&g.d_push, &g.d_cur);

        int area = ak_dirty_area(&g.d_push);
        area_total += area;
        if (area > area_max) {
            area_max = area;
        }
        /* ---- reference: the whole screen, from scratch ----
         * NOTE: g.last_area is updated AFTER comparing. The FPS counter is
         * drawn by the game itself, so if it were updated first, the reference
         * pass would write a different number and the comparison would fail
         * because of the test bench and not because of the game. */
        ak_dirty_t save_push = g.d_push;
        ak_dirty_t save_cur  = g.d_cur;
        memcpy(ref, bg, sizeof(ref));
        g.fb.px = ref;
        ak_draw_movers(&g);
        g.hud_dirty = 1;
        ak_draw_hud(&g);
        g.fb.px = fb;
        g.d_push = save_push;
        g.d_cur  = save_cur;
        g.hud_dirty = 0;

        if (memcmp(fb, ref, sizeof(ref)) != 0) {
            int primero = -1, cuantos = 0;
            for (int i = 0; i < AK_W * AK_H; i++) {
                if (fb[i] != ref[i]) {
                    if (primero < 0) {
                        primero = i;
                    }
                    cuantos++;
                }
            }
            if (fallos < 8) {
                printf("FRAME %d: %d pixels not noted down, the first at "
                       "(%d,%d) estado=%d rects=%d\n",
                       f, cuantos, primero % AK_W, primero / AK_W,
                       g.state, g.d_push.n);
            }
            fallos++;
            memcpy(fb, ref, sizeof(ref));   /* carry on measuring from something sane */
        }

        g.last_area = (uint16_t)(area * 100 / (AK_W * AK_H));
        g.d_prev = g.d_cur;
        ak_dirty_reset(&g.d_bg);

        if (g.level != nivel_prev) {
            niveles++;
            nivel_prev = g.level;
        }
        if (shots && (f % 400) == 60) {
            char path[256];
            snprintf(path, sizeof(path), "%s%02d.ppm", shots, f / 400);
            dump_ppm(fb, path);
        }
    }

    printf("\n%d frames | %d levels cleared | %d games lost | "
           "%d beeps\n", cuadros, niveles, muertes, s_beeps);
    /* with one decimal: truncating to an integer, 2.9% and 3.0% show as "2"
       and "3" and look like a regression where there is none */
    long prom10 = area_total * 1000 / cuadros / (AK_W * AK_H);
    printf("area pushed: %ld.%ld%% on average, %ld%% in the worst frame "
           "(the whole screen is %d pixels)\n",
           prom10 / 10, prom10 % 10,
           area_max * 100 / (AK_W * AK_H), AK_W * AK_H);
    printf("score %lu | best %lu | level %d | lives %d\n",
           (unsigned long)g.score, (unsigned long)g.hiscore, g.level + 1, g.lives);

    if (fallos) {
        printf("\n%d frames FAILED: something moves without noting down its "
               "rectangulo\n", fallos);
        return 1;
    }
    printf("\nthe dirty list matches the full redraw across all "
           "%d frames\n", cuadros);
    return 0;
}
