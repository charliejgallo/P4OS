/*
 * GOLF on P4OS - the renderers on the Mac, the best of a few runs each: the
 * 3D view (with the time of each stage, per core's half) and the aiming map,
 * at the P4's sizes, and the pictures they made.
 *
 *   cc -O2 -Imain -I../../components/aos_hal/include -DAOS_SIM tools/bench.c  *      main/gf_world.c main/gf_map.c main/gf_gfx.c main/gf_holes.c main/gf_art.c  *      main/gf_view3d.c main/gf_phys.c main/gf_game.c main/gf_outfit.c -o build/bench -lm
 *   build/bench p|l <hole> build/golf_p4.pak      (upright or lying down)
 *
 * -> bench_3d.ppm, bench_v.ppm (the map). Both halves run one after the
 * other here: the board splits them over its two cores.
 */
#include "gf_world.h"
#include "gf_map.h"
#include "gf_gfx.h"
#include "gf_view3d.h"
#include "gf_phys.h"
#include "gf_art.h"
#include "gf_game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
void aos_hal_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
uint64_t aos_hal_uptime_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u; }
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static uint32_t clk(void) { return (uint32_t)aos_hal_uptime_ms(); }
int main(int argc, char **argv)
{
    int land = argc > 1 && argv[1][0] == 'l';
    int hole = argc > 2 ? atoi(argv[2]) : 1;
    gf_w = land ? 1280 : 720; gf_h = land ? 720 : 1280;
    gf_set_clock(clk);
    gf_art_load(argc > 3 ? argv[3] : "golf.pak");
    gf_map_set_tree_art(gf_art_trees());
    gf_phys_init(); gf_tex_init();
    gf_course_select(0);
    gf_world_t w; memset(&w, 0, sizeof w);
    int gw, gh; gf_world_grid_size(&gf_course()->holes[hole - 1], &gw, &gh);
    gf_world_init(&w, gw * gh * 2);
    gf_world_load(&w, &gf_course()->holes[hole - 1], 1, 0);
    float mpp = 0.5f; int tw = (int)(w.gw / mpp), th = (int)(w.gh / mpp);
    uint16_t *alb = malloc((size_t)tw * th * 2);
    double t0 = now_ms(); gf_map_albedo(&w, alb, tw, th, mpp); double talb = now_ms() - t0;
    uint16_t *out = malloc(GF_MAXPX * 2), *low = malloc(GF_MAXPX / 2), *dep = malloc(GF_MAXPX / 2);
    gf_cam_frame(gf_w / 2, land ? 318 : 700);
    gf_cam_t c; gf_cam_swing(&c, &w, w.tee_x, w.tee_y, atan2f(w.pin_x - w.tee_x, w.pin_y - w.tee_y));
    gf_albedo_t a = { alb, tw, th, mpp };
    gf_v3d_target_t t = { out, gf_w, gf_h, low, dep, 2 };
    double best = 1e9, bg = 1e9, bs = 1e9, bu = 1e9, bt = 1e9;
    for (int i = 0; i < 25; i++) {
        t0 = now_ms(); gf_view3d_render(&w, &c, &a, &t); double d = now_ms() - t0;
        if (d < best) best = d;
        if (gf_prof_get(0) < bg) bg = gf_prof_get(0);
        if (gf_prof_get(1) < bs) bs = gf_prof_get(1);
        if (gf_prof_get(3) < bu) bu = gf_prof_get(3);
        if (gf_prof_get(2) < bt) bt = gf_prof_get(2);
    }
    { FILE *f = fopen("bench_3d.ppm", "wb"); fprintf(f, "P6\n%d %d\n255\n", gf_w, gf_h);
      for (int i = 0; i < gf_w * gf_h; i++) { uint16_t q = out[i]; unsigned char rgb[3] = { (q >> 11) << 3, ((q >> 5) & 63) << 2, (q & 31) << 3 }; fwrite(rgb, 1, 3, f); }
      fclose(f); }
    gf_view_t v; gf_view_set(&v, (w.tee_x + w.pin_x) / 2, (w.tee_y + w.pin_y) / 2, gf_w / 2.0f, gf_h / 2.0f, 3.0f, atan2f(w.pin_x - w.tee_x, w.pin_y - w.tee_y));
    double bm = 1e9;
    for (int i = 0; i < 3; i++) { t0 = now_ms(); gf_map_render(&w, &v, out, gf_w, gf_h, MAP_SHADE | MAP_TREES | MAP_MARKERS); double d = now_ms() - t0; if (d < bm) bm = d; }
    printf("%s hole %d: view3d %.1f ms (per part: ground %.0f sky %.0f up %.0f trees %.0f), map %.1f ms, albedo %.1f ms\n",
           land ? "land" : "port", hole, best, bg, bs, bu, bt, bm, talb);
    FILE *f = fopen("bench_v.ppm", "wb"); fprintf(f, "P6\n%d %d\n255\n", gf_w, gf_h);
    for (int i = 0; i < gf_w * gf_h; i++) { uint16_t q = out[i]; unsigned char rgb[3] = { (q >> 11) << 3, ((q >> 5) & 63) << 2, (q & 31) << 3 }; fwrite(rgb, 1, 3, f); }
    fclose(f);
    return 0;
}
