/*
 * CIELO - the catalogues (see cl_cat.h).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "cl_cat.h"
#include "cl_mem.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int16_t i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

/* a black body's colour for B-V (Ballesteros' temperature, then a table of
 * the colours of black bodies as a display shows them) */
uint32_t cl_bv_rgb(float bv)
{
    static const struct {
        float t;
        uint8_t r, g, b;
    } T[] = {
        { 2500, 255, 160, 70 },   { 3000, 255, 180, 107 }, { 4000, 255, 206, 160 }, { 5000, 255, 226, 200 },
        { 5800, 255, 240, 228 },  { 6500, 255, 249, 251 }, { 7500, 232, 238, 255 }, { 9000, 212, 224, 255 },
        { 12000, 190, 206, 255 }, { 20000, 168, 190, 255 }, { 40000, 155, 178, 255 },
    };
    if (bv < -0.4f) bv = -0.4f;
    if (bv > 2.0f) bv = 2.0f;
    float t = 4600.0f * (1.0f / (0.92f * bv + 1.7f) + 1.0f / (0.92f * bv + 0.62f));
    int n = (int)(sizeof T / sizeof T[0]);
    if (t <= T[0].t) return (uint32_t)T[0].r << 16 | (uint32_t)T[0].g << 8 | T[0].b;
    for (int i = 1; i < n; i++) {
        if (t <= T[i].t) {
            float k = (t - T[i - 1].t) / (T[i].t - T[i - 1].t);
            int r = (int)(T[i - 1].r + (T[i].r - T[i - 1].r) * k);
            int g = (int)(T[i - 1].g + (T[i].g - T[i - 1].g) * k);
            int b = (int)(T[i - 1].b + (T[i].b - T[i - 1].b) * k);
            return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
    return (uint32_t)T[n - 1].r << 16 | (uint32_t)T[n - 1].g << 8 | T[n - 1].b;
}

const char *cl_cat_str(const cl_cat_t *c, uint32_t off)
{
    if (!c->strs || off >= (uint32_t)c->nstrs) return "";
    return c->strs + off;
}

const char *cl_cat_star_name(const cl_cat_t *c, int i)
{
    if (i < 0 || i >= c->n || c->name[i] == 0xFFFF || c->name[i] >= c->nnames) return NULL;
    return cl_cat_str(c, c->names[c->name[i]]);
}

void cl_cat_star_cons(const cl_cat_t *c, int i, char out[4])
{
    out[0] = 0;
    const char *d = cl_cat_str(c, c->desig[i]);
    /* "Alp Ori · 58 Ori · HR 2061": the word before the first separator */
    const char *e = strstr(d, " \xC2\xB7");
    if (!e) return;
    if (e - d >= 3 && e[-4] == ' ') {
        memcpy(out, e - 3, 3);
        out[3] = 0;
    }
}

void cl_cat_free(cl_cat_t *c)
{
    cl_free(c->xyz);
    cl_free(c->mag);
    cl_free(c->rgb);
    cl_free(c->bv);
    cl_free(c->hr);
    cl_free(c->plx);
    cl_free(c->name);
    cl_free(c->desig);
    cl_free(c->spec);
    cl_free(c->names);
    cl_free(c->cons);
    cl_free(c->segs);
    cl_free(c->mess);
    cl_free(c->strs);
    cl_free(c->mw);
    memset(c, 0, sizeof *c);
}

typedef struct {
    const uint8_t *p;
    uint32_t len;
} sec_t;

static bool find(const uint8_t *d, uint32_t size, const char *tag, sec_t *s)
{
    uint32_t n = u32(d + 8);
    for (uint32_t i = 0; i < n && 12 + 12 * (i + 1) <= size; i++) {
        const uint8_t *e = d + 12 + 12 * i;
        if (memcmp(e, tag, 4)) continue;
        uint32_t off = u32(e + 4), len = u32(e + 8);
        if (off > size || len > size - off || len < 4) return false;
        s->p = d + off;
        s->len = len;
        return true;
    }
    return false;
}

bool cl_cat_load(cl_cat_t *c, const char *path)
{
    memset(c, 0, sizeof *c);
    FILE *f = fopen(path, "rb");
    if (!f) {
        aos_hal_log("cielo", "no %s: the sky pack is missing", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 64 || size > 8 * 1024 * 1024) {
        fclose(f);
        aos_hal_log("cielo", "%s: %ld bytes, not a pack", path, size);
        return false;
    }
    uint8_t *d = (uint8_t *)cl_malloc((size_t)size);
    if (!d) {
        fclose(f);
        return false;
    }
    bool ok = fread(d, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    sec_t st, sn, cs, sg, ms, ss;
    ok = ok && !memcmp(d, "CIELOPK1", 8) && find(d, (uint32_t)size, "STAR", &st) &&
         find(d, (uint32_t)size, "SNAM", &sn) && find(d, (uint32_t)size, "CONS", &cs) &&
         find(d, (uint32_t)size, "SEGS", &sg) && find(d, (uint32_t)size, "MESS", &ms) &&
         find(d, (uint32_t)size, "STRS", &ss);
    if (!ok) {
        aos_hal_log("cielo", "%s: not a sky pack of this version", path);
        cl_free(d);
        return false;
    }

    int n = (int)u32(st.p);
    if (n <= 0 || (uint32_t)n * 24 + 4 > st.len) n = 0;
    c->n = n;
    c->xyz = (float *)cl_malloc(sizeof(float) * 3 * (size_t)n);
    c->mag = (float *)cl_malloc(sizeof(float) * (size_t)n);
    c->rgb = (uint32_t *)cl_malloc(sizeof(uint32_t) * (size_t)n);
    c->bv = (int16_t *)cl_malloc(sizeof(int16_t) * (size_t)n);
    c->hr = (uint16_t *)cl_malloc(sizeof(uint16_t) * (size_t)n);
    c->plx = (uint16_t *)cl_malloc(sizeof(uint16_t) * (size_t)n);
    c->name = (uint16_t *)cl_malloc(sizeof(uint16_t) * (size_t)n);
    c->desig = (uint32_t *)cl_malloc(sizeof(uint32_t) * (size_t)n);
    c->spec = (uint8_t *)cl_malloc((size_t)n);
    ok = c->xyz && c->mag && c->rgb && c->bv && c->hr && c->plx && c->name && c->desig && c->spec;
    for (int i = 0; ok && i < n; i++) {
        const uint8_t *r = st.p + 4 + 24 * i;
        float x = i16(r) / 32767.0f, y = i16(r + 2) / 32767.0f, z = i16(r + 4) / 32767.0f;
        float k = 1.0f / sqrtf(x * x + y * y + z * z);
        c->xyz[3 * i] = x * k;
        c->xyz[3 * i + 1] = y * k;
        c->xyz[3 * i + 2] = z * k;
        c->mag[i] = i16(r + 6) / 100.0f;
        c->bv[i] = i16(r + 8);
        c->rgb[i] = cl_bv_rgb(c->bv[i] == 32767 ? 0.6f : c->bv[i] / 1000.0f);
        c->hr[i] = u16(r + 10);
        c->plx[i] = u16(r + 12);
        c->name[i] = u16(r + 14);
        c->desig[i] = u32(r + 16);
        c->spec[i] = r[20];
    }

    int nn = (int)u32(sn.p);
    if ((uint32_t)nn * 4 + 4 > sn.len) nn = 0;
    c->nnames = nn;
    c->names = (uint32_t *)cl_malloc(sizeof(uint32_t) * (size_t)(nn ? nn : 1));
    ok = ok && c->names;
    for (int i = 0; ok && i < nn; i++) c->names[i] = u32(sn.p + 4 + 4 * i);

    int nc = (int)u32(cs.p);
    if ((uint32_t)nc * 20 + 4 > cs.len) nc = 0;
    c->ncons = nc;
    c->cons = (struct cl_cons *)cl_malloc(sizeof(struct cl_cons) * (size_t)(nc ? nc : 1));
    ok = ok && c->cons;
    for (int i = 0; ok && i < nc; i++) {
        const uint8_t *r = cs.p + 4 + 20 * i;
        struct cl_cons *k = &c->cons[i];
        memcpy(k->abbr, r, 4);
        k->abbr[3] = 0;
        k->xyz[0] = i16(r + 4) / 32767.0f;
        k->xyz[1] = i16(r + 6) / 32767.0f;
        k->xyz[2] = i16(r + 8) / 32767.0f;
        k->first = u16(r + 10);
        k->nseg = u16(r + 12);
        k->latin = u32(r + 16);
    }

    int ng = (int)u32(sg.p);
    if ((uint32_t)ng * 4 + 4 > sg.len) ng = 0;
    c->nsegs = ng;
    c->segs = (uint16_t *)cl_malloc(sizeof(uint16_t) * 2 * (size_t)(ng ? ng : 1));
    ok = ok && c->segs;
    for (int i = 0; ok && i < ng; i++) {
        uint16_t a = u16(sg.p + 4 + 4 * i), b = u16(sg.p + 6 + 4 * i);
        c->segs[2 * i] = a < n ? a : 0;
        c->segs[2 * i + 1] = b < n ? b : 0;
    }
    for (int i = 0; ok && i < nc; i++)
        if (c->cons[i].first + c->cons[i].nseg > ng) c->cons[i].nseg = 0;

    int nm = (int)u32(ms.p);
    if ((uint32_t)nm * 16 + 4 > ms.len) nm = 0;
    c->nmess = nm;
    c->mess = (struct cl_mess *)cl_malloc(sizeof(struct cl_mess) * (size_t)(nm ? nm : 1));
    ok = ok && c->mess;
    for (int i = 0; ok && i < nm; i++) {
        const uint8_t *r = ms.p + 4 + 16 * i;
        struct cl_mess *m = &c->mess[i];
        m->m = r[0];
        m->type = r[1];
        int mg = i16(r + 2);
        m->mag = mg == 32767 ? 99 : mg / 100.0f;
        m->xyz[0] = i16(r + 4) / 32767.0f;
        m->xyz[1] = i16(r + 6) / 32767.0f;
        m->xyz[2] = i16(r + 8) / 32767.0f;
        m->size = u16(r + 10) / 10.0f;
        m->ngc = u32(r + 12);
    }

    c->nstrs = (int)ss.len;
    c->strs = (char *)cl_malloc(ss.len + 1);
    ok = ok && c->strs;
    if (ok) {
        memcpy(c->strs, ss.p, ss.len);
        c->strs[ss.len] = 0;
    }
    cl_free(d);
    if (!ok) {
        aos_hal_log("cielo", "%s: no memory for the catalogues", path);
        cl_cat_free(c);
        return false;
    }
    c->ok = true;
    aos_hal_log("cielo", "pack: %d stars, %d names, %d constellations, %d segments, %d Messier", c->n, c->nnames,
                c->ncons, c->nsegs, c->nmess);
    return true;
}

/* ---------------------------------------------------------------------------
 * The Milky Way, from a model
 * ------------------------------------------------------------------------- */

static float hash2(int x, int y, int s)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFF) / 65535.0f;
}

/* value noise on a grid wrapped in x (period px cells) */
static float vnoise(float x, float y, int px, int s)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    int x0 = ((ix % px) + px) % px, x1 = (x0 + 1) % px;
    float a = hash2(x0, iy, s), b = hash2(x1, iy, s), c = hash2(x0, iy + 1, s), d = hash2(x1, iy + 1, s);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float blob(float l, float b, float l0, float b0, float sl, float sb)
{
    float dl = l - l0;
    if (dl > 180) dl -= 360;
    if (dl < -180) dl += 360;
    float q = (dl * dl) / (sl * sl) + ((b - b0) * (b - b0)) / (sb * sb);
    return q > 20 ? 0 : expf(-0.5f * q);
}

bool cl_cat_make_milky_way(cl_cat_t *c)
{
    if (c->mw) return true;
    uint8_t *m = (uint8_t *)cl_malloc(CL_MW_W * CL_MW_H);
    if (!m) return false;
    for (int y = 0; y < CL_MW_H; y++) {
        float b = 90.0f - ((float)y + 0.5f) * 180.0f / CL_MW_H;
        for (int x = 0; x < CL_MW_W; x++) {
            float l = ((float)x + 0.5f) * 360.0f / CL_MW_W - 180.0f;       /* -180..180, 0 the centre */
            float al = fabsf(l);
            float sig = 4.5f + 6.0f * expf(-(l / 28.0f) * (l / 28.0f));
            float amp = 0.30f + 0.55f * expf(-(l / 70.0f) * (l / 70.0f));
            float band = amp * expf(-0.5f * (b * b) / (sig * sig));
            /* a thin brighter core along the plane */
            band += 0.25f * amp * expf(-0.5f * (b * b) / (1.8f * 1.8f));
            /* the star clouds */
            band += 0.55f * blob(l, b, 3, -4, 5, 4);         /* Sagittarius */
            band += 0.40f * blob(l, b, 27, -2, 3, 2.5f);     /* Scutum */
            band += 0.35f * blob(l, b, 78, 1, 8, 4);         /* Cygnus */
            band += 0.30f * blob(l, b, -73, -1, 6, 3.5f);    /* Carina */
            band += 0.25f * blob(l, b, -30, -1, 6, 3);       /* Norma */
            band += 0.20f * blob(l, b, -50, 0, 6, 3);        /* Centaurus, Crux */
            /* the dark lanes */
            if (l > 2 && l < 70) {
                float w = blob(l, b, 32, 2.5f, 26, 2.4f);    /* the Great Rift */
                band *= 1.0f - 0.65f * w;
            }
            band *= 1.0f - 0.7f * blob(l, b, -59, -1, 2.5f, 2.2f);    /* the Coalsack */
            band *= 1.0f - 0.4f * blob(l, b, 5, 8, 5, 4);             /* Ophiuchus' dust */
            /* the Magellanic Clouds */
            band += 0.75f * blob(l, b, -79.5f, -32.9f, 4.0f, 2.8f);
            band += 0.55f * blob(l, b, -57.2f, -44.3f, 1.7f, 1.2f);
            /* the anticentre is faint */
            if (al > 120) band *= 0.75f;
            /* the mottle: four octaves, wrapped in l */
            float u = ((float)x / CL_MW_W) * 32.0f, v = ((float)y / CL_MW_H) * 16.0f;
            float nz = 0, amp2 = 0.5f;
            int per = 32;
            for (int o = 0; o < 4; o++) {
                nz += amp2 * vnoise(u, v, per, o + 1);
                u *= 2;
                v *= 2;
                per *= 2;
                amp2 *= 0.5f;
            }
            band *= 0.45f + 1.1f * nz;
            if (band < 0) band = 0;
            float val = sqrtf(band) * 255.0f;
            if (val > 255) val = 255;
            m[y * CL_MW_W + x] = (uint8_t)(val < 10 ? 0 : val);
        }
    }
    c->mw = m;
    return true;
}

int cl_cat_mw(const cl_cat_t *c, float l, float b)
{
    if (!c->mw) return 0;
    /* l radians from the centre, -pi..pi */
    float fx = (l * 57.29578f + 180.0f) * (CL_MW_W / 360.0f) - 0.5f;
    float fy = (90.0f - b * 57.29578f) * (CL_MW_H / 180.0f) - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float ax = fx - (float)ix, ay = fy - (float)iy;
    if (iy < 0) iy = 0, ay = 0;
    if (iy >= CL_MW_H - 1) iy = CL_MW_H - 2, ay = 1;
    int x0 = ((ix % CL_MW_W) + CL_MW_W) % CL_MW_W, x1 = (x0 + 1) % CL_MW_W;
    const uint8_t *r0 = c->mw + iy * CL_MW_W, *r1 = r0 + CL_MW_W;
    float top = r0[x0] + (r0[x1] - r0[x0]) * ax, bot = r1[x0] + (r1[x1] - r1[x0]) * ax;
    return (int)(top + (bot - top) * ay);
}
