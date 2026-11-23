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

#define VERSION   3

typedef struct {
    char     name[24];
    uint32_t off, size;
} entry_t;

/* the tables of every pack open, each entry with its pack */
typedef struct {
    char     name[24];
    uint32_t off, size;
    uint8_t  pack;              /* TQ_DLC_* */
} ent_t;

/* A group of sprites loaded together: each sprite in a block of its own
 * (the board's PSRAM gets fragmented by other apps: a few MB in one piece is
 * not always there), and the smaller copies made of its low sprites */
typedef struct {
    uint8_t **blob;
    int      nblob;
    size_t   bytes;
    uint8_t *mips[160];
    int      nmips;
} group_t;

static ent_t *s_ent;
static int s_nent;
static bool s_open;

static group_t s_tank_g[TQ_TANK_TYPES], s_biome_g, s_show_g, s_marks_g;
static tq_spr_t s_mark[TQ_SETS][TQ_PROPS_ALL - TQ_PROPS][TQ_MIPS];
static tq_lut_t *s_marks_lut;
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
    for (int i = 0; i < g->nblob; i++) tq_free(g->blob[i]);
    tq_free(g->blob);
    memset(g, 0, sizeof *g);
}

/* A pack on the card, which may be in parts (tanques_p4.pak, .pak.1,
 * .pak.2...: packs over 8 MB travel so); a reader keeps one part of one
 * pack open */
typedef struct {
    FILE    *f;
    int      pack;
    int      part;
    uint32_t base, end;
} reader_t;

static void rd_close(reader_t *r)
{
    if (r->f) fclose(r->f);
    r->f = NULL;
}

static bool rd_part(reader_t *r, int part, uint32_t base)
{
    rd_close(r);
    char path[176];
    const char *name = TQ_DLC_PACK[r->pack];
    if (part) snprintf(path, sizeof path, "%s/%s.%d", aos_hal_path_apps(), name, part);
    else snprintf(path, sizeof path, "%s/%s", aos_hal_path_apps(), name);
    r->f = fopen(path, "rb");
    if (!r->f) return false;
    fseek(r->f, 0, SEEK_END);
    long size = ftell(r->f);
    if (size <= 0) {
        rd_close(r);
        return false;
    }
    r->part = part;
    r->base = base;
    r->end = base + (uint32_t)size;
    return true;
}

/* [off, off + n) of a pack */
static bool rd_read(reader_t *r, int pack, uint32_t off, uint32_t n, uint8_t *dst)
{
    if (r->pack != pack) {
        rd_close(r);
        r->pack = pack;
    }
    while (n) {
        if (!r->f || off < r->base) {
            if (!rd_part(r, 0, 0)) return false;
        }
        while (off >= r->end) {
            if (r->part >= 15 || !rd_part(r, r->part + 1, r->end)) return false;
        }
        uint32_t take = r->end - off < n ? r->end - off : n;
        if (fseek(r->f, (long)(off - r->base), SEEK_SET) != 0 || fread(dst, 1, take, r->f) != take) return false;
        dst += take;
        off += take;
        n -= take;
    }
    return true;
}

typedef bool (*match_fn)(const char *name, const void *arg);

/* Loads every entry the test takes, each in its own block, and parses each
 * with cb. On a failure the group is left empty. */
static bool group_load(group_t *g, match_fn match, const void *arg,
                       void (*cb)(group_t *g, const char *name, const tq_spr_t *s, const void *arg))
{
    int n = 0;
    for (int i = 0; i < s_nent; i++)
        if (match(s_ent[i].name, arg)) n++;
    if (!n) return false;
    g->blob = (uint8_t **)tq_calloc(sizeof(uint8_t *) * (size_t)n);
    if (!g->blob) return false;
    reader_t r = { 0 };
    bool ok = true;
    for (int i = 0; i < s_nent && ok; i++) {
        if (!match(s_ent[i].name, arg)) continue;
        uint8_t *b = (uint8_t *)tq_malloc(s_ent[i].size);
        if (!b || !rd_read(&r, s_ent[i].pack, s_ent[i].off, s_ent[i].size, b)) {
            tq_free(b);
            ok = false;
            break;
        }
        g->blob[g->nblob++] = b;
        g->bytes += s_ent[i].size;
        tq_spr_t sp;
        if (parse(b, s_ent[i].size, &sp)) cb(g, s_ent[i].name, &sp, arg);
    }
    rd_close(&r);
    if (!ok) {
        aos_hal_log("tanques", "art: out of memory after %u KB", (unsigned)(g->bytes / 1024));
        group_free(g);
    }
    return ok;
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
            /* what was taken before the failure points at freed blocks */
            memset(s_tank[0][t], 0, sizeof s_tank[0][t]);
            memset(s_tank[1][t], 0, sizeof s_tank[1][t]);
            memset(&s_roof[t], 0, sizeof s_roof[t]);
            memset(s_barrel[t], 0, sizeof s_barrel[t]);
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
        { TQ_B_LUNA, 8, 0x84E0FF }, { TQ_B_LUNA, 9, 0x5C5E6B }, { TQ_B_LUNA, 10, 0xB3B3BA }, { TQ_B_LUNA, 11, 0xEDEFF5 },
        { TQ_B_PLAYA, 8, 0x5CB34D }, { TQ_B_PLAYA, 9, 0xA8784A }, { TQ_B_PLAYA, 10, 0xADA394 },
        { TQ_B_PLAYA, 11, 0xEBD19E }, { TQ_B_PLAYA, 2, 0xF2524D },
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
    if (!group_load(&s_biome_g, match_biome, &biome, take_prop)) {
        memset(s_prop, 0, sizeof s_prop);
        return false;
    }
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
    if (!group_load(&s_show_g, match_show, &type, take_show)) {
        memset(s_showspr, 0, sizeof s_showspr);
        return false;
    }
    s_show = type;
    return true;
}

/* --------------------------------------------------------------------------
 * The pack
 * -------------------------------------------------------------------------- */

/* "<set>/marks_<slot>": the modes' marks, whichever DLC brought them */
static bool match_marks(const char *name, const void *arg)
{
    (void)arg;
    const char *rest;
    return set_of(name, &rest) >= 0 && !strncmp(rest, "marks_", 6);
}

static void take_mark(group_t *g, const char *name, const tq_spr_t *s, const void *arg)
{
    (void)g;
    (void)arg;
    static const char *const SLOT[TQ_PROPS_ALL - TQ_PROPS] = { "fl", "ch", "bc" };
    const char *rest;
    int set = set_of(name, &rest);
    for (int m = 0; m < TQ_PROPS_ALL - TQ_PROPS; m++)
        if (!strcmp(rest + 6, SLOT[m])) s_mark[set][m][0] = *s;
}

/* One pack's table into s_ent; false: not there or not a pack */
static bool open_pack(int pack)
{
    reader_t r = { 0 };
    uint8_t hdr[12];
    uint32_t ver = 0, count = 0;
    if (!rd_read(&r, pack, 0, 12, hdr)) {
        rd_close(&r);
        return false;
    }
    memcpy(&ver, hdr + 4, 4);
    memcpy(&count, hdr + 8, 4);
    if (memcmp(hdr, "TQPK", 4) || ver != VERSION || count == 0 || count > 8192) {
        aos_hal_log("tanques", "%s: not a version %d pack", TQ_DLC_PACK[pack], VERSION);
        rd_close(&r);
        return false;
    }
    entry_t *tab = (entry_t *)tq_malloc(sizeof(entry_t) * count);
    ent_t *all = (ent_t *)tq_malloc(sizeof(ent_t) * (s_nent + count));
    bool ok = tab && all && rd_read(&r, pack, 12, (uint32_t)(sizeof(entry_t) * count), (uint8_t *)tab);
    rd_close(&r);
    if (ok) {
        if (s_nent) memcpy(all, s_ent, sizeof(ent_t) * s_nent);
        for (uint32_t i = 0; i < count; i++) {
            ent_t *e = &all[s_nent + i];
            memcpy(e->name, tab[i].name, 24);
            e->name[23] = 0;
            e->off = tab[i].off;
            e->size = tab[i].size;
            e->pack = (uint8_t)pack;
        }
        tq_free(s_ent);
        s_ent = all;
        s_nent += (int)count;
        aos_hal_log("tanques", "%s: %u sprites", TQ_DLC_PACK[pack], (unsigned)count);
    } else {
        tq_free(all);
    }
    tq_free(tab);
    return ok;
}

bool tq_art_open(void)
{
    if (s_open) return true;
    s_scen = (tq_lut_t *)tq_malloc(sizeof(tq_lut_t));
    s_marks_lut = (tq_lut_t *)tq_malloc(sizeof(tq_lut_t));
    if (!s_scen || !s_marks_lut || !open_pack(TQ_DLC_BASE)) {
        aos_hal_log("tanques", "no pack: %s in %s", TQ_DLC_PACK[TQ_DLC_BASE], aos_hal_path_apps());
        tq_art_close();
        return false;
    }
    uint32_t have = 1;
    for (int d = 1; d < TQ_DLCS; d++)
        if (open_pack(d)) have |= 1u << d;
    tq_dlc_set(have);
    s_open = true;
    /* the marks are small: they stay */
    if (group_load(&s_marks_g, match_marks, NULL, take_mark))
        for (int m = 0; m < TQ_PROPS_ALL - TQ_PROPS; m++) mips(&s_marks_g, s_mark[TQ_SET_LOW][m]);
    static const uint32_t MC[16] = {
        [1] = 0xF58C29, [2] = 0xFAE09E, [3] = 0x403D45, [4] = 0x9EA3AD, [5] = 0xFFEB99, [6] = 0xF7F7F2,
        [7] = 0xF5F7FC, [9] = 0x9A6A3E, [10] = 0xB3AB9E, [11] = 0xEBC485,
    };
    tq_lut_build(s_marks_lut, MC);
    s_marks_lut->emblem_on = false;
    aos_hal_log("tanques", "pack: %d sprites, DLCs %s%s", s_nent, (have & 2) ? "luna " : "", (have & 4) ? "playa" : "");
    return true;
}

void tq_art_close(void)
{
    for (int t = 0; t < TQ_TANK_TYPES; t++) group_free(&s_tank_g[t]);
    group_free(&s_biome_g);
    group_free(&s_show_g);
    group_free(&s_marks_g);
    tq_free(s_ent);
    tq_free(s_scen);
    tq_free(s_marks_lut);
    s_ent = NULL;
    s_scen = NULL;
    s_marks_lut = NULL;
    memset(s_mark, 0, sizeof s_mark);
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
    size_t n = s_biome_g.bytes + s_show_g.bytes + s_marks_g.bytes;
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
    if (prop >= TQ_PROPS) return prop < TQ_PROPS_ALL ? ok(&s_mark[set][prop - TQ_PROPS][mip]) : NULL;
    return ok(&s_prop[set][prop][yaw_index & 7][mip]);
}

const tq_lut_t *tq_art_marks_lut(void)
{
    return s_marks_lut;
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
