/*
 * PLANEO - the engine on the Mac: every world made, flown for a while by the
 * autopilot, pictures saved and frames timed; and Visor 3D's rasteriser
 * timed beside it, whose times on the board are known, so the Mac's numbers
 * can be turned into the board's (the same calibration as Tanques').
 *
 *   V=apps/vuelo/main
 *   cc -O2 -DVU_BENCH -DAOS_SIM -I$V -Iapps/visor3d/main -Icomponents/aos_hal/include \
 *      -Icomponents/aos_ui/include apps/vuelo/tools/bench.c $V/vu_world.c $V/vu_render.c \
 *      $V/vu_plane.c $V/vu_flight.c $V/vu_mem.c apps/visor3d/main/v3_raster.c \
 *      apps/visor3d/main/v3_mesh.c -o <dir>/bench -lm
 *   <dir>/bench <out dir> [W H] [biome] [hour] [seconds]
 *
 * One core. Writes PPM pictures of a few moments of each flight.
 */
#include "vu_flight.h"
#include "vu_mem.h"
#include "vu_plane.h"
#include "vu_render.h"
#include "vu_world.h"
#include "v3_mesh.h"
#include "v3_raster.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void aos_hal_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  [%s] ", tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

uint64_t aos_hal_uptime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint32_t vr_now_us(void)
{
    return (uint32_t)aos_hal_uptime_us();
}

const char *aos_hal_path_apps(void) { return "."; }
const char *aos_hal_path_data(void) { return "."; }
bool aos_hal_worker_split(void (*fn)(void *arg, int part), void *arg) { (void)fn; (void)arg; return false; }
const char *aos_tr(const char *s) { return s; }

static double ms(void) { return (double)aos_hal_uptime_us() / 1000.0; }

static void ppm(const char *dir, const char *name, const uint16_t *fb, int w, int h)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint16_t c = fb[i];
        uint8_t rgb[3] = { (uint8_t)((c >> 11) << 3 | (c >> 13)), (uint8_t)(((c >> 5) & 63) << 2 | ((c >> 9) & 3)),
                           (uint8_t)((c & 31) << 3 | ((c >> 2) & 7)) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static double v3_time(v3_mesh_t *m, int w, int h, float scale, int n)
{
    v3_scratch_t s;
    v3_scratch_alloc(&s, m->nv);
    uint16_t *fb = malloc((size_t)w * h * 2), *zb = malloc((size_t)w * h * 2);
    v3_box_t fd = { 0, 0, w, h }, zd = { 0, 0, w, h };
    v3_view_t view = { 0.6f, 0.35f, 0, scale, 0, 0, V3_SOLID, 0x808890 };
    double best = 1e9;
    for (int i = 0; i < n; i++) {
        view.yaw += 0.05f;
        double t0 = ms();
        v3_render(m, &view, &s, fb, zb, w, h, &fd, &zd);
        double d = ms() - t0;
        if (d < best) best = d;
    }
    v3_scratch_free(&s);
    free(zb);
    free(fb);
    return best;
}

typedef struct {
    vp_draw_t d;
} over_t;

static void over_fn(void *arg, uint16_t *out, int W, int H, int y0, int y1)
{
    over_t *o = (over_t *)arg;
    vp_draw(&o->d, out, W, H, y0, y1);
}

static const char *BIOME[] = { "islas", "canones", "nieve", "volcan", "ciudad" };
static const char *HOUR[] = { "manana", "mediodia", "tarde", "noche" };

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    int W = argc > 3 ? atoi(argv[2]) : 400, H = argc > 3 ? atoi(argv[3]) : 225;
    int only_b = argc > 4 ? atoi(argv[4]) : -1;
    int only_h = argc > 5 ? atoi(argv[5]) : -1;
    float secs = argc > 6 ? (float)atof(argv[6]) : 40.0f;
    double K = 0;
    {
        v3_mesh_t mila_hd;
        if (v3_mesh_load(&mila_hd, "apps/visor3d/models/mila_hd.m3d", NULL)) {
            double t = v3_time(&mila_hd, 180, 320, 1.0f, 30);
            K = 105.0 / t;
            printf("calibration: mila_hd 180x320 %.2f ms here, 105 ms on one board core -> x%.0f\n", t, K);
        }
    }
    static vw_world_t w;
    if (!vw_alloc(&w) || !vr_init(W, H)) {
        printf("no memory\n");
        return 1;
    }
    uint16_t *fb = malloc((size_t)W * H * 2);
    static over_t ov;
    for (int b = 0; b < VW_BIOMES; b++) {
        if (only_b >= 0 && b != only_b) continue;
        int hour = only_h >= 0 ? only_h : (b == VW_CANONES ? VW_EVENING : b == VW_NIEVE ? VW_MORNING : VW_NOON);
        double t0 = ms();
        vw_generate(&w, b, 7 + (uint32_t)b, hour, NULL, NULL);
        double tg = ms() - t0;
        printf("\n== %s, %s: made in %.0f ms (board ~%.1f s on two cores), highest %.0f m\n", BIOME[b], HOUR[hour], tg,
               tg * K / 2 / 1000.0, w.hmax);
        printf("  stages: heights %u, trees+shadows %u, colours %u, levels %u ms\n", w.gen_ms[0], w.gen_ms[1], w.gen_ms[2],
               w.gen_ms[3]);
        vf_plane_t pl;
        memset(&pl, 0, sizeof pl);
        vf_reset(&pl, &w);
        float dt = 1.0f / 30.0f;
        double sum = 0, worst = 0, ground = 0, turn = 0;
        uint32_t samples = 0, lines = 0;
        int frames = 0, shot = 0;
        for (float t = 0; t < secs; t += dt) {
            vf_autopilot(&pl, &w, t);
            vf_step(&pl, &w, dt);
            if (pl.crashed) {
                printf("  crashed at %.1f s\n", t);
                vf_reset(&pl, &w);
            }
            vr_cam_t cam;
            vf_camera(&pl, &w, dt, t, &cam);
            vr_proj_t pr;
            vr_setup(&cam, W, H, &pr);
            vp_pose_t pose;
            vf_pose(&pl, &pose);
            pose.paint[0] = 0xF2E6C8;
            pose.paint[1] = 0xD8352A;
            pose.paint[2] = 0xF2E6C8;
            vr_prof_t prof;
            double a = ms();
            vp_prepare(&ov.d, &pose, &pr, w.sun, w.sky_hor);
            vr_frame(&w, &cam, &pr, fb, over_fn, &ov, &prof);
            double d = ms() - a;
            sum += d;
            if (d > worst) worst = d;
            ground += prof.us_ground / 1000.0;
            turn += prof.us_turn / 1000.0;
            samples += prof.samples;
            lines += prof.lines;
            frames++;
            if (t >= (float)shot * secs / 4.0f + 2.0f && shot < 4) {
                char n[64];
                snprintf(n, sizeof n, "%s_%s_%d", BIOME[b], HOUR[hour], shot);
                ppm(out, n, fb, W, H);
                printf("  %s: agl %.0f m, bank %.0f, pitch %.0f, v %.0f km/h, up %dx%d\n", n, pl.agl,
                       pl.bank * 57.3f, pl.pitch * 57.3f, pl.v * 3.6f, prof.wu, prof.hu);
                shot++;
            }
        }
        double avg = sum / frames;
        printf("  %dx%d: %.2f ms a frame (worst %.2f), ground+sky %.2f, turn+plane %.2f, %u samples, %u lines\n", W, H,
               avg, worst, ground / frames, turn / frames, samples / (uint32_t)frames, lines / (uint32_t)frames);
        if (K > 0)
            printf("  board guess: %.1f ms one core, ~%.1f ms on two (%.0f fps), worst ~%.1f ms on two\n", avg * K,
                   avg * K / 1.75, 1000.0 / (avg * K / 1.75), worst * K / 1.75);
    }
    printf("\nPSRAM held: %u KB\n", (unsigned)(vu_mem_peak() / 1024));
    return 0;
}
