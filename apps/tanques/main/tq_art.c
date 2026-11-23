/*
 * TANQUES - the art pack (see tq_art.h)
 */
#include "tq_art.h"
#include "tq_data.h"
#include "tq_mem.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_NAME "tanques_p4.pak"
#define VERSION   3

typedef struct {
    char     name[24];
    uint32_t off, size;
} entry_t;

/* A group of sprites read in one go: its bytes, and the smaller copies
 * made of its low sprites */
typedef struct {
    uint8_t *blob;
    size_t   bytes;
    uint8_t *mips[160];
    int      nmips;
} group_t;

static entry_t *s_ent;
static int s_nent;
static bool s_open;

static group_t s_tank_g[TQ_TANK_TYPES], s_biome_g, s_show_g;
static uint32_t s_tanks, s_fps;
static int s_biome = -1, s_show = -1;

static tq_spr_t s_tank[TQ_SETS][TQ_TANK_TYPES][TQ_PARTS][TQ_YAWS][TQ_MIPS];
static tq_spr_t s_prop[TQ_SETS][TQ_PROPS][8][TQ_MIPS];
static tq_spr_t s_roof[TQ_TANK_TYPES], s_barrel[TQ_TANK_TYPES][TQ_PITCHES];
static tq_spr_t s_showspr[TQ_SHOWS];
static tq_lut_t *s_scen;

static inline int mini_(int a, int b) { return a < b ? a : b; }
static inline int maxi_(int a, int b) { return a > b ? a : b; }

/* --------------------------------------------------------------------------
 * Sprites
 * -------------------------------------------------------------------------- */

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
    bool noz = n & 0x80000000u, uv = n & 0x40000000u;
    n &= 0x3FFFFFFFu;
    size_t need = 16 + (size_t)h * sizeof(tq_row_t) + (size_t)n * (3 + !noz + uv);
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
    s->U = uv ? (noz ? s->I + n : s->Z + n) : NULL;
    return true;
}

/* A half-size copy, packed like the pack's: coverage and shade averaged,
 * the region, depth and badge cell of the most covered source pixel */
static bool half(group_t *g, const tq_spr_t *src, tq_spr_t *dst)
{
    int w = (src->w + 1) / 2, h = (src->h + 1) / 2;
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *m = NULL;
        tq_row_t *rows = NULL;
        uint8_t *L = NULL, *A = NULL, *I = NULL, *Z = NULL, *U = NULL;
        if (pass) {
            if (g->nmips >= (int)(sizeof g->mips / sizeof g->mips[0])) return false;
            m = (uint8_t *)tq_malloc((size_t)h * sizeof(tq_row_t) + (src->U ? 5 : 4) * (n ? n : 1));
            if (!m) return false;
            g->mips[g->nmips++] = m;
            rows = (tq_row_t *)m;
            L = m + (size_t)h * sizeof(tq_row_t);
            A = L + n;
            I = A + n;
            Z = I + n;
            U = src->U ? Z + n : NULL;
        }
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
                if (pass) {
                    rows[y].x0 = 0;
                    rows[y].len = 0;
                    rows[y].off = off;
                }
                continue;
            }
            if (!pass) {
                n += (size_t)(hi - lo);
                continue;
            }
            rows[y].x0 = (uint16_t)lo;
            rows[y].len = (uint16_t)(hi - lo);
            rows[y].off = off;
            for (int x = lo; x < hi; x++) {
                int asum = 0, lsum = 0, best = -1, bk = -1;
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
                        bk = i;
                    }
                }
                A[off] = (uint8_t)(asum / 4);
                L[off] = (uint8_t)(asum ? lsum / asum : 0);
                I[off] = bk >= 0 ? src->I[bk] : 0;
                Z[off] = bk >= 0 && src->Z ? src->Z[bk] : 128;
                if (U) U[off] = bk >= 0 ? src->U[bk] : 0;
                off++;
            }
        }
    }
    *dst = (tq_spr_t){ (int16_t)w, (int16_t)h, (int16_t)(src->ax / 2), (int16_t)(src->ay / 2), NULL, NULL, NULL, NULL, NULL, NULL };
    uint8_t *m = g->mips[g->nmips - 1];
    dst->rows = (const tq_row_t *)m;
    dst->L = m + (size_t)h * sizeof(tq_row_t);
    dst->A = dst->L + n;
    dst->I = dst->A + n;
    dst->Z = dst->I + n;
    dst->U = src->U ? dst->Z + n : NULL;
    return true;
}

static void mips(group_t *g, tq_spr_t *m)
{
    for (int k = 1; k < TQ_MIPS; k++) {
        if (!m[k - 1].w || !half(g, &m[k - 1], &m[k])) m[k] = m[k - 1];
    }
}

/* --------------------------------------------------------------------------
 * Groups
 * -------------------------------------------------------------------------- */

static void group_free(group_t *g)
{
    for (int i = 0; i < g->nmips; i++) tq_free(g->mips[i]);
    tq_free(g->blob);
    memset(g, 0, sizeof *g);
}

/* Reads [off, off + n) of the pack, which may be in parts on the card
 * (tanques_p4.pak, .pak.1, .pak.2...: packs over 8 MB travel so) */
static bool pak_read(uint32_t off, uint32_t n, uint8_t *dst)
{
    uint32_t base = 0;
    for (int part = 0; part < 16 && n; part++) {
        char path[176];
        if (part) snprintf(path, sizeof path, "%s/%s.%d", aos_hal_path_apps(), PACK_NAME, part);
        else snprintf(path, sizeof path, "%s/%s", aos_hal_path_apps(), PACK_NAME);
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if (size <= 0) {
            fclose(f);
            return false;
        }
        uint32_t end = base + (uint32_t)size;
        if (off < end) {
            uint32_t take = end - off < n ? end - off : n;
            bool ok = fseek(f, (long)(off - base), SEEK_SET) == 0 && fread(dst, 1, take, f) == take;
            if (!ok) {
                fclose(f);
                return false;
            }
            dst += take;
            off += take;
            n -= take;
        }
        fclose(f);
        base = end;
    }
    return n == 0;
}

typedef bool (*match_fn)(const char *name, const void *arg);

/* Reads every entry the test takes, in one read of the span they cover
 * (pack.py keeps a group's entries together), and parses each with cb */
static bool group_load(group_t *g, match_fn match, const void *arg,
                       void (*cb)(group_t *g, const char *name, const tq_spr_t *s, const void *arg))
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < s_nent; i++) {
        if (!match(s_ent[i].name, arg)) continue;
        if (s_ent[i].off < lo) lo = s_ent[i].off;
        if (s_ent[i].off + s_ent[i].size > hi) hi = s_ent[i].off + s_ent[i].size;
    }
    if (hi <= lo) return false;
    g->blob = (uint8_t *)tq_malloc(hi - lo);
    g->bytes = hi - lo;
    if (!g->blob || !pak_read(lo, hi - lo, g->blob)) {
        group_free(g);
        return false;
    }
    for (int i = 0; i < s_nent; i++) {
        if (!match(s_ent[i].name, arg)) continue;
        tq_spr_t s;
        if (parse(g->blob + (s_ent[i].off - lo), s_ent[i].size, &s)) cb(g, s_ent[i].name, &s, arg);
    }
    return true;
}

static int set_of(const char *name, const char **rest)
{
    if (!strncmp(name, "top/", 4)) {
        *rest = name + 4;
        return TQ_SET_TOP;
    }
    if (!strncmp(name, "low/", 4)) {
        *rest = name + 4;
        return TQ_SET_LOW;
    }
    *rest = name;
    return -1;
}

/* "<set>/<key>_..." or "fp/<key>_..." for one tank type */
static bool match_tank(const char *name, const void *arg)
{
    int t = *(const int *)arg;
    const char *key = TQ_TANK[t].key;
    const char *slash = strchr(name, '/');
    if (!slash) return false;
    if (strncmp(name, "top/", 4) && strncmp(name, "low/", 4) && strncmp(name, "fp/", 3)) return false;
    size_t l = strlen(key);
    return !strncmp(slash + 1, key, l) && slash[1 + l] == '_';
}

static void take_tank(group_t *g, const char *name, const tq_spr_t *s, const void *arg)
{
    (void)g;
    int t = *(const int *)arg;
    const char *rest;
    int set = set_of(name, &rest);
    rest += strlen(TQ_TANK[t].key) + 1;
    int idx;
    if (set >= 0) {
        int part = rest[0] == 'h' ? TQ_PART_HULL : TQ_PART_TURRET;
        if (sscanf(rest + 2, "%d", &idx) == 1 && idx >= 0 && idx < TQ_YAWS) s_tank[set][t][part][idx][0] = *s;
    } else if (!strncmp(name, "fp/", 3)) {
        rest = name + 3 + strlen(TQ_TANK[t].key) + 1;
        if (!strcmp(rest, "roof")) s_roof[t] = *s;
        else if (sscanf(rest, "b_%d", &idx) == 1 && idx >= 0 && idx < TQ_PITCHES) s_barrel[t][idx] = *s;
    }
}

static bool match_tank_nofp(const char *name, const void *arg)
{
    return strncmp(name, "fp/", 3) && match_tank(name, arg);
}

bool tq_art_use_tanks(uint32_t types, uint32_t with_fp)
{
    if (!s_open) return false;
    bool ok = true;
    for (int t = 0; t < TQ_TANK_TYPES; t++) {
        bool want = types & (1u << t), fp = with_fp & (1u << t);
        bool have = s_tanks & (1u << t), have_fp = s_fps & (1u << t);
        if (have && (!want || fp != have_fp)) {
            group_free(&s_tank_g[t]);
            memset(s_tank[0][t], 0, sizeof s_tank[0][t]);
            memset(s_tank[1][t], 0, sizeof s_tank[1][t]);
            memset(&s_roof[t], 0, sizeof s_roof[t]);
            memset(s_barrel[t], 0, sizeof s_barrel[t]);
            s_tanks &= ~(1u << t);
            s_fps &= ~(1u << t);
            have = false;
        }
        if (!want || have) continue;
        if (!group_load(&s_tank_g[t], fp ? match_tank : match_tank_nofp, &t, take_tank)) {
            ok = false;
            continue;
        }
        for (int p = 0; p < TQ_PARTS; p++)
            for (int y = 0; y < TQ_YAWS; y++) mips(&s_tank_g[t], s_tank[TQ_SET_LOW][t][p][y]);
        s_tanks |= 1u << t;
        if (fp) s_fps |= 1u << t;
    }
    return ok;
}

static const char *const PROP_SLOT[TQ_PROPS] = { "ta", "tb", "st", "b1", "b2" };

static bool match_biome(const char *name, const void *arg)
{
    const char *key = TQ_BIOME_KEY[*(const int *)arg], *rest;
    if (set_of(name, &rest) < 0) return false;
    size_t l = strlen(key);
    return !strncmp(rest, key, l) && rest[l] == '_';
}

static void take_prop(group_t *g, const char *name, const tq_spr_t *s, const void *arg)
{
    (void)g;
    const char *rest;
    int set = set_of(name, &rest);
    rest += strlen(TQ_BIOME_KEY[*(const int *)arg]) + 1;
    for (int p = 0; p < TQ_PROPS; p++) {
        if (strncmp(rest, PROP_SLOT[p], 2)) continue;
        int idx;
        if (rest[2] == 0) {
            for (int y = 0; y < 8; y++) s_prop[set][p][y][0] = *s;
        } else if (rest[2] == '_' && sscanf(rest + 3, "%d", &idx) == 1 && idx >= 0 && idx < 8) {
            s_prop[set][p][idx][0] = *s;
        }
    }
}

/* The scenery's paints per biome (tools/pack.py, BIOMES over FIXED) */
static void scenery_lut(int biome)
{
    static const uint32_t BASE[16] = {
        [3] = 0x403D45, [4] = 0x9EA3AD, [5] = 0xFFEB99, [6] = 0xF7F7F2, [7] = 0xF5F7FC,
        [8] = 0x66B847, [9] = 0x855733, [10] = 0xB3AB9E, [11] = 0xEBC485, [12] = 0xFF8A29,
    };
    static const struct { int biome, id; uint32_t rgb; } OVER[] = {
        { TQ_B_DESIERTO, 8, 0x5EA159 }, { TQ_B_DESIERTO, 9, 0x996B42 }, { TQ_B_DESIERTO, 10, 0xE0B27A },
        { TQ_B_DESIERTO, 11, 0xF2D49B }, { TQ_B_DESIERTO, 5, 0xFF7FA8 },
        { TQ_B_NIEVE, 8, 0x408A59 }, { TQ_B_NIEVE, 9, 0x6E4A33 }, { TQ_B_NIEVE, 10, 0xA8DDF0 },
        { TQ_B_NIEVE, 11, 0xD9443B }, { TQ_B_NIEVE, 12, 0xFF8A29 }, { TQ_B_NIEVE, 3, 0x2E2B33 },
        { TQ_B_RUINAS, 10, 0xB86B4F }, { TQ_B_RUINAS, 11, 0xD9D1C4 }, { TQ_B_RUINAS, 4, 0x6B7380 },
        { TQ_B_RUINAS, 5, 0xFFE8A1 },
        { TQ_B_VOLCAN, 9, 0x3D3330 }, { TQ_B_VOLCAN, 10, 0x4F4A52 }, { TQ_B_VOLCAN, 11, 0x8A8078 },
        { TQ_B_VOLCAN, 12, 0xFF6B1F },
    };
    uint32_t c[16];
    memcpy(c, BASE, sizeof c);
    for (size_t i = 0; i < sizeof OVER / sizeof OVER[0]; i++)
        if (OVER[i].biome == biome) c[OVER[i].id] = OVER[i].rgb;
    tq_lut_build(s_scen, c);
    s_scen->emblem_on = false;
}

bool tq_art_use_biome(int biome)
{
    if (!s_open) return false;
    if (biome == s_biome) return true;
    group_free(&s_biome_g);
    memset(s_prop, 0, sizeof s_prop);
    s_biome = -1;
    if (!group_load(&s_biome_g, match_biome, &biome, take_prop)) return false;
    for (int p = 0; p < TQ_PROPS; p++) {
        for (int y = 0; y < 8; y++) {
            if (y > 0 && s_prop[TQ_SET_LOW][p][y][0].L == s_prop[TQ_SET_LOW][p][0][0].L) {
                memcpy(s_prop[TQ_SET_LOW][p][y], s_prop[TQ_SET_LOW][p][0], sizeof s_prop[0][0][0]);
                continue;
            }
            mips(&s_biome_g, s_prop[TQ_SET_LOW][p][y]);
        }
    }
    s_biome = biome;
    scenery_lut(biome);
    return true;
}

static bool match_show(const char *name, const void *arg)
{
    const char *key = TQ_TANK[*(const int *)arg].key;
    size_t l = strlen(key);
    return !strncmp(name, "show/", 5) && !strncmp(name + 5, key, l) && name[5 + l] == '_';
}

static void take_show(group_t *g, const char *name, const tq_spr_t *s, const void *arg)
{
    (void)g;
    int idx;
    if (sscanf(name + 5 + strlen(TQ_TANK[*(const int *)arg].key) + 1, "%d", &idx) == 1 && idx >= 0 && idx < TQ_SHOWS)
        s_showspr[idx] = *s;
}

bool tq_art_use_show(int type)
{
    if (!s_open) return false;
    if (type == s_show) return true;
    group_free(&s_show_g);
    memset(s_showspr, 0, sizeof s_showspr);
    s_show = -1;
    if (type < 0) return true;
    if (!group_load(&s_show_g, match_show, &type, take_show)) return false;
    s_show = type;
    return true;
}

/* --------------------------------------------------------------------------
 * The pack
 * -------------------------------------------------------------------------- */

bool tq_art_open(void)
{
    if (s_open) return true;
    char path[160];
    snprintf(path, sizeof path, "%s/%s", aos_hal_path_apps(), PACK_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) {
        aos_hal_log("tanques", "no pack: %s", path);
        return false;
    }
    fclose(f);
    uint8_t hdr[12];
    uint32_t ver = 0, count = 0;
    if (pak_read(0, 12, hdr)) {
        memcpy(&ver, hdr + 4, 4);
        memcpy(&count, hdr + 8, 4);
    }
    if (memcmp(hdr, "TQPK", 4) || ver != VERSION || count == 0 || count > 8192) {
        aos_hal_log("tanques", "pack: not a version %d pack", VERSION);
        return false;
    }
    s_ent = (entry_t *)tq_malloc(sizeof(entry_t) * count);
    s_scen = (tq_lut_t *)tq_malloc(sizeof(tq_lut_t));
    if (!s_ent || !s_scen || !pak_read(12, (uint32_t)(sizeof(entry_t) * count), (uint8_t *)s_ent)) {
        tq_art_close();
        return false;
    }
    for (uint32_t i = 0; i < count; i++) s_ent[i].name[23] = 0;
    s_nent = (int)count;
    s_open = true;
    aos_hal_log("tanques", "pack: %d sprites", s_nent);
    return true;
}

void tq_art_close(void)
{
    for (int t = 0; t < TQ_TANK_TYPES; t++) group_free(&s_tank_g[t]);
    group_free(&s_biome_g);
    group_free(&s_show_g);
    tq_free(s_ent);
    tq_free(s_scen);
    s_ent = NULL;
    s_scen = NULL;
    s_nent = 0;
    s_tanks = s_fps = 0;
    s_biome = s_show = -1;
    memset(s_tank, 0, sizeof s_tank);
    memset(s_prop, 0, sizeof s_prop);
    memset(s_roof, 0, sizeof s_roof);
    memset(s_barrel, 0, sizeof s_barrel);
    memset(s_showspr, 0, sizeof s_showspr);
    s_open = false;
}

bool tq_art_ready(void)
{
    return s_open;
}

size_t tq_art_bytes(void)
{
    size_t n = s_biome_g.bytes + s_show_g.bytes;
    for (int t = 0; t < TQ_TANK_TYPES; t++) n += s_tank_g[t].bytes;
    return n;
}

static const tq_spr_t *ok(const tq_spr_t *s)
{
    return s->w ? s : NULL;
}

const tq_spr_t *tq_art_tank(int set, int type, int part, int yaw_index, int mip)
{
    if (mip >= TQ_MIPS) mip = TQ_MIPS - 1;
    if (set == TQ_SET_TOP) mip = 0;
    return ok(&s_tank[set][type % TQ_TANK_TYPES][part][yaw_index & (TQ_YAWS - 1)][mip]);
}

const tq_spr_t *tq_art_prop(int set, int prop, int yaw_index, int mip)
{
    if (mip >= TQ_MIPS) mip = TQ_MIPS - 1;
    if (set == TQ_SET_TOP) mip = 0;
    return ok(&s_prop[set][prop][yaw_index & 7][mip]);
}

const tq_spr_t *tq_art_roof(int type)
{
    return ok(&s_roof[type % TQ_TANK_TYPES]);
}

const tq_spr_t *tq_art_barrel(int type, int pitch_index)
{
    if (pitch_index < 0) pitch_index = 0;
    if (pitch_index >= TQ_PITCHES) pitch_index = TQ_PITCHES - 1;
    return ok(&s_barrel[type % TQ_TANK_TYPES][pitch_index]);
}

const tq_spr_t *tq_art_show(int type, int index)
{
    if (type != s_show) return NULL;
    return ok(&s_showspr[index & (TQ_SHOWS - 1)]);
}

void tq_art_tank_lut(tq_lut_t *lut, uint32_t paint1, uint32_t paint2, int emblem, uint32_t emblem_paint)
{
    uint32_t c[16] = {
        [1] = paint1, [2] = paint2, [3] = 0x403D45, [4] = 0x9EA3AD, [5] = 0xFFEB99, [6] = 0xF7F7F2,
        [7] = emblem_paint,
    };
    tq_lut_build(lut, c);
    lut->emblem_on = emblem >= 0;
    if (emblem >= 0) tq_emblem(emblem, lut->emblem);
}

const tq_lut_t *tq_art_scenery_lut(void)
{
    return s_scen;
}
