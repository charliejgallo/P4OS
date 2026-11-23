/*
 * TANQUES - the renderers on the Mac, to choose the 3D's road and guess the
 * board's numbers; and the twenty weapons, each fired once by the board's
 * aim, with what each did.
 *
 *   T=apps/tanques/main
 *   cc -O2 -DTQ_BENCH -DAOS_SIM -I$T -Iapps/visor3d/main -Icomponents/aos_hal/include \
 *      -Icomponents/aos_ui/include apps/tanques/tools/bench.c $T/tq_game.c $T/tq_data.c \
 *      $T/tq_scene.c $T/tq_gfx.c $T/tq_terrain.c $T/tq_art.c $T/tq_mem.c \
 *      apps/visor3d/main/v3_raster.c apps/visor3d/main/v3_mesh.c -o apps/tanques/build/bench -lm
 *   apps/tanques/build/bench <out dir>
 *
 * One core, the best of several runs of each thing. The same runs time
 * Visor 3D's rasteriser on its sample models, whose times on the board are
 * known (apps/visor3d/README.md): the ratio between the two is how the
 * Mac's numbers become the board's. Writes the frames as PPM.
 */
#include "tq_art.h"
#include "tq_game.h"
#include "tq_mem.h"
#include "tq_scene.h"
#include "v3_mesh.h"
#include "v3_raster.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the HAL, as far as these files need it */
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
const char *aos_hal_path_apps(void) { return "apps/tanques/build"; }
const char *aos_hal_path_data(void) { return "apps/tanques/build"; }
bool aos_hal_worker_split(void (*fn)(void *arg, int part), void *arg) { (void)fn; (void)arg; return false; }
const char *aos_tr(const char *s) { return s; }

static double ms(void) { return aos_hal_uptime_us() / 1000.0; }

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

/* A Visor 3D mesh from tanques_art.py's OBJ (v / f a b c region) */
static bool obj_mesh(const char *path, v3_mesh_t *m)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    int nv = 0, nt = 0, cv = 1024, ct = 1024;
    float *v = malloc(cv * 12);
    uint32_t *idx = malloc(ct * 12);
    uint16_t *fc = malloc(ct * 2);
    char line[160];
    static const uint16_t RC[16] = { 0, 0xF465, 0xFF13, 0x41E8, 0x9D15, 0xFF53, 0xFFBE };
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            if (nv == cv) { cv *= 2; v = realloc(v, cv * 12); }
            float x, y, z;
            sscanf(line + 2, "%f %f %f", &x, &y, &z);
            v[nv * 3] = x; v[nv * 3 + 1] = z; v[nv * 3 + 2] = -y;     /* Y up, as M3D */
            nv++;
        } else if (line[0] == 'f') {
            if (nt == ct) { ct *= 2; idx = realloc(idx, ct * 12); fc = realloc(fc, ct * 2); }
            unsigned a, b, c, r = 1;
            sscanf(line + 2, "%u %u %u %u", &a, &b, &c, &r);
            idx[nt * 3] = a - 1; idx[nt * 3 + 1] = b - 1; idx[nt * 3 + 2] = c - 1;
            fc[nt] = RC[r & 15];
            nt++;
        }
    }
    fclose(f);
    /* centre and radius 1, face normals: as v3_mesh_load leaves a mesh */
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) { lo[k] = fminf(lo[k], v[i * 3 + k]); hi[k] = fmaxf(hi[k], v[i * 3 + k]); }
    float c[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 }, rad = 0;
    for (int i = 0; i < nv; i++) {
        float d = 0;
        for (int k = 0; k < 3; k++) { v[i * 3 + k] -= c[k]; d += v[i * 3 + k] * v[i * 3 + k]; }
        if (d > rad) rad = d;
    }
    rad = sqrtf(rad);
    for (int i = 0; i < nv * 3; i++) v[i] /= rad;
    float *fn = malloc(nt * 12);
    for (int t = 0; t < nt; t++) {
        float *a = &v[idx[t * 3] * 3], *b = &v[idx[t * 3 + 1] * 3], *d = &v[idx[t * 3 + 2] * 3];
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f;
        for (int k = 0; k < 3; k++) fn[t * 3 + k] = n[k] / l;
    }
    memset(m, 0, sizeof *m);
    m->nv = nv; m->nt = nt; m->v = v; m->idx = idx; m->fn = fn; m->fc = fc; m->nt_file = nt;
    m->closed = false;
    return true;
}

/* Visor 3D's frame: best of n */
static double v3_time(v3_mesh_t *m, int w, int h, float scale, int n, uint16_t **keep)
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
    if (keep) *keep = fb;
    else free(fb);
    return best;
}

static double frame(tq_req_t *r, tq_terrain_t *t, uint16_t *fb, int n, tq_prof_t *best)
{
    double b = 1e9;
    for (int i = 0; i < n; i++) {
        tq_prof_t p;
        memset(&p, 0, sizeof p);
        double t0 = ms();
        tq_scene_render(r, t, fb, &p);
        double d = ms() - t0;
        if (d < b) {
            b = d;
            if (best) *best = p;
        }
    }
    return b;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    printf("== calibration: Visor 3D's rasteriser on this Mac (one core)\n");
    v3_mesh_t mila, mila_hd;
    double v3a = 0, v3b = 0;
    if (v3_mesh_load(&mila, "apps/visor3d/models/mila.m3d", NULL)) {
        v3a = v3_time(&mila, 288, 512, 1.0f, 40, NULL);
        printf("mila (%d tri) 288x512: %.2f ms  [board, two cores: ~35 ms]\n", mila.nt, v3a);
    }
    if (v3_mesh_load(&mila_hd, "apps/visor3d/models/mila_hd.m3d", NULL)) {
        v3b = v3_time(&mila_hd, 180, 320, 1.0f, 30, NULL);
        printf("mila_hd (%d tri) 180x320: %.2f ms  [board: 105 ms one core, 67 ms two]\n", mila_hd.nt, v3b);
    }
    double K = v3b > 0 ? 105.0 / v3b : 0;
    printf("-> one board core ~ %.0fx this Mac's core (mila_hd); mila says %.0fx for two cores\n", K,
           v3a > 0 ? 35.0 / v3a : 0);

    printf("\n== the tank as real-time triangles (Visor 3D's rasteriser, flat shaded)\n");
    const char *objs[3] = { "low", "mid", "full" };
    for (int k = 0; k < 3; k++) {
        char p[160];
        snprintf(p, sizeof p, "apps/tanques/build/art/mesh/tank_%s.obj", objs[k]);
        v3_mesh_t m;
        if (!obj_mesh(p, &m)) continue;
        uint16_t *fb = NULL;
        /* a tank 12 m off fills ~1/5 of the half-size frame's width */
        double a = v3_time(&m, 360, 640, 0.35f, 30, k == 0 ? &fb : NULL);
        double b = v3_time(&m, 360, 640, 1.0f, 30, NULL);
        printf("tank_%s %d tri at 360x640: %.2f ms far (small), %.2f ms near (filling) -> board ~%.0f / %.0f ms one core\n",
               objs[k], m.nt, a, b, a * K, b * K);
        if (fb) { ppm(out, "bench_tank_rt", fb, 360, 640); free(fb); }
    }

    printf("\n== the art\n");
    if (!tq_art_open()) {
        printf("no pack: python3 apps/tanques/tools/pack.py\n");
        return 1;
    }
    tq_setup_t su;
    memset(&su, 0, sizeof su);
    su.biome = 0;
    su.size = 2;
    su.diff = 1;
    su.seed = 7;
    su.ntank = 2;
    for (int k = 0; k < 2; k++) {
        su.t[k].type = k ? TQ_T_MAMUT : TQ_T_BOMBO;
        su.t[k].team = k;
        su.t[k].ai = k == 1;
        snprintf(su.t[k].name, sizeof su.t[k].name, "t%d", k);
        memset(su.t[k].load, 0xFF, sizeof su.t[k].load);
        su.t[k].load[0] = TQ_W_CANNON;
        su.t[k].load[1] = TQ_W_MORTAR;
    }
    double t0 = ms();
    tq_art_use_biome(su.biome);
    tq_art_use_tanks((1u << TQ_T_BOMBO) | (1u << TQ_T_MAMUT), 1u << TQ_T_BOMBO);
    printf("a battle's art (pradera, bombo with its gunner's view, mamut): %u KB in %.1f ms\n",
           (unsigned)(tq_art_bytes() / 1024), ms() - t0);

    tq_terrain_t *ter = malloc(sizeof *ter);
    tq_state_t *st = malloc(sizeof *st);
    t0 = ms();
    tq_game_new(st, ter, &su);
    printf("map: %.1f ms (%.0f m, %d trees, %d stones, the light)\n", ms() - t0, TQ_SIZE_M[su.size], st->ntree, st->nblock);
    if (!tq_scene_init()) return 1;
    printf("views' buffers: %u KB\n", (unsigned)(tq_scene_bytes() / 1024));
    static tq_lut_t lut[3];
    tq_art_tank_lut(&lut[0], 0xF58C29, 0xFAE09E, 2, 0xE5483B);
    tq_art_tank_lut(&lut[1], 0x478CEB, 0xF2F2FA, 4, 0x3A3D45);
    tq_art_tank_lut(&lut[2], 0x3A3634, 0x2A2624, -1, 0);

    uint16_t *fb = malloc(720 * 1280 * 2);
    tq_req_t *r = calloc(1, sizeof *r);
    r->lut[0] = &lut[0];
    r->lut[1] = &lut[1];
    r->wreck = &lut[2];
    r->rgb[0] = 0xF58C29;
    r->rgb[1] = 0x478CEB;
    int sizes[2][2] = { { 720, 1280 }, { 1280, 720 } };

    printf("\n== the battle view\n");
    for (int o = 0; o < 2; o++) {
        int W = sizes[o][0], H = sizes[o][1];
        r->mode = TQ_VIEW_TOP;
        r->w = W;
        r->h = H;
        r->cx = st->tank[0].x + 4;
        r->cy = st->tank[0].y + 6;
        r->hp_bars = true;
        memcpy(&r->st, st, sizeof *st);
        tq_game_reach(&r->st, ter);
        r->st.show_reach = true;
        int ap;
        tq_game_route(&r->st, ter, st->tank[0].x + 4, st->tank[0].y + 3, &ap);
        tq_scene_world_all();
        tq_prof_t p;
        memset(&p, 0, sizeof p);
        t0 = ms();
        tq_scene_render(r, ter, fb, &p);
        double first = ms() - t0;
        tq_scene_top_clamp(ter, W, H, &r->cx, &r->cy);
        double f = frame(r, ter, fb, 20, &p);
        printf("%dx%d: the whole world %.1f ms (once); a frame %.2f ms (%d things, reach tiles and path on)\n", W, H,
               first, f, p.sprites);
        char name[32];
        snprintf(name, sizeof name, "bench_top_%c", o ? 'l' : 'p');
        ppm(out, name, fb, W, H);
        r->st.show_reach = false;
        r->st.npath = 0;
        f = frame(r, ter, fb, 20, &p);
        printf("   the same without the reach: %.2f ms\n", f);
    }

    printf("\n== the gunner's view (half size each way, then doubled)\n");
    for (int o = 0; o < 2; o++) {
        int W = sizes[o][0], H = sizes[o][1];
        tq_tank_t *me = &st->tank[0];
        const tq_tank_t *en = &st->tank[1];
        me->tyaw = atan2f(en->y - me->y, en->x - me->x);
        me->pitch = 48 * TQ_DEG;
        me->sel = 1;
        const tq_tank_type_t *T = &TQ_TANK[me->type];
        r->mode = TQ_VIEW_FP;
        r->w = W;
        r->h = H;
        r->me = 0;
        r->eye = tq_fp_eye(me->x, me->y, tq_tank_z(ter, me), me->tyaw, T->eye_x, T->eye_y, T->eye_z);
        r->yaw = me->tyaw;
        r->pitch = TQ_FP_PITCH * TQ_DEG;
        st->cur = 0;
        tq_game_arc(st, ter);
        memcpy(&r->st, st, sizeof *st);
        tq_prof_t p;
        double f = frame(r, ter, fb, 20, &p);
        printf("%dx%d: %.2f ms a frame: sky + ground + things %.2f (%d triangles, %d things), doubling %.2f\n", W, H, f,
               p.base_us / 1000.0, p.tris, p.sprites, p.up_us / 1000.0);
        char name[32];
        snprintf(name, sizeof name, "bench_fp_%c", o ? 'l' : 'p');
        ppm(out, name, fb, W, H);
    }

    printf("\n== the twenty weapons, each fired by the board's aim at the mamut\n");
    for (int w = 0; w < TQ_WEAPONS; w++) {
        tq_setup_t s2 = su;
        s2.t[0].load[0] = (uint8_t)w;
        s2.t[0].load[1] = 0xFF;
        s2.t[1].type = TQ_T_MAMUT;
        tq_game_new(st, ter, &s2);
        /* close enough for every weapon's reach */
        st->tank[1].x = st->tank[0].x + 9;
        st->tank[1].y = st->tank[0].y + 3;
        st->cur = 0;
        st->tank[0].ap = 20;
        float yaw, pitch;
        int slot;
        tq_ai_plan_aim(st, ter, &yaw, &pitch, &slot);
        tq_tank_t *k = &st->tank[0];
        k->sel = 0;
        if (slot < 0) {
            yaw = atan2f(st->tank[1].y - k->y, st->tank[1].x - k->x);
            pitch = (TQ_WEAPON[w].min_pitch + TQ_WEAPON[w].max_pitch) * 0.5f * TQ_DEG;
        }
        k->tyaw = yaw;
        k->pitch = pitch;
        int hp0 = st->tank[1].hp, hpme = k->hp, trees0 = 0, blocks0 = st->nblock;
        float x0 = k->x, y0 = k->y;
        for (int i = 0; i < st->ntree; i++) trees0 += st->tree[i].alive;
        tq_event_t ev[TQ_MAX_EVENTS];
        int nev = 0, events = 0, types = 0;
        tq_game_fire(st, ter, ev, &nev);
        for (int i = 0; i < nev; i++) types |= 1 << ev[i].type;
        events += nev;
        float t = 0;
        while ((tq_game_flying(st) || t < 0.2f) && t < 12) {
            tq_game_step(st, ter, 1.0f / 30, ev, &nev);
            for (int i = 0; i < nev; i++) types |= 1 << ev[i].type;
            events += nev;
            t += 1.0f / 30;
        }
        int trees = 0, mines = 0, zones = 0, alive_blocks = 0;
        for (int i = 0; i < st->ntree; i++) trees += st->tree[i].alive;
        for (int i = 0; i < TQ_MAX_MINES; i++) mines += st->mine[i].live;
        for (int i = 0; i < TQ_MAX_ZONES; i++) zones += st->zone[i].live;
        for (int i = 0; i < st->nblock; i++) alive_blocks += st->block[i].alive;
        printf("%-15s aim %s: %d events (types 0x%03x) after %.1f s, damage to it %d, to me %d, trees -%d, blocks %d->%d, "
               "mines %d, zones %d, shield %d, its emp %d, moved %.1f m, hp me %d\n",
               TQ_WEAPON[w].name, slot >= 0 ? "ai  " : "flat", events, types, t, hp0 - st->tank[1].hp, hpme - k->hp,
               trees0 - trees, blocks0, alive_blocks, mines, zones, k->shield, st->tank[1].emp,
               sqrtf((k->x - x0) * (k->x - x0) + (k->y - y0) * (k->y - y0)), k->hp);
    }
    printf("\nboard factor used: %.0f\n", K);
    return 0;
}
