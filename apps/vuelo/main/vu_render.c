/*
 * PLANEO - the 3D (see vu_render.h)
 */
#include "vu_render.h"
#include "vu_mem.h"

#include <math.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define VR_MAXL     640         /* lines of equal depth, at most */
#define VR_ZNEAR    1.0f
#define VR_CLOUD_Z  640.0f      /* the cloud deck, metres */
#define VR_CLOUD_FAR 5000.0f

typedef struct {
    float   kf;                 /* pixels a height unit */
    int32_t u0, v0, du, dv;     /* 16.16 texels of the level */
    uint8_t lev, fog, refl, glint;
} line_t;

static uint16_t *s_up;          /* the upright picture */
static int16_t  *s_yb;          /* per upright column: the topmost row drawn */
static int16_t  *s_hd;          /* ... and the height drawn there */
static line_t   *s_line;
static float    *s_lz;          /* the lines' depths, made once */
static int       s_nlz;
static int       s_cap;         /* the upright side the buffers hold */
static vw_split_fn s_split;
static uint8_t   s_wave[64 * 64];

uint32_t vr_now_us(void);

void vr_set_split(vw_split_fn split)
{
    s_split = split;
}

static void make_lines(void)
{
    float z = VR_ZNEAR;
    s_nlz = 0;
    while (z < 1400.0f && s_nlz < VR_MAXL) {
        s_lz[s_nlz++] = z;
        float dz = z * (0.0115f + z * 0.000016f);
        if (dz < 0.2f) dz = 0.2f;
        z += dz;
    }
}

bool vr_init(int maxW, int maxH)
{
    int d = (int)ceilf(sqrtf((float)(maxW * maxW + maxH * maxH))) + 6;
    d = (d + 7) & ~7;
    if (s_up && s_cap >= d) return true;
    vr_free();
    s_up = (uint16_t *)vu_malloc((size_t)d * d * 2);
    s_yb = (int16_t *)vu_malloc((size_t)d * 2);
    s_hd = (int16_t *)vu_malloc((size_t)d * 2);
    s_line = (line_t *)vu_malloc(sizeof(line_t) * VR_MAXL);
    s_lz = (float *)vu_malloc(sizeof(float) * VR_MAXL);
    if (!s_up || !s_yb || !s_hd || !s_line || !s_lz) {
        vr_free();
        return false;
    }
    s_cap = d;
    make_lines();
    /* the water's ripples: a soft tiling noise of 64 x 64 */
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float v = sinf((float)x * 0.19635f * 2) * 0.5f + sinf((float)(x + y) * 0.19635f * 3) * 0.3f +
                      sinf((float)(y * 2 - x) * 0.19635f) * 0.4f;
            uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663);
            h = (h ^ (h >> 13)) * 0x5BD1E995u;
            v += (float)((h >> 8) & 255) / 255.0f * 0.5f;
            int q = (int)((v + 1.2f) * 9.0f);
            s_wave[y * 64 + x] = (uint8_t)(q < 0 ? 0 : q > 31 ? 31 : q);
        }
    }
    return true;
}

void vr_free(void)
{
    vu_free(s_up);
    vu_free(s_yb);
    vu_free(s_hd);
    vu_free(s_line);
    vu_free(s_lz);
    s_up = NULL;
    s_yb = s_hd = NULL;
    s_line = NULL;
    s_lz = NULL;
    s_cap = 0;
}

/* --------------------------------------------------------------------------
 * The projection
 * -------------------------------------------------------------------------- */

void vr_setup(const vr_cam_t *cam, int W, int H, vr_proj_t *p)
{
    memset(p, 0, sizeof *p);
    p->cx = cam->x;
    p->cy = cam->y;
    p->cz = cam->z;
    p->fx = cosf(cam->yaw);
    p->fy = sinf(cam->yaw);
    p->rx = p->fy;
    p->ry = -p->fx;
    p->W = W;
    p->H = H;
    p->f = (float)W * 0.5f / cam->tanfov;
    float c = cosf(cam->roll), s = -sinf(cam->roll);
    if (fabsf(cam->roll) < 0.002f) {
        c = 1;
        s = 0;
    }
    p->c = c;
    p->s = s;
    /* the upright picture: the turned output's bounding box, and a margin */
    float ac = fabsf(c), as = fabsf(s);
    int wu = (int)ceilf((float)W * ac + (float)H * as) + 4;
    int hu = (int)ceilf((float)W * as + (float)H * ac) + 4;
    if (s == 0) {
        wu = W;
        hu = H;
    }
    wu = (wu + 1) & ~1;
    if (wu > s_cap) wu = s_cap;
    if (hu > s_cap) hu = s_cap;
    p->Wu = wu;
    p->Hu = hu;
    p->cxu = (float)wu * 0.5f;
    p->cyu = (float)hu * 0.5f;
    p->cxo = (float)W * 0.5f;
    p->cyo = (float)H * 0.5f;
    p->hor = p->cyu + p->f * tanf(cam->pitch);
}

static inline float wrapd(float d)
{
    if (d > VW_SIZE * 0.5f) d -= VW_SIZE;
    if (d < -VW_SIZE * 0.5f) d += VW_SIZE;
    return d;
}

bool vr_project(const vr_proj_t *p, float x, float y, float z, float *sx, float *sy, float *depth)
{
    float dx = wrapd(x - p->cx), dy = wrapd(y - p->cy), dz = z - p->cz;
    float d = dx * p->fx + dy * p->fy;
    if (d < 0.5f) return false;
    float lat = dx * p->rx + dy * p->ry;
    float xu = p->f * lat / d, yu = p->hor - p->f * dz / d - p->cyu;
    /* upright -> output: the inverse of the turn the output samples with */
    *sx = p->cxo + xu * p->c - yu * p->s;
    *sy = p->cyo + xu * p->s + yu * p->c;
    if (depth) *depth = d;
    return true;
}

/* --------------------------------------------------------------------------
 * A frame
 * -------------------------------------------------------------------------- */

typedef struct {
    const vw_world_t *w;
    const vr_cam_t   *cam;
    const vr_proj_t  *p;
    uint16_t         *out;
    int               nl, l0;
    int               camh;     /* units */
    uint16_t          fog, sky_top, wrefl, cloud_lit, cloud_dark, sun_col;
    float             sun_x, sun_y, sun_r;
    bool              sun_on;
    vr_over_fn        over;
    void             *over_arg;
    uint32_t          samples[2], spans[2];
    float             wave_t;
} job_t;

/* The ground: columns x0..x1-1 of the upright picture */
static void ground_half(job_t *j, int part)
{
    const vr_proj_t *p = j->p;
    int Wu = p->Wu, Hu = p->Hu;
    int x0 = part ? Wu / 2 : 0, x1 = part ? Wu : Wu / 2;
    int16_t *yb = s_yb, *hd = s_hd;
    for (int x = x0; x < x1; x++) {
        yb[x] = (int16_t)Hu;
        hd[x] = -32000;
    }
    int open = x1 - x0;
    uint32_t samples = 0, spans = 0;
    float hor = p->hor;
    float camh = (float)j->camh;
    uint16_t fog = j->fog, wrefl = j->wrefl;
    int wt = (int)(j->wave_t * 64.0f);
    for (int li = j->l0; li < j->nl && open > 0; li++) {
        const line_t *L = &s_line[li];
        int lg = VW_LG - L->lev;
        uint32_t mk = (1u << lg) - 1;
        const uint32_t *map = j->w->lev[L->lev];
        int32_t du = L->du, dv = L->dv;
        int32_t u = L->u0 + du * x0, v = L->v0 + dv * x0;
        float kf = L->kf, base = hor + camh * kf;
        int fa = L->fog, ra = L->refl, ga = L->glint;
        for (int x = x0; x < x1; x++, u += du, v += dv) {
            int yt = yb[x];
            if (yt <= 0) continue;
            uint32_t t = map[vw_idx(((uint32_t)u >> 16) & mk, ((uint32_t)v >> 16) & mk, lg)];
            int h = (int)((t >> 16) & VW_HMASK);
            int y = (int)(base - (float)h * kf);
            samples++;
            if (y >= yt) continue;
            uint16_t c = (uint16_t)t;
            if (t & VW_WATER) {
                /* the sky in it far away, ripples near */
                if (ga) {
                    int wv = s_wave[(((uint32_t)v >> 13) & 63) * 64 + ((((uint32_t)u >> 13) + (uint32_t)wt) & 63)];
                    c = vr_blend(c, 0xFFFF, (wv * ga) >> 6);
                }
                c = vr_blend(c, wrefl, ra);
            } else if (h - hd[x] > 40 && yt - y > 2 && hd[x] > -32000) {
                /* a wall or a cliff: what rose since the last sample in this column */
                c = vr_blend(c, 0, 9);
            }
            if (fa) c = vr_blend(c, fog, fa);
            if (y < 0) {
                y = 0;
                open--;
            }
            uint16_t *d = s_up + (size_t)y * Wu + x;
            for (int n = yt - y; n > 0; n--, d += Wu) *d = c;
            yb[x] = (int16_t)y;
            hd[x] = (int16_t)h;
            spans++;
        }
    }
    j->samples[part] = samples;
    j->spans[part] = spans;
}

/* The sky over what the ground left, columns x0..x1-1 */
static void sky_half(job_t *j, int part)
{
    const vr_proj_t *p = j->p;
    const vw_world_t *w = j->w;
    int Wu = p->Wu, Hu = p->Hu;
    int x0 = part ? Wu / 2 : 0, x1 = part ? Wu : Wu / 2;
    int16_t *yb = s_yb;
    int ytop = Hu;
    for (int x = x0; x < x1; x++)
        if (yb[x] > 0 && Hu > 0) {
            ytop = 0;
            break;
        }
    if (ytop) return;
    int ymax = 0;
    for (int x = x0; x < x1; x++)
        if (yb[x] > ymax) ymax = yb[x];
    const uint8_t *cl = vw_clouds();
    float f = p->f, hor = p->hor;
    float cover_far = VR_CLOUD_FAR;
    uint16_t top = j->sky_top, fog = j->fog, clit = j->cloud_lit, cdark = j->cloud_dark;
    float cloud_h = VR_CLOUD_Z - j->cam->z;
    float drift = j->cam->time * 6.0f;
    bool stars = w->stars;
    for (int y = 0; y < ymax; y++) {
        float e = (hor - (float)y) / f;        /* tan of the elevation */
        uint16_t sky;
        if (e <= 0) {
            sky = fog;
        } else {
            float t = e * 1.6f;
            if (t > 1) t = 1;
            t = sqrtf(t);
            sky = vr_blend(fog, top, (int)(t * 32.0f));
        }
        uint16_t *row = s_up + (size_t)y * Wu;
        if (e > 0.02f && cl && cloud_h > 10.0f) {
            /* the cloud deck: a ceiling at a fixed height, linear along the row */
            float s = cloud_h / e;
            int fade = (int)((1.0f - s / cover_far) * 40.0f);
            if (fade > 32) fade = 32;
            if (fade > 0) {
                float k = s / f / VW_CLOUD_M * 65536.0f;
                float bx = (p->cx + s * p->fx + drift) / VW_CLOUD_M * 65536.0f;
                float by = (p->cy + s * p->fy) / VW_CLOUD_M * 65536.0f;
                int32_t du = (int32_t)(p->rx * k), dv = (int32_t)(p->ry * k);
                int32_t u = (int32_t)(bx + (float)((float)x0 - p->cxu) * p->rx * k);
                int32_t v = (int32_t)(by + (float)((float)x0 - p->cxu) * p->ry * k);
                for (int x = x0; x < x1; x++, u += du, v += dv) {
                    if (y >= yb[x]) continue;
                    int d = cl[((((uint32_t)v >> 16) & 255) << 8) | (((uint32_t)u >> 16) & 255)];
                    uint16_t c = sky;
                    if (stars && d < 8) {
                        uint32_t hs = ((uint32_t)u >> 12) * 0x9E3779B1u ^ ((uint32_t)v >> 12) * 0x85EBCA77u;
                        if ((hs >> 24) == 0x5A) c = 0xFFFF;
                    }
                    if (d) {
                        uint16_t cc = vr_blend(clit, cdark, d >> 4);
                        c = vr_blend(c, cc, (d * fade) >> 8);
                    }
                    row[x] = c;
                }
                continue;
            }
        }
        for (int x = x0; x < x1; x++)
            if (y < yb[x]) row[x] = sky;
    }
    /* the sun (or the moon): a glow and a disc */
    if (j->sun_on) {
        float r = j->sun_r, R = r * 4.0f;
        int ya = (int)(j->sun_y - R), yz = (int)(j->sun_y + R) + 1;
        int xa = (int)(j->sun_x - R), xz = (int)(j->sun_x + R) + 1;
        if (xa < x0) xa = x0;
        if (xz > x1) xz = x1;
        if (ya < 0) ya = 0;
        if (yz > Hu) yz = Hu;
        float ir = 1.0f / R;
        for (int y = ya; y < yz; y++) {
            uint16_t *row = s_up + (size_t)y * Wu;
            float dy = (float)y - j->sun_y;
            for (int x = xa; x < xz; x++) {
                if (y >= yb[x]) continue;
                float dx = (float)x - j->sun_x;
                float d = sqrtf(dx * dx + dy * dy);
                int a;
                if (d < r) a = 32;
                else {
                    float g = 1.0f - d * ir;
                    if (g <= 0) continue;
                    a = (int)(g * g * 20.0f);
                }
                row[x] = vr_blend(row[x], j->sun_col, a);
            }
        }
    }
}

static void ground_sky(void *arg, int part)
{
    job_t *j = (job_t *)arg;
    ground_half(j, part);
    sky_half(j, part);
}

/* The turn into the output, rows y0..y1-1, and what goes over it */
static void turn_half(void *arg, int part)
{
    job_t *j = (job_t *)arg;
    const vr_proj_t *p = j->p;
    int W = p->W, H = p->H, Wu = p->Wu;
    int y0 = part ? H / 2 : 0, y1 = part ? H : H / 2;
    if (p->s == 0 && p->Wu == W && p->Hu == H) {
        memcpy(j->out + (size_t)y0 * W, s_up + (size_t)y0 * W, (size_t)(y1 - y0) * W * 2);
    } else {
        int32_t dux = (int32_t)(p->c * 65536.0f), dvx = (int32_t)(-p->s * 65536.0f);
        for (int y = y0; y < y1; y++) {
            float dy = (float)y + 0.5f - p->cyo, dx = 0.5f - p->cxo;
            float uf = p->cxu + dx * p->c + dy * p->s;
            float vf = p->cyu - dx * p->s + dy * p->c;
            int32_t u = (int32_t)(uf * 65536.0f), v = (int32_t)(vf * 65536.0f);
            uint16_t *o = j->out + (size_t)y * W;
            for (int x = 0; x < W; x++, u += dux, v += dvx) o[x] = s_up[(size_t)(v >> 16) * Wu + (u >> 16)];
        }
    }
    if (j->over) j->over(j->over_arg, j->out, W, H, y0, y1);
}

void vr_frame(const vw_world_t *w, const vr_cam_t *cam, const vr_proj_t *p, uint16_t *out,
              vr_over_fn over, void *over_arg, vr_prof_t *prof)
{
    uint32_t t0 = vr_now_us();
    job_t j;
    memset(&j, 0, sizeof j);
    j.w = w;
    j.cam = cam;
    j.p = p;
    j.out = out;
    j.over = over;
    j.over_arg = over_arg;
    j.camh = (int)(cam->z * VW_HU);
    j.fog = vr_565(w->fog);
    j.sky_top = vr_565(w->sky_top);
    j.cloud_lit = vr_565(w->cloud_lit);
    j.cloud_dark = vr_565(w->cloud_dark);
    j.sun_col = vr_565(w->hour == VW_NIGHT ? 0xF0F0E0 : w->sun_col | 0x404040);
    j.wrefl = vr_blend(j.fog, j.sky_top, 10);
    j.wave_t = cam->time * 0.35f;

    /* the lines: from the first that can show anything to the fog's end */
    float f = p->f;
    float zfar = w->fog_far;
    float above = cam->z - w->hmax;
    float zfirst = VR_ZNEAR;
    if (above > 0 && (float)p->Hu > p->hor) zfirst = above * f / ((float)p->Hu - p->hor);
    float inv_texm = 1.0f / VW_TEXM;
    int nl = 0, l0 = 0;
    for (int i = 0; i < s_nlz && s_lz[i] < zfar; i++) {
        float z = s_lz[i];
        if (z < zfirst) {
            l0 = i + 1;
            continue;
        }
        line_t *L = &s_line[i];
        L->kf = f / z / VW_HU;
        float spacing = z / f * inv_texm;       /* texels of level 0 between two columns */
        int lev = 0;
        while (lev < VW_LEVELS - 1 && spacing > (float)(2 << lev) * 0.75f) lev++;
        float sc = inv_texm / (float)(1 << lev) * 65536.0f;
        float lx = p->cx + p->fx * z - p->rx * (p->cxu / f) * z;
        float ly = p->cy + p->fy * z - p->ry * (p->cxu / f) * z;
        L->u0 = (int32_t)(lx * sc);
        L->v0 = (int32_t)(ly * sc);
        L->du = (int32_t)(p->rx * (z / f) * sc);
        L->dv = (int32_t)(p->ry * (z / f) * sc);
        L->lev = (uint8_t)lev;
        float fa = (z - w->fog_near) / (zfar - w->fog_near);
        L->fog = (uint8_t)(fa <= 0 ? 0 : fa >= 1 ? 32 : (int)(fa * fa * 0.3f * 32.0f + fa * 0.7f * 32.0f));
        float ra = z / 450.0f;
        L->refl = (uint8_t)(ra >= 1 ? 22 : (int)(ra * 22.0f) + 4);
        L->glint = (uint8_t)(z < 160.0f ? (int)((1.0f - z / 160.0f) * 10.0f) : 0);
        nl = i + 1;
    }
    j.nl = nl;
    j.l0 = l0;

    /* the sun's place in the upright picture */
    {
        const float *sd = w->sun;
        float d = sd[0] * p->fx + sd[1] * p->fy;
        if (d > 0.05f) {
            float lat = sd[0] * p->rx + sd[1] * p->ry;
            j.sun_x = p->cxu + f * lat / d;
            j.sun_y = p->hor - f * sd[2] / d;
            j.sun_r = f * 0.018f;
            j.sun_on = true;
        }
    }

    uint32_t t1 = vr_now_us();
    if (!s_split || !s_split(ground_sky, &j)) {
        ground_sky(&j, 0);
        ground_sky(&j, 1);
    }
    uint32_t t2 = vr_now_us();
    if (!s_split || !s_split(turn_half, &j)) {
        turn_half(&j, 0);
        turn_half(&j, 1);
    }
    uint32_t t3 = vr_now_us();
    if (prof) {
        prof->us_ground = t2 - t1;
        prof->us_turn = t3 - t2;
        prof->us_total = t3 - t0;
        prof->samples = j.samples[0] + j.samples[1];
        prof->spans = j.spans[0] + j.spans[1];
        prof->lines = (uint32_t)(nl - l0);
        prof->wu = p->Wu;
        prof->hu = p->Hu;
    }
}
