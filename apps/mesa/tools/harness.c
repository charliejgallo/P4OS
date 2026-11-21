/*
 * Juegos de mesa - the rules and the engines, checked on the Mac.
 *
 *   cc -O2 -I apps/mesa/main -o /tmp/mh apps/mesa/tools/harness.c apps/mesa/main/g_*.c
 *   ./mh perft          chess move generation against the published counts
 *   ./mh think <fen> <ms>  what the engine plays
 *   ./mh match <a> <b> <games>   chess level a against level b
 */
#include "g_ajedrez.h"
#include "mesa_game.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void hlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
static const char *htr(const char *s) { return s; }
static void hfree(void *p) { free(p); }

static const mesa_host_t HOST = { MESA_ABI, hlog, htr, malloc, hfree, now_ms };
const mesa_host_t *mesa_host = &HOST;

static int perft(void)
{
    static const struct { const char *fen; int d; uint64_t n; } T[] = {
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609 },
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603 },
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624 },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333 },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487 },
        { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594 },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        ch_pos_t p;
        if (!ch_fen(&p, T[i].fen)) {
            printf("bad fen %zu\n", i);
            bad++;
            continue;
        }
        uint32_t t0 = now_ms();
        uint64_t n = ch_perft(&p, T[i].d);
        printf("%s perft %d = %llu (%s, %u ms)\n", i ? "pos" : "start", T[i].d, (unsigned long long)n,
               n == T[i].n ? "ok" : "WRONG", now_ms() - t0);
        if (n != T[i].n) bad++;
    }
    return bad;
}

static int think(const char *fen, int ms, int level_noise)
{
    ch_pos_t p;
    if (!ch_fen(&p, fen)) return 1;
    volatile bool stop = false;
    ch_think_t t = { 0 };
    t.stop = &stop;
    t.now = now_ms;
    t.ms = (uint32_t)ms;
    t.noise = level_noise;
    t.seed = 1234;
    uint32_t m = ch_think(&p, &t);
    char san[16];
    ch_san(&p, m, san);
    printf("%s  depth %d score %d nodes %u (%u knps)\n", san, t.depth_done, t.score, t.nodes,
           t.nodes / (uint32_t)(ms ? ms : 1));
    return 0;
}

int main(int argc, char **argv)
{
    ch_tables();
    if (argc >= 2 && !strcmp(argv[1], "perft")) return perft();
    if (argc >= 4 && !strcmp(argv[1], "think")) return think(argv[2], atoi(argv[3]), argc > 4 ? atoi(argv[4]) : 0);
    fprintf(stderr, "harness perft | think <fen> <ms> [noise]\n");
    return 2;
}
