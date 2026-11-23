/*
 * DERIVA - the game played with no screen, to measure how hard it is.
 *
 *   harness -m campaign|arcade  [-d 0|1|2] [-n runs] [-g] [-l] [-v] [-s seed]
 *
 * -g: the good bot (the title screen's demo) instead of the person;
 * -l: the field lying down (880 x 720) instead of upright (720 x 980).
 *
 * The campaign is played as a person would: a sector lost is tried again
 * from its start with fresh lives (the crystals stay), and the workshop
 * buys the cheapest upgrade it can between sectors, over and over. It
 * prints per sector how many tries it took, the lives lost, the time and
 * the crystals. The arcade prints the wave reached and the score.
 */
#include "ast_bot.h"
#include "ast_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_verbose;
static int g_cause[6];

static void shop(ag_game_t *g)
{
    static const int ORDER[] = { UP_GUN, UP_ENGINE, UP_ARMOR, UP_JUMP, UP_MAGNET };
    for (;;) {
        int best = -1, cost = 1 << 30;
        for (int k = 0; k < 5; k++) {
            int u = ORDER[k];
            int c = ag_upg_cost(u, g->upg[u]);
            if (c && c <= g->crystals && c < cost) {
                cost = c;
                best = u;
            }
        }
        if (best < 0) break;
        g->crystals -= cost;
        g->upg[best]++;
    }
}

typedef struct {
    double tries, deaths, time, crystals, cleared;
    int cause[6];
    int n;
} sec_stat_t;

int main(int argc, char **argv)
{
    int arcade = 0, diff = AG_NORMAL, runs = 20, skill = BOT_PERSON, land = 0;
    uint32_t seed = 1;
    int first = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) arcade = !strcmp(argv[++i], "arcade");
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) diff = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) runs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) first = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g")) skill = BOT_GOOD;
        else if (!strcmp(argv[i], "-l")) land = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
    }
    float W = land ? 880.0f : 720.0f, H = land ? 720.0f : 980.0f;
    static ag_game_t game;
    ag_game_t *g = &game;
    ast_bot_t bot;
    if (!arcade) {
        sec_stat_t st[AG_SECTORS];
        memset(st, 0, sizeof st);
        int finished = 0;
        double total_time = 0;
        for (int r = 0; r < runs; r++) {
            ag_new(g, AG_CAMPAIGN, 1, diff, W, H, seed * 7919u + (uint32_t)r * 104729u + 1);
            ast_bot_init(&bot, skill, seed + (uint32_t)r * 31u);
            int lives0 = g->ship[0].lives;
            int sector = first;
            /* started further on: what the sectors before would have bought */
            g->crystals = 70 * (first - 1);
            shop(g);
            bool gave_up = false;
            while (sector <= AG_SECTORS && !gave_up) {
                int tries = 0, deaths0 = g->deaths;
                int c0[6];
                memcpy(c0, g->cause, sizeof c0);
                float t0 = g->t;
                int cr0 = g->crystals;
                int score0 = g->score;
                for (;;) {
                    tries++;
                    ag_sector_start(g, sector);
                    while (g->state != AG_ST_DONE && g->state != AG_ST_WON && g->state != AG_ST_OVER) {
                        ag_hand_t h;
                        ast_bot_hand(&bot, g, 0, &h);
                        ag_step(g, &h, NULL);
                        g->nev = 0;
                        g->nsnd = 0;
                        g->shake = 0;
                        if (g->since_kill > 240.0f) break;
                    }
                    if (g->state == AG_ST_DONE || g->state == AG_ST_WON) break;
                    if (g->since_kill > 240.0f) printf("  run %d sector %d: stalled (wave %d)\n", r, sector, g->wave);
                    if (g_verbose && g->boss.on)
                        printf("  lost to the boss: hp %.0f/%.0f, parts %.0f %.0f %.0f %.0f %.0f %.0f\n", (double)g->boss.hp,
                               (double)g->boss.hp_max, (double)g->boss.part_hp[0], (double)g->boss.part_hp[1],
                               (double)g->boss.part_hp[2], (double)g->boss.part_hp[3], (double)g->boss.part_hp[4],
                               (double)g->boss.part_hp[5]);
                    if (g_verbose && g->boss.on && g->boss.kind == BOSS_SERPIENTE) {
                        int n = 0;
                        for (int k = 0; k < AG_SEGS; k++) n += g->boss.seg_hp[k] > 0;
                        printf("  serpent: %d rings left\n", n);
                    }
                    if (tries >= 8) {
                        gave_up = true;
                        break;
                    }
                    /* tried again: fresh lives, the score of the sector's start */
                    g->state = AG_ST_INTRO;
                    g->score = score0;
                    for (int s = 0; s < g->players; s++) {
                        g->ship[s].in_play = true;
                        g->ship[s].lives = lives0;
                    }
                }
                sec_stat_t *s = &st[sector - 1];
                s->n++;
                s->tries += tries;
                s->deaths += g->deaths - deaths0;
                s->time += g->t - t0;
                s->crystals += g->crystals - cr0;
                s->cleared += !gave_up;
                for (int k = 0; k < 6; k++) s->cause[k] += g->cause[k] - c0[k];
                if (g_verbose)
                    printf("run %2d sector %2d: %d tries, %d lives lost, %.0f s, %d crystals, upgrades %d%d%d%d%d\n", r, sector,
                           tries, g->deaths - deaths0, (double)(g->t - t0), g->crystals - cr0, g->upg[0], g->upg[1],
                           g->upg[2], g->upg[3], g->upg[4]);
                if (gave_up) break;
                shop(g);
                sector++;
            }
            for (int k = 0; k < 6; k++) g_cause[k] += g->cause[k];
            if (!gave_up) {
                finished++;
                total_time += g->t;
            }
        }
        printf("campaign, difficulty %d, %s, %s, %d runs:\n", diff, skill == BOT_PERSON ? "a person" : "the good bot",
               land ? "lying down" : "upright", runs);
        printf("sector  reached  cleared  tries  lives lost  minutes  crystals   (lost to rock lapa enemy shot blast boss)\n");
        for (int i = 0; i < AG_SECTORS; i++) {
            sec_stat_t *s = &st[i];
            if (!s->n) continue;
            printf("  %2d     %3d      %3.0f%%    %4.2f     %4.2f      %4.1f     %4.0f      %3d %3d %3d %3d %3d %3d\n", i + 1, s->n,
                   100.0 * s->cleared / s->n, s->tries / s->n, s->deaths / s->n, s->time / s->n / 60.0, s->crystals / s->n,
                   s->cause[0], s->cause[1], s->cause[2], s->cause[3], s->cause[4], s->cause[5]);
        }
        printf("lives lost to: rocks %d, lapas %d, enemies %d, shots %d, blasts %d, bosses %d\n", g_cause[0], g_cause[1],
               g_cause[2], g_cause[3], g_cause[4], g_cause[5]);
        printf("finished the campaign: %d of %d", finished, runs);
        if (finished) printf(", in %.0f minutes on average", total_time / finished / 60.0);
        printf("\n");
    } else {
        double waves = 0, score = 0;
        int best = 0, worst = 1 << 30;
        double time = 0;
        int hist[64];
        memset(hist, 0, sizeof hist);
        for (int r = 0; r < runs; r++) {
            ag_new(g, AG_ARCADE, 1, diff, W, H, seed * 7919u + (uint32_t)r * 104729u + 1);
            ast_bot_init(&bot, skill, seed + (uint32_t)r * 31u);
            ag_sector_start(g, 1);
            while (g->state != AG_ST_OVER && g->t < 3600.0f) {
                ag_hand_t h;
                ast_bot_hand(&bot, g, 0, &h);
                ag_step(g, &h, NULL);
                g->nev = 0;
                g->nsnd = 0;
                g->shake = 0;
                if (g->since_kill > 240.0f) {
                    printf("  run %d wave %d: stalled\n", r, g->sector);
                    break;
                }
            }
            waves += g->sector;
            score += g->score;
            time += g->t;
            if (g->sector > best) best = g->sector;
            if (g->sector < worst) worst = g->sector;
            hist[g->sector < 63 ? g->sector : 63]++;
            if (g_verbose)
                printf("run %2d: wave %d, %d points, %.1f min, %d kills, %d bosses, %d jumps\n", r, g->sector, g->score,
                       (double)g->t / 60.0, g->kills, g->bosses, g->jumps);
        }
        printf("arcade, difficulty %d, %s, %s, %d runs: wave %.1f on average (%d..%d), %.0f points, %.1f minutes\n", diff,
               skill == BOT_PERSON ? "a person" : "the good bot", land ? "lying down" : "upright", runs, waves / runs,
               worst, best, score / runs, time / runs / 60.0);
        printf("waves reached:");
        for (int i = 1; i < 64; i++) {
            if (hist[i]) printf(" %d:%d", i, hist[i]);
        }
        printf("\n");
    }
    return 0;
}
