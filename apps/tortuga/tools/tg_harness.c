/*
 * Tortuga - the C core run on the Mac against the levels, the same check
 * as tools/tg_ref.py: every Logo solution on every map in both modes, the
 * pieces of the three languages, and the tree written back as Lua and
 * Python (which tg_ref.py then runs, with --gen).
 *
 *   cc -O1 -g -Wall -DTG_NO_I18N -Iapps/tortuga/main -o <scratch>/tg_harness \
 *      apps/tortuga/tools/tg_harness.c apps/tortuga/main/tg_world.c apps/tortuga/main/tg_logo.c -lm
 *   <scratch>/tg_harness apps/tortuga/levels/muestra.tl [--gen <dir>]
 */
#include "tg_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *tg_malloc(size_t n) { return calloc(1, n); }
void tg_free(void *p) { free(p); }

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static int run_one(const tg_level_t *lv, tg_prog_t *pr, int variant, bool hard, tg_ev_t *ev, char *why,
                   size_t whylen)
{
    tg_world_t w;
    bool paper = (lv->goals & TG_G_DRAW) != 0;
    tg_world_init(&w, lv, variant, hard, paper, ev, TG_EV_CAP);
    tg_logo_run(pr, &w);
    if (w.state == TG_ST_WIN && paper) {
        tg_prog_t fp;
        tg_prog_init(&fp, 2048);
        if (!tg_logo_parse(&fp, lv->figure)) {
            snprintf(why, whylen, "figure: %s", fp.err);
            return 0;
        }
        tg_ev_t *fev = calloc(TG_EV_CAP, sizeof(tg_ev_t));
        tg_level_t free_lv = *lv;
        free_lv.orders = 0;
        tg_world_t f;
        tg_world_init(&f, &free_lv, 0, false, true, fev, TG_EV_CAP);
        tg_logo_run(&fp, &f);
        bool same = tg_world_same_drawing(&w, &f);
        free(fev);
        tg_prog_free(&fp);
        if (!same) {
            snprintf(why, whylen, "el dibujo no es igual a la figura");
            return 0;
        }
        return 1;
    }
    if (w.state != TG_ST_WIN) {
        snprintf(why, whylen, "reason %d line %d arg %d %s", w.reason, w.fail_line, w.arg, w.msg);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *gen = argc > 3 && !strcmp(argv[2], "--gen") ? argv[3] : NULL;
    size_t len;
    char *txt = slurp(argv[1], &len);
    if (!txt) return 2;
    tg_levels_t L;
    tg_levels_parse(&L, txt, len);
    tg_ev_t *ev = calloc(TG_EV_CAP, sizeof(tg_ev_t));
    int bad = 0;
    for (int i = 0; i < L.n; i++) {
        const tg_level_t *lv = &L.lv[i];
        tg_prog_t pr;
        tg_prog_init(&pr, 4096);
        if (!tg_logo_parse(&pr, lv->sol[TG_SOL_LOGO] ? lv->sol[TG_SOL_LOGO] : "")) {
            printf("%-5s PARSE line %d: %s\n", lv->id, pr.err_line, pr.err);
            bad++;
            continue;
        }
        int ok = 1;
        int nv = lv->nmap ? lv->nmap : 1;
        for (int v = 0; v < nv; v++) {
            for (int hard = 0; hard < 2; hard++) {
                char why[160] = "";
                if (!run_one(lv, &pr, v, hard, ev, why, sizeof(why))) {
                    printf("  %s mapa %d %s: %s\n", lv->id, v + 1, hard ? "dificil" : "facil", why);
                    ok = 0;
                }
            }
        }
        int pl = tg_logo_pieces(&pr);
        int pu = lv->sol[TG_SOL_LUA] ? tg_text_pieces(lv->sol[TG_SOL_LUA], false) : -1;
        int pp = lv->sol[TG_SOL_PY] ? tg_text_pieces(lv->sol[TG_SOL_PY], true) : -1;
        bool same = pl == lv->star3 && (pu < 0 || pu == pl) && (pp < 0 || pp == pl);
        printf("%-5s %-34s %s logo %d lua %d python %d  ***<=%d%s\n", lv->id,
               lv->title[TG_ES] ? lv->title[TG_ES] : "", ok ? "ok " : "MAL", pl, pu, pp, lv->star3,
               same ? "" : "  <- no coincide");
        bad += !ok || !same;
        if (gen) {
            static char out[16384];
            const char *ext[] = { "logo", "lua", "py" };
            for (int k = 0; k < 3; k++) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.%s", gen, lv->id, ext[k]);
                tg_logo_gen(&pr, k, TG_ES, out, sizeof(out));
                FILE *f = fopen(path, "w");
                if (f) {
                    fputs(out, f);
                    fputs("\n", f);
                    fclose(f);
                }
            }
        }
        tg_prog_free(&pr);
    }
    free(ev);
    tg_levels_free(&L);
    free(txt);
    return bad ? 1 : 0;
}
