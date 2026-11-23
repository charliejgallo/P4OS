/*
 * TANQUES - the art pack (see tq_art.h)
 */
#include "tq_art.h"
#include "tq_mem.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define PACK_NAME "tanques_p4.pak"
#define VERSION   2

typedef struct {
    char     name[24];
    uint32_t off, size;
} entry_t;

static uint8_t *s_pak;
static size_t s_pak_size;
static tq_spr_t s_tank[TQ_SETS][TQ_PARTS][TQ_YAWS][TQ_MIPS];
static tq_spr_t s_prop[TQ_SETS][TQ_PROPS][8][TQ_MIPS];
static tq_spr_t s_roof, s_barrel[TQ_PITCHES];
static uint8_t *s_mips[512];
static int s_nmips;
static tq_lut_t *s_lut;
static bool s_ready;

/* The paints, region by region (tools/pack.py, FIXED and SCHEMES) */
static const uint32_t FIXED[16] = {
    [3] = 0x403D45, [4] = 0x9EA3AD, [5] = 0xFFEB99, [6] = 0xF7F7F2,
    [8] = 0x66B847, [9] = 0x855733, [10] = 0xB3AB9E, [11] = 0xEBC485,
};
static const uint32_t SCHEME[TQ_SCHEMES][2] = {
    { 0xF58C29, 0xFAE09E },     /* orange and cream */
    { 0x478CEB, 0xF2F2FA },     /* blue and white */
    { 0x7AB34D, 0xEDCC66 },     /* green and sand */
};

uint32_t tq_art_scheme_rgb(int scheme)
{
    return SCHEME[scheme % TQ_SCHEMES][0];
}

static inline int mini_(int a, int b) { return a < b ? a : b; }
static inline int maxi_(int a, int b) { return a > b ? a : b; }

static bool parse(const uint8_t *b, uint32_t size, tq_spr_t *s)
{
    if (size < 16 || memcmp(b, "TSPR", 4)) return false;
    uint16_t w, h;
    int16_t ax, ay;
    uint32_t n;
    memcpy(&w, b + 4, 2);
    memcpy(&h, b + 6, 2);
    memcpy(&ax, b + 8, 2);
    memcpy(&ay, b + 10, 2);
    memcpy(&n, b + 12, 4);
    bool noz = n & 0x80000000u;
    n &= 0x7FFFFFFFu;
    size_t need = 16 + (size_t)h * sizeof(tq_row_t) + (size_t)n * (noz ? 3 : 4);
    if (need > size) return false;
    s->w = (int16_t)w;
    s->h = (int16_t)h;
    s->ax = ax;
    s->ay = ay;
    s->rows = (const tq_row_t *)(b + 16);
    s->L = b + 16 + (size_t)h * sizeof(tq_row_t);
    s->A = s->L + n;
    s->I = s->A + n;
    s->Z = noz ? NULL : s->I + n;
    return true;
}

/* A half-size copy, packed like the pack's: coverage and shade averaged,
 * the region and depth of the most covered source pixel */
static bool half(const tq_spr_t *src, tq_spr_t *dst)
{
    int w = (src->w + 1) / 2, h = (src->h + 1) / 2;
    /* the runs first, to size the planes */
    size_t n = 0;
    for (int y = 0; y < h; y++) {
        int lo = 1 << 30, hi = -1;
        for (int k = 0; k < 2; k++) {
            int sy = 2 * y + k;
            if (sy >= src->h || !src->rows[sy].len) continue;
            lo = mini_(lo, src->rows[sy].x0 / 2);
            hi = maxi_(hi, (src->rows[sy].x0 + src->rows[sy].len + 1) / 2);
        }
        if (hi > lo) n += (size_t)(hi - lo);
    }
    if (s_nmips >= (int)(sizeof s_mips / sizeof s_mips[0])) return false;
    uint8_t *m = (uint8_t *)tq_malloc((size_t)h * sizeof(tq_row_t) + 4 * (n ? n : 1));
    if (!m) return false;
    s_mips[s_nmips++] = m;
    tq_row_t *rows = (tq_row_t *)m;
    uint8_t *L = m + (size_t)h * sizeof(tq_row_t), *A = L + n, *I = A + n, *Z = I + n;
    uint32_t off = 0;
    for (int y = 0; y < h; y++) {
        int lo = 1 << 30, hi = -1;
        for (int k = 0; k < 2; k++) {
            int sy = 2 * y + k;
            if (sy >= src->h || !src->rows[sy].len) continue;
            lo = mini_(lo, src->rows[sy].x0 / 2);
            hi = maxi_(hi, (src->rows[sy].x0 + src->rows[sy].len + 1) / 2);
        }
        if (hi <= lo) {
            rows[y].x0 = 0;
            rows[y].len = 0;
            rows[y].off = off;
            continue;
        }
        rows[y].x0 = (uint16_t)lo;
        rows[y].len = (uint16_t)(hi - lo);
        rows[y].off = off;
        for (int x = lo; x < hi; x++) {
            int asum = 0, lsum = 0, best = -1, bi = 0, bz = 128;
            for (int k = 0; k < 4; k++) {
                int sx = 2 * x + (k & 1), sy = 2 * y + (k >> 1);
                if (sy >= src->h) continue;
                const tq_row_t *r = &src->rows[sy];
                if (sx < r->x0 || sx >= r->x0 + r->len) continue;
                int i = (int)r->off + sx - r->x0;
                int a = src->A[i];
                asum += a;
                lsum += a * src->L[i];
                if (a > best) {
                    best = a;
                    bi = src->I[i];
                    bz = src->Z ? src->Z[i] : 128;
                }
            }
            A[off] = (uint8_t)(asum / 4);
            L[off] = (uint8_t)(asum ? lsum / asum : 0);
            I[off] = (uint8_t)bi;
            Z[off] = (uint8_t)bz;
            off++;
        }
    }
    dst->w = (int16_t)w;
    dst->h = (int16_t)h;
    dst->ax = (int16_t)(src->ax / 2);
    dst->ay = (int16_t)(src->ay / 2);
    dst->rows = rows;
    dst->L = L;
    dst->A = A;
    dst->I = I;
    dst->Z = Z;
    return true;
}

static void mips(tq_spr_t *m)
{
    for (int k = 1; k < TQ_MIPS; k++) {
        if (!m[k - 1].w || !half(&m[k - 1], &m[k])) m[k] = m[k - 1];
    }
}

static const char *PROP_NAME[TQ_PROPS] = { "tree_round", "tree_pine", "stump", "block1", "block2" };

bool tq_art_load(void)
{
    if (s_ready) return true;
    char path[160];
    snprintf(path, sizeof path, "%s/%s", aos_hal_path_apps(), PACK_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) {
        aos_hal_log("tanques", "no pack: %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint64_t t0 = aos_hal_uptime_us();
    s_pak = size > 12 ? (uint8_t *)tq_malloc((size_t)size) : NULL;
    if (!s_pak || fread(s_pak, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        aos_hal_log("tanques", "pack: cannot read %ld bytes", size);
        tq_art_free();
        return false;
    }
    fclose(f);
    s_pak_size = (size_t)size;
    uint32_t ver, count;
    memcpy(&ver, s_pak + 4, 4);
    memcpy(&count, s_pak + 8, 4);
    if (memcmp(s_pak, "TQPK", 4) || ver != VERSION || 12 + (size_t)count * 32 > s_pak_size) {
        aos_hal_log("tanques", "pack: not a version %d pack", VERSION);
        tq_art_free();
        return false;
    }
    int found = 0;
    for (uint32_t k = 0; k < count; k++) {
        entry_t e;
        memcpy(&e, s_pak + 12 + k * 32, sizeof e);
        e.name[23] = 0;
        if ((size_t)e.off + e.size > s_pak_size) continue;
        tq_spr_t s;
        if (!parse(s_pak + e.off, e.size, &s)) continue;
        int set = !strncmp(e.name, "top/", 4) ? TQ_SET_TOP : (!strncmp(e.name, "low/", 4) ? TQ_SET_LOW : -1);
        const char *n = e.name + 4;                     /* after "top/" or "low/" */
        int idx;
        if (set >= 0 && sscanf(n, "hull_%d", &idx) == 1 && idx >= 0 && idx < TQ_YAWS) {
            s_tank[set][TQ_PART_HULL][idx][0] = s;
        } else if (set >= 0 && sscanf(n, "turret_%d", &idx) == 1 && idx >= 0 && idx < TQ_YAWS) {
            s_tank[set][TQ_PART_TURRET][idx][0] = s;
        } else if (!strncmp(e.name, "fp/", 3)) {
            if (!strcmp(e.name + 3, "roof")) s_roof = s;
            else if (sscanf(e.name + 3, "barrel_%d", &idx) == 1 && idx >= 0 && idx < TQ_PITCHES) s_barrel[idx] = s;
        } else if (set >= 0) {
            for (int p = 0; p < TQ_PROPS; p++) {
                size_t l = strlen(PROP_NAME[p]);
                if (strncmp(n, PROP_NAME[p], l)) continue;
                if (n[l] == 0) {
                    for (int y = 0; y < 8; y++) s_prop[set][p][y][0] = s;
                } else if (n[l] == '_' && sscanf(n + l + 1, "%d", &idx) == 1 && idx >= 0 && idx < 8) {
                    s_prop[set][p][idx][0] = s;
                }
            }
        } else {
            continue;
        }
        found++;
    }
    /* the smaller copies, for the first person's distance */
    for (int p = 0; p < TQ_PARTS; p++)
        for (int y = 0; y < TQ_YAWS; y++) mips(s_tank[TQ_SET_LOW][p][y]);
    for (int p = 0; p < TQ_PROPS; p++) {
        for (int y = 0; y < 8; y++) {
            if (y > 0 && s_prop[TQ_SET_LOW][p][y][0].L == s_prop[TQ_SET_LOW][p][0][0].L) {
                memcpy(s_prop[TQ_SET_LOW][p][y], s_prop[TQ_SET_LOW][p][0], sizeof s_prop[0][0][0]);
                continue;
            }
            mips(s_prop[TQ_SET_LOW][p][y]);
        }
    }
    s_lut = (tq_lut_t *)tq_malloc(sizeof(tq_lut_t) * TQ_SCHEMES);
    if (!s_lut) {
        tq_art_free();
        return false;
    }
    for (int k = 0; k < TQ_SCHEMES; k++) {
        uint32_t c[16];
        memcpy(c, FIXED, sizeof c);
        c[1] = SCHEME[k][0];
        c[2] = SCHEME[k][1];
        tq_lut_build(&s_lut[k], c);
    }
    s_ready = found > 0 && s_tank[TQ_SET_TOP][0][0][0].w;
    aos_hal_log("tanques", "pack: %d sprites, %u KB + %u KB of smaller copies, %u ms", found,
                (unsigned)(s_pak_size / 1024), (unsigned)((tq_mem_used() - s_pak_size) / 1024),
                (unsigned)((aos_hal_uptime_us() - t0) / 1000));
    return s_ready;
}

void tq_art_free(void)
{
    for (int i = 0; i < s_nmips; i++) tq_free(s_mips[i]);
    s_nmips = 0;
    tq_free(s_pak);
    tq_free(s_lut);
    s_pak = NULL;
    s_lut = NULL;
    s_pak_size = 0;
    memset(s_tank, 0, sizeof s_tank);
    memset(s_prop, 0, sizeof s_prop);
    memset(&s_roof, 0, sizeof s_roof);
    memset(s_barrel, 0, sizeof s_barrel);
    s_ready = false;
}

bool tq_art_ready(void)
{
    return s_ready;
}

size_t tq_art_bytes(void)
{
    return s_pak_size;
}

static const tq_spr_t *ok(const tq_spr_t *s)
{
    return s->w ? s : NULL;
}

const tq_spr_t *tq_art_tank(int set, int part, int yaw_index, int mip)
{
    if (mip >= TQ_MIPS) mip = TQ_MIPS - 1;
    if (set == TQ_SET_TOP) mip = 0;
    return ok(&s_tank[set][part][yaw_index & (TQ_YAWS - 1)][mip]);
}

const tq_spr_t *tq_art_prop(int set, int prop, int yaw_index, int mip)
{
    if (mip >= TQ_MIPS) mip = TQ_MIPS - 1;
    if (set == TQ_SET_TOP) mip = 0;
    return ok(&s_prop[set][prop][yaw_index & 7][mip]);
}

const tq_spr_t *tq_art_roof(void)
{
    return ok(&s_roof);
}

const tq_spr_t *tq_art_barrel(int pitch_index)
{
    if (pitch_index < 0) pitch_index = 0;
    if (pitch_index >= TQ_PITCHES) pitch_index = TQ_PITCHES - 1;
    return ok(&s_barrel[pitch_index]);
}

const tq_lut_t *tq_art_lut(int scheme)
{
    return s_lut ? &s_lut[scheme % TQ_SCHEMES] : NULL;
}
