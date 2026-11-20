/*
 * BASTION - a bot plays the whole campaign, to tune the numbers
 *
 *     tools/harness.sh [skill 0..1] [-v] [-e] [-D 0|1|2]
 *
 * -e also plays the mode without end in every world, -D sets the purse
 * (0 normal, 1 easy, 2 hard).
 *
 * It links the battle itself (main/bs_game.c, bs_world.c, bs_data.c) and
 * plays every sector of every world in order, as a player would: it puts
 * towers on the pads that see most of the road, upgrades them, uses the
 * powers, and between battles spends its cores in the laboratory. A
 * sector lost is tried again (with what the loss paid) up to six times.
 * 'skill' is how much of its credits it manages to spend well: 1 is a
 * player who wastes nothing, 0.6 a casual one.
 *
 * What it prints a sector: tries, stars, the fort's life at the end, the
 * towers standing, and the cores and rank the campaign has reached - the
 * curve to look at when a number in bs_data.c or bsworlds.py changes.
 */
#include "bs_game.h"
#include "bs_pak.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXW 16

typedef struct {
    int     cores, xp;
    uint8_t stars[MAXW][BS_SECTORS];
    uint8_t owned[TW_N], mastery[TW_N], tech[TECH_N], power[PW_N];
} save_t;

static float skill = 0.85f;
static int g_mode, g_diff;
static int verbose;
static uint32_t seed = 12345;

static float frand(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8) / 16777216.0f;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

/* how much of the road a tower on this pad would reach */
static float cover(const bs_world_t *w, int pad, float range)
{
    float n = 0;
    for (int p = 0; p < w->npath; p++) {
        const bs_path_t *pa = &w->path[p];
        for (int i = 0; i < pa->n; i++) {
            if (pa->y[i] > 38.0f) continue;
            float dx = pa->x[i] - w->pad_x[pad], dy = pa->y[i] - w->pad_y[pad];
            if (dx * dx + dy * dy < range * range) n += 1.0f;
        }
    }
    return n;
}

/* and how much of the fliers' ways */
static float cover_air(const bs_world_t *w, int pad, float range)
{
    float n = 0;
    for (int p = 0; p < w->npath; p++) {
        const bs_path_t *pa = &w->air[p];
        for (int i = 0; i < pa->n; i++) {
            if (pa->y[i] > 38.0f) continue;
            float dx = pa->x[i] - w->pad_x[pad], dy = pa->y[i] - w->pad_y[pad];
            if (dx * dx + dy * dy < range * range) n += 1.0f;
        }
    }
    return n;
}

static int count(const bs_game_t *g, int type)
{
    int n = 0;
    for (int i = 0; i < g->w->npad; i++) n += g->tw[i].on && g->tw[i].type == type;
    return n;
}

/* what the bot would like to have next: a mix that answers what comes */
static int want(const bs_game_t *g)
{
    int c[AL_N];
    bs_game_preview(g, c);
    int fliers = c[AL_WASP] + 4 * c[AL_CARRIER];
    int armor = c[AL_SHELL] + 3 * c[AL_BRUTE] + 10 * c[AL_TITAN] + 6 * c[AL_QUEEN];
    int crowd = c[AL_SWARM] + c[AL_GRUB] + c[AL_RUNNER] + c[AL_SPLIT] * 3;
    int towers = 0, aa = 0;
    for (int i = 0; i < g->w->npad; i++) {
        if (!g->tw[i].on) continue;
        towers++;
        aa += BS_TOWER[g->tw[i].type].air && g->tw[i].type != TW_CRYO && g->tw[i].type != TW_RADAR;
    }
    const uint8_t *o = g->kit.owned;
    if (fliers > 0 && aa * 3 < towers + 2) return o[TW_MISSILE] && count(g, TW_MISSILE) < 3 ? TW_MISSILE : TW_GATLING;
    if (towers >= 3 && !count(g, TW_CRYO)) return TW_CRYO;
    if (towers >= 5 && o[TW_RADAR] && !count(g, TW_RADAR)) return TW_RADAR;
    if (towers >= 4 && o[TW_BANK] && count(g, TW_BANK) < 2 && g->wave < g->nwaves / 2) return TW_BANK;
    if (armor > 2) {
        if (o[TW_RAIL] && count(g, TW_RAIL) < 2) return TW_RAIL;
        if (o[TW_LASER] && count(g, TW_LASER) < 3) return TW_LASER;
        if (count(g, TW_CANNON) < 3) return TW_CANNON;
    }
    if (crowd > 20) {
        if (o[TW_MORTAR] && count(g, TW_MORTAR) < 2) return TW_MORTAR;
        if (o[TW_TESLA] && count(g, TW_TESLA) < 3) return TW_TESLA;
        if (o[TW_FLAME] && count(g, TW_FLAME) < 2) return TW_FLAME;
        if (count(g, TW_CANNON) < 4) return TW_CANNON;
    }
    if (o[TW_EMP] && (c[AL_SHIELD] || c[AL_OVERMIND]) && !count(g, TW_EMP)) return TW_EMP;
    if (o[TW_GRAVITY] && towers >= 7 && !count(g, TW_GRAVITY)) return TW_GRAVITY;
    if (o[TW_DRONE] && towers >= 6 && count(g, TW_DRONE) < 2) return TW_DRONE;
    /* otherwise whatever it owns, in turn */
    static const int ORDER[] = { TW_GATLING, TW_CANNON, TW_LASER, TW_TESLA, TW_MISSILE, TW_GATLING, TW_FLAME, TW_CANNON };
    for (int k = 0; k < 8; k++) {
        int t = ORDER[(towers + k) % 8];
        if (o[t]) return t;
    }
    return TW_GATLING;
}

static void spend(bs_game_t *g, float *wallet)
{
    /* the unskilled leave part of every purse unspent */
    for (int guard = 0; guard < 20; guard++) {
        int budget = (int)((float)g->credits - *wallet);
        if (budget < 40) return;
        int type = want(g);
        int cost = bs_game_cost(g, type, 0);
        /* what it wants may be dear: something it can pay does more than
         * an empty pad */
        if (cost > budget) {
            int c[AL_N];
            bs_game_preview(g, c);
            type = c[AL_WASP] + c[AL_CARRIER] > 0 && (count(g, TW_GATLING) + count(g, TW_MISSILE)) * 2 <= count(g, TW_CANNON)
                       ? TW_GATLING : (count(g, TW_CANNON) <= count(g, TW_GATLING) ? TW_CANNON : TW_GATLING);
            cost = bs_game_cost(g, type, 0);
            if (cost > budget) {
                type = TW_GATLING;
                cost = bs_game_cost(g, type, 0);
            }
        }
        float range = bs_game_range(g, type, 0);
        if (range <= 0) range = 1.0f;
        int nx[AL_N];
        bs_game_preview(g, nx);
        bool fliers = nx[AL_WASP] + nx[AL_CARRIER] > 0;
        int best = -1;
        float bc = -1;
        for (int i = 0; i < g->w->npad; i++) {
            if (g->tw[i].on) continue;
            float c = type == TW_BANK ? 100.0f - cover(g->w, i, 4.0f) : cover(g->w, i, range);
            /* what is bought against fliers goes where they pass */
            if (fliers && BS_TOWER[type].air && type != TW_BANK) c = c * 0.4f + cover_air(g->w, i, range) * 2.0f;
            if (type == TW_RADAR) {
                c = 0;
                for (int k = 0; k < g->w->npad; k++) {
                    float dx = g->w->pad_x[k] - g->w->pad_x[i], dy = g->w->pad_y[k] - g->w->pad_y[i];
                    if (g->tw[k].on && dx * dx + dy * dy < range * range) c += 1;
                }
            }
            if (c > bc) {
                bc = c;
                best = i;
            }
        }
        /* an upgrade of the tower that sees most, when it is the better buy */
        int up = -1, upcost = 1 << 30;
        float ub = -1;
        for (int i = 0; i < g->w->npad; i++) {
            const bs_tower_t *t = &g->tw[i];
            if (!t->on || t->tier >= BS_TIERS - 1) continue;
            float c = cover(g->w, i, bs_game_range(g, t->type, t->tier) + 0.1f) + (float)t->kills * 0.2f;
            if (c > ub) {
                ub = c;
                up = i;
                upcost = bs_game_cost(g, t->type, t->tier + 1);
            }
        }
        int towers = 0;
        for (int i = 0; i < g->w->npad; i++) towers += g->tw[i].on;
        bool prefer_up = up >= 0 && (best < 0 || (towers >= 4 && (towers & 1)) || bc < ub * 0.45f);
        if (prefer_up && upcost <= budget) {
            bs_game_upgrade(g, up);
        } else if (best >= 0 && cost <= budget) {
            bs_game_build(g, best, type);
        } else if (up >= 0 && upcost <= budget) {
            bs_game_upgrade(g, up);
        } else {
            return;
        }
    }
}

static void powers(bs_game_t *g)
{
    if (g->kit.power[PW_REPAIR] && g->power_cd[PW_REPAIR] <= 0 && g->lives * 2 < g->max_lives) bs_game_power(g, PW_REPAIR, 0, 0);
    int near = 0, best = -1, most = 0;
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        const bs_alien_t *a = &g->al[i];
        if (!a->on) continue;
        if (a->left < 8.0f) near++;
        int n = 0;
        for (int k = 0; k < BS_MAX_ALIENS; k++) {
            const bs_alien_t *o = &g->al[k];
            if (!o->on) continue;
            float dx = o->x - a->x, dy = o->y - a->y;
            if (dx * dx + dy * dy < 6.0f) n += BS_ALIEN[o->type].boss ? 10 : 1;
        }
        if (n > most && a->y < 38.0f) {
            most = n;
            best = i;
        }
    }
    if (g->kit.power[PW_FREEZE] && g->power_cd[PW_FREEZE] <= 0 && near >= 4) bs_game_power(g, PW_FREEZE, 0, 0);
    if (g->kit.power[PW_STRIKE] && g->power_cd[PW_STRIKE] <= 0 && most >= 7 && best >= 0)
        bs_game_power(g, PW_STRIKE, g->al[best].x, g->al[best].y);
}

static void battle(bs_game_t *g, const bs_world_t *w, int sector, const save_t *sv)
{
    bs_kit_t kit;
    memset(&kit, 0, sizeof kit);
    memcpy(kit.owned, sv->owned, sizeof kit.owned);
    memcpy(kit.mastery, sv->mastery, sizeof kit.mastery);
    memcpy(kit.tech, sv->tech, sizeof kit.tech);
    memcpy(kit.power, sv->power, sizeof kit.power);
    kit.rank = bs_rank_of(sv->xp);
    kit.mode = (uint8_t)g_mode;
    kit.diff = (uint8_t)g_diff;
    bs_game_start(g, w, sector, &kit);
    float wallet = 0;
    int steps = 0;
    while (g->state != BS_WON && g->state != BS_LOST && steps < 60 * 60 * 40) {
        if (g->state == BS_BUILD) {
            wallet = (float)g->credits * (1.0f - skill) * frand();
            spend(g, &wallet);
            bs_game_call_wave(g);
        } else if (steps % 90 == 0) {
            spend(g, &wallet);
            powers(g);
        }
        bs_game_step(g);
        g->nsnd = 0;
        steps++;
    }
}

/* the laboratory, as a sensible player would shop: whatever gives most
 * for its cores, a new tower above all while there are few */
static void shop(save_t *sv, int world)
{
    for (int guard = 0; guard < 60; guard++) {
        int best = -1, kind = 0, cost = 1;
        float bv = 0;
        int owned = 0;
        for (int i = 0; i < TW_N; i++) owned += sv->owned[i];
        for (int i = 0; i < TW_N; i++) {
            if (sv->owned[i]) continue;
            float v = (owned < 5 + world ? 150.0f : 70.0f) / (float)BS_TOWER[i].unlock;
            if (v > bv) {
                bv = v;
                best = i;
                kind = 0;
                cost = BS_TOWER[i].unlock;
            }
        }
        for (int i = 0; i < TECH_N; i++) {
            if (sv->tech[i] >= BS_TECH_MAX) continue;
            static const float W[TECH_N] = { 30, 34, 30, 40, 26, 10, 22, 30 };
            float v = W[i] / (float)BS_TECH[i].cost[sv->tech[i]];
            if (v > bv) {
                bv = v;
                best = i;
                kind = 1;
                cost = BS_TECH[i].cost[sv->tech[i]];
            }
        }
        for (int i = 0; i < PW_N; i++) {
            if (sv->power[i] >= BS_POWER_MAX) continue;
            float v = (sv->power[i] ? 45.0f : 75.0f) / (float)BS_POWER[i].cost[sv->power[i]];
            if (v > bv) {
                bv = v;
                best = i;
                kind = 2;
                cost = BS_POWER[i].cost[sv->power[i]];
            }
        }
        for (int i = 0; i < TW_N; i++) {
            if (!sv->owned[i] || sv->mastery[i] >= BS_MASTERY_MAX) continue;
            float v = (i <= TW_TESLA ? 26.0f : 14.0f) / (float)BS_MASTERY_COST[sv->mastery[i]];
            if (v > bv) {
                bv = v;
                best = i;
                kind = 3;
                cost = BS_MASTERY_COST[sv->mastery[i]];
            }
        }
        /* it saves for the best thing rather than buy a lesser one */
        if (best < 0 || cost > sv->cores) return;
        sv->cores -= cost;
        if (kind == 0) sv->owned[best] = 1;
        else if (kind == 1) sv->tech[best]++;
        else if (kind == 2) sv->power[best]++;
        else sv->mastery[best]++;
    }
}

static uint16_t base_hp[MAXW][4096];

static void scale(bs_world_t *w, int wi, int sector, float f)
{
    for (int wv = 0; wv < w->nwaves[sector]; wv++) {
        const bs_wave_t *wa = &w->wave[sector][wv];
        for (int i = 0; i < wa->n; i++) {
            float v = (float)base_hp[wi][wa->first + i] * f;
            w->group[wa->first + i].hp_pct = (uint16_t)(v > 60000 ? 60000 : v < 5 ? 5 : v);
        }
    }
}

/* the share of the fort's life the bot keeps, over a few battles */
static float trial(bs_game_t *g, const bs_world_t *w, int sector, const save_t *sv, int n)
{
    float sum = 0;
    for (int k = 0; k < n; k++) {
        seed = 1000u + (uint32_t)k * 7919u;
        battle(g, w, sector, sv);
        if (g->state == BS_WON) sum += (float)g->lives / (float)g->max_lives;
    }
    return sum / (float)n;
}

int main(int argc, char **argv)
{
    const char *dir = "build/defs";
    int calibrate = 0, endless = 0;
    float target = 0.72f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-c")) calibrate = 1;
        else if (!strcmp(argv[i], "-e")) endless = 1;
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) g_diff = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) target = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
        else skill = strtof(argv[i], NULL);
    }
    static bs_world_t world[MAXW];
    int nw = 0;
    char path[300];
    for (int i = 1; i <= MAXW; i++) {
        snprintf(path, sizeof path, "%s/%02d.txt", dir, i);
        char *txt = slurp(path);
        if (!txt) break;
        if (!bs_world_parse(&world[nw], txt, false)) {
            printf("%s: not a world\n", path);
            return 1;
        }
        free(txt);
        for (int k = 0; k < world[nw].ngroup && k < 4096; k++) base_hp[nw][k] = world[nw].group[k].hp_pct;
        nw++;
    }
    if (!nw) {
        printf("no worlds in %s (tools/harness.sh writes them)\n", dir);
        return 1;
    }
    save_t sv;
    memset(&sv, 0, sizeof sv);
    for (int i = 0; i < TW_N; i++) sv.owned[i] = BS_TOWER[i].unlock == 0;
    static bs_game_t g;
    int table[MAXW * BS_SECTORS];
    printf("skill %.2f%s\n", (double)skill, calibrate ? ", calibrating" : "");
    printf("%-9s %-3s %5s %5s %6s %6s %7s %6s %5s %5s %6s %5s\n", "world", "sec", "tries", "stars", "lives", "towers",
           "credits", "cores", "rank", "owned", "kills", "hp%");
    int total_tries = 0, lost = 0;
    for (int w = 0; w < nw; w++) {
        for (int s = 0; s < BS_SECTORS; s++) {
            int tries = 0, level = w * 3 + s;
            int won = 0;
            float f = 1.0f;
            shop(&sv, w);
            if (calibrate) {
                /* the toughest the bot still holds with 'target' of the
                 * fort's life left */
                float lo = 0.15f, hi = 60.0f;
                for (int it = 0; it < 10; it++) {
                    f = sqrtf(lo * hi);
                    scale(&world[w], w, s, f);
                    if (trial(&g, &world[w], s, &sv, 3) >= target) lo = f;
                    else hi = f;
                }
                f = lo;
                scale(&world[w], w, s, f);
            }
            table[level] = (int)(100.0f * f + 0.5f);
            seed = 4242u + (uint32_t)level;
            while (tries < 6 && !won) {
                tries++;
                if (tries > 1) shop(&sv, w);
                battle(&g, &world[w], s, &sv);
                sv.xp += g.xp;
                if (g.state == BS_WON) {
                    int nu = g.stars > sv.stars[w][s] ? g.stars - sv.stars[w][s] : 0;
                    sv.stars[w][s] = (uint8_t)(g.stars > sv.stars[w][s] ? g.stars : sv.stars[w][s]);
                    sv.cores += 4 + level / 2 + nu * (6 + w);
                    won = 1;
                } else {
                    sv.cores += g.wave / 2;
                    if (verbose) printf("    lost at wave %d/%d with %d towers\n", g.wave + 1, g.nwaves, g.built);
                }
            }
            if (verbose) {
                printf("    lives after each wave:");
                for (int k = 0; k < g.wave && k < BS_WAVES; k++) printf(" %d", g.lives_at[k]);
                printf("\n    leaked:");
                for (int k = 0; k < AL_N; k++) {
                    if (g.leak_by[k]) printf(" %s x%d", BS_ALIEN[k].id, g.leak_by[k]);
                }
                printf("\n    towers:");
                for (int k = 0; k < TW_N; k++) {
                    int n = count(&g, k);
                    if (n) printf(" %s x%d", BS_TOWER[k].id, n);
                }
                printf("\n");
            }
            int towers = 0, owned = 0;
            for (int i = 0; i < world[w].npad; i++) towers += g.tw[i].on;
            for (int i = 0; i < TW_N; i++) owned += sv.owned[i];
            printf("%-9s %-3d %5d %5d %3d/%-3d %6d %7d %6d %5d %5d %6d %5d%s\n", world[w].id, s + 1, tries, won ? g.stars : 0,
                   g.lives, g.max_lives, towers, g.credits, sv.cores, bs_rank_of(sv.xp), owned, g.kills,
                   (int)world[w].group[world[w].wave[s][0].first].hp_pct, won ? "" : "   LOST");
            total_tries += tries;
            lost += !won;
        }
    }
    printf("%d battles for %d sectors, %d never won\n", total_tries, nw * BS_SECTORS, lost);
    if (endless) {
        /* the mode without end, with the laboratory the campaign left:
         * how many waves the bot holds in each world */
        g_mode = BS_MODE_ENDLESS;
        printf("without end:");
        for (int w = 0; w < nw; w++) {
            seed = 99u + (uint32_t)w;
            battle(&g, &world[w], 0, &sv);
            printf(" %s %d (%d kills, %.0f min)", world[w].id, g.wave, g.kills, (double)(g.time / 60.0f));
        }
        printf("\n");
        g_mode = BS_MODE_CAMPAIGN;
    }
    if (calibrate) {
        /* the bot wastes nothing from its first battle; a person learns
         * the game over the first worlds, so those are eased */
        printf("HP = [");
        for (int i = 0; i < nw * BS_SECTORS; i++) {
            float ease = i >= 12 ? 1.0f : 0.55f + 0.45f * (float)i / 12.0f;
            printf("%s%d", i ? ", " : "", (int)((float)table[i] * ease + 0.5f));
        }
        printf("]\n");
    }
    return 0;
}
