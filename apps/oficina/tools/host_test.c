/*
 * P4OS - Office: the formats on the Mac, without LVGL.
 *
 *     cc -DOF_HOST -I apps/oficina/main apps/oficina/tools/host_test.c \
 *        apps/oficina/main/of_{util,deflate,tinfl,zip,xml,sheet,formula,numfmt,csv,xlsx,ods,text}.c -o host_test
 *     ./host_test dump  book.xlsx                      the cells, shown and as typed
 *     ./host_test edit  book.xlsx out.xlsx 'A1=12' 'Resumen!B2==A1*2' ...
 *     ./host_test conv  book.csv out.xlsx              save in another format
 *     ./host_test text  in.txt out.txt 'line:text'    insert a line, save
 *     ./host_test sort  book.xlsx out.xlsx Sheet r1 r2 c1 c2 key desc
 *     ./host_test fx    '=SUMA(1;2)*3'                 parse, write back, evaluate
 *
 * tools/test_formats.sh runs it over the samples and checks what it saved.
 */
#include "of.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump(of_book_t *b)
{
    for (int i = 0; i < b->nsh; i++) {
        of_sheet_t *s = &b->sh[i];
        printf("== %s (freeze %d,%d)\n", s->name, s->freeze_r, s->freeze_c);
        for (int r = 0; r < s->nrows; r++) {
            for (int k = 0; k < s->rows[r].n; k++) {
                of_cell_t *c = &s->rows[r].c[k];
                if (c->v.t == OV_EMPTY && !c->f) continue;
                char ref[16], shown[256], src[256];
                bool right;
                of_ref_name(s->rows[r].row, c->col, ref);
                of_cell_display(b, c, shown, sizeof shown, &right);
                of_cell_source(b, c, src, sizeof src);
                printf("%-6s %-28s | %s%s\n", ref, shown, src, c->flags & OC_UNKNOWN_F ? "  (unknown fn: cached)" : "");
            }
        }
    }
}

static bool apply(of_book_t *b, const char *arg)
{
    char sheet[64] = "";
    const char *p = arg, *bang = strchr(arg, '!'), *eq = strchr(arg, '=');
    if (!eq) return false;
    if (bang && bang < eq) {
        snprintf(sheet, sizeof sheet, "%.*s", (int)(bang - arg), arg);
        p = bang + 1;
    }
    int ix = 0;
    for (int i = 0; i < b->nsh; i++)
        if (!strcmp(b->sh[i].name, sheet)) ix = i;
    int32_t r, c;
    int used;
    if (!of_parse_ref(p, &r, &c, &used)) return false;
    of_cell_input(b, &b->sh[ix], r, c, eq + 1);
    return true;
}

int main(int argc, char **argv)
{
    of_locale_set(true);
    if (getenv("OF_EN")) of_locale_set(false);
    char err[128] = "";
    if (argc >= 3 && !strcmp(argv[1], "fx")) {
        of_book_t *b = of_book_new(OF_FMT_XLSX);
        of_book_add_sheet(b, "Hoja1");
        of_cell_input(b, &b->sh[0], 0, 0, "10");
        of_cell_input(b, &b->sh[0], 1, 0, "20");
        of_cell_input(b, &b->sh[0], 2, 0, "texto");
        of_cell_input(b, &b->sh[0], 0, 1, argv[2]);
        of_book_recalc(b);
        dump(b);
        of_book_free(b);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "dump")) {
        of_book_t *b = of_book_load(argv[2], err, sizeof err);
        if (!b) { printf("load failed: %s\n", err); return 1; }
        dump(b);
        of_book_free(b);
        return 0;
    }
    if (argc >= 4 && (!strcmp(argv[1], "edit") || !strcmp(argv[1], "conv"))) {
        of_book_t *b = of_book_load(argv[2], err, sizeof err);
        if (!b) { printf("load failed: %s\n", err); return 1; }
        for (int i = 4; i < argc; i++)
            if (!apply(b, argv[i])) printf("bad edit %s\n", argv[i]);
        of_book_recalc(b);
        int fmt = of_fmt_from_ext(of_ext(argv[3]));
        if (!of_book_save(b, fmt, 0, argv[3], err, sizeof err)) { printf("save failed: %s\n", err); return 1; }
        dump(b);
        of_book_free(b);
        /* and read back */
        b = of_book_load(argv[3], err, sizeof err);
        if (!b) { printf("reload failed: %s\n", err); return 1; }
        printf("-- read back\n");
        dump(b);
        of_book_free(b);
        return 0;
    }
    if (argc >= 10 && !strcmp(argv[1], "sort")) {
        of_book_t *b = of_book_load(argv[2], err, sizeof err);
        if (!b) return 1;
        int ix = 0;
        for (int i = 0; i < b->nsh; i++)
            if (!strcmp(b->sh[i].name, argv[4])) ix = i;
        of_sheet_sort(b, &b->sh[ix], atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), atoi(argv[8]), atoi(argv[9]),
                      argc > 10 && atoi(argv[10]));
        if (!of_book_save(b, of_fmt_from_ext(of_ext(argv[3])), ix, argv[3], err, sizeof err)) { printf("save failed %s\n", err); return 1; }
        dump(b);
        of_book_free(b);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "text")) {
        of_text_t t;
        if (!of_text_load(&t, argv[2], err, sizeof err)) { printf("load failed %s\n", err); return 1; }
        printf("lines %d enc %d eol %d bom %d final %d\n", t.nlines, t.enc, t.eol, t.bom, t.final_eol);
        const char *colon = strchr(argv[4], ':');
        int line = atoi(argv[4]);
        size_t a, z;
        of_text_line_span(&t, line, &a, &z);
        of_text_insert(&t, a, colon + 1, strlen(colon + 1));
        of_text_insert(&t, a + strlen(colon + 1), "\n", 1);
        char buf[256];
        for (int i = 0; i < t.nlines && i < 8; i++) {
            of_text_line_copy(&t, i, buf, sizeof buf);
            printf("%3d %s\n", i + 1, buf);
        }
        long f = of_text_find(&t, 0, "PLACA", false, false);
        printf("find PLACA: %ld\n", f);
        if (!of_text_save(&t, argv[3], err, sizeof err)) { printf("save failed\n"); return 1; }
        of_text_free(&t);
        return 0;
    }
    fprintf(stderr, "usage: see the top of host_test.c\n");
    return 2;
}
