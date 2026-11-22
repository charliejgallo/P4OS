/*
 * P4OS - Office: the formats on the Mac, without LVGL.
 *
 *     cc -DOF_HOST -I apps/oficina/main apps/oficina/tools/host_test.c \
 *        apps/oficina/main/of_{util,deflate,tinfl,zip,xml,sheet,formula,numfmt,csv,xlsx,ods,text,doc,docx,odt,import,rtf,text2,archive,crypt,pdf,pdftext,pdfw,slides,pptx,odp,cfb,doc97,xls,ppt,iwa,pdffont,pdfdraw}.c -o host_test
 *     ./host_test dump  book.xlsx                      the cells, shown and as typed
 *     ./host_test edit  book.xlsx out.xlsx 'A1=12' 'Resumen!B2==A1*2' ...
 *     ./host_test conv  book.csv out.xlsx              save in another format
 *     ./host_test text  in.txt out.txt 'line:text'    insert a line, save
 *     ./host_test sort  book.xlsx out.xlsx Sheet r1 r2 c1 c2 key desc
 *     ./host_test fx    '=SUMA(1;2)*3'                 parse, write back, evaluate
 *     ./host_test read  file.rtf [copy.docx|.odt|.txt]  a reader's document (phase 3), and a copy
 *     ./host_test arc   archive.zip [dir]              an archive's entries, extracted to dir
 *     ./host_test pdf   file.docx|xlsx|md|txt out.pdf  written as PDF (phase 4); "read out.pdf" reads one
 *     ./host_test deck  slides.pptx|odp [out.pdf|docx|odt|txt]   a presentation's shapes (phase 5), and a copy
 *                       (OF_PDF_PW=... for a password)
 *     ./host_test render file.pdf page scale out.png  a page drawn (phase 7; page from 1, scale in pixels a point);
 *                       pictures through sips, the fonts a file lacks from the Mac's Arial, Times and Courier
 *
 * tools/test_formats.sh runs it over the samples and checks what it saved.
 */
#include "of.h"
#include "of_doc.h"
#include "of_arc.h"
#include "of_pdf.h"
#include "of_slides.h"
#include "of_cfb.h"
#include "of_pdfd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* What the board's system gives, on the Mac                                  */
/* -------------------------------------------------------------------------- */

/* pictures: sips makes a BMP, read and fitted here */
uint16_t *aos_hal_image_decode(const char *path, uint32_t offset, uint32_t size, int max_w, int max_h, bool fill,
                               int *out_w, int *out_h, int *src_w, int *src_h)
{
    char in[64] = "/tmp/of_host_in.jpg", cmd[600];
    if (size) {
        FILE *f = fopen(path, "rb");
        if (!f) return NULL;
        uint8_t *b = malloc(size);
        fseek(f, (long)offset, SEEK_SET);
        size_t got = fread(b, 1, size, f);
        fclose(f);
        f = fopen(in, "wb");
        fwrite(b, 1, got, f);
        fclose(f);
        free(b);
    } else {
        snprintf(in, sizeof in, "%.60s", path);
        if (strlen(path) >= 60) return NULL;
    }
    snprintf(cmd, sizeof cmd, "sips -s format bmp '%s' --out /tmp/of_host_out.bmp >/dev/null 2>&1", in);
    if (system(cmd)) return NULL;
    size_t n;
    uint8_t *b = of_file_read("/tmp/of_host_out.bmp", &n, 200u << 20);
    if (!b || n < 54 || b[0] != 'B') { of_free(b); return NULL; }
    int off = b[10] | b[11] << 8 | b[12] << 16 | b[13] << 24, w = b[18] | b[19] << 8 | b[20] << 16 | b[21] << 24;
    int h = (int)((uint32_t)b[22] | (uint32_t)b[23] << 8 | (uint32_t)b[24] << 16 | (uint32_t)b[25] << 24), bpp = b[28] | b[29] << 8;
    bool down = h > 0;
    if (h < 0) h = -h;
    int stride = ((w * bpp / 8) + 3) & ~3;
    if (src_w) *src_w = w;
    if (src_h) *src_h = h;
    float k = 1;
    if (!fill) { k = (float)max_w / w < (float)max_h / h ? (float)max_w / w : (float)max_h / h; if (k > 1) k = 1; }
    int ow = (int)(w * k + 0.5f), oh = (int)(h * k + 0.5f);
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    uint16_t *px = malloc((size_t)ow * oh * 2);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int sx = x * w / ow, sy = y * h / oh;
            int row = down ? h - 1 - sy : sy;
            const uint8_t *p = b + off + (size_t)row * stride + (size_t)sx * (bpp / 8);
            px[y * ow + x] = (uint16_t)((p[2] >> 3) << 11 | (p[1] >> 2) << 5 | p[0] >> 3);
        }
    of_free(b);
    *out_w = ow;
    *out_h = oh;
    return px;
}

void aos_hal_image_free(void *px) { free(px); }

static of_fprog_t s_fb[6];
static bool s_fb_tried;
static uint8_t *s_fb_bm;

static bool host_fallback(uint32_t cp, float px, uint8_t flags, of_fbglyph_t *out)
{
    static const char *const FILES[6] = { "Arial.ttf", "Arial Bold.ttf", "Times New Roman.ttf", "Times New Roman Bold.ttf", "Courier New.ttf", "Courier New Bold.ttf" };
    if (!s_fb_tried) {
        s_fb_tried = true;
        for (int i = 0; i < 6; i++) {
            char p[200];
            snprintf(p, sizeof p, "/System/Library/Fonts/Supplemental/%s", FILES[i]);
            size_t n;
            uint8_t *d = of_file_read(p, &n, 64u << 20);
            if (d) of_fprog_load(&s_fb[i], d, n, 2);
        }
    }
    int k = (flags & 4 ? 4 : flags & 8 ? 2 : 0) + (flags & 1 ? 1 : 0);
    of_fprog_t *F = &s_fb[k];
    if (!F->kind) return false;
    int gid = of_fprog_by_uni(F, cp);
    if (gid <= 0) return false;
    free(s_fb_bm);
    int w, h, l, t;
    s_fb_bm = of_pdf_glyph_a8(F, gid, px, &w, &h, &l, &t);
    if (!s_fb_bm) return false;
    *out = (of_fbglyph_t){ s_fb_bm, w, h, l, t, px, of_fprog_advance(F, gid) * px };
    return true;
}

static bool png_rgb565(const char *path, const uint16_t *px, int w, int h)
{
    size_t row = (size_t)w * 3;
    uint8_t *raw = malloc((row + 1) * (size_t)h);
    for (int y = 0; y < h; y++) {
        raw[y * (row + 1)] = 0;
        for (int x = 0; x < w; x++) {
            uint16_t v = px[y * w + x];
            uint8_t *o = raw + y * (row + 1) + 1 + x * 3;
            o[0] = (uint8_t)((v >> 11) << 3 | (v >> 13));
            o[1] = (uint8_t)(((v >> 5) & 63) << 2 | ((v >> 9) & 3));
            o[2] = (uint8_t)((v & 31) << 3 | ((v >> 2) & 7));
        }
    }
    of_sb_t z = { 0 }, o = { 0 };
    of_sb_put(&z, "\x78\x01", 2);
    of_deflate(raw, (row + 1) * (size_t)h, &z);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < (row + 1) * (size_t)h; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = b << 16 | a;
    char be[4] = { (char)(ad >> 24), (char)(ad >> 16), (char)(ad >> 8), (char)ad };
    of_sb_put(&z, be, 4);
    free(raw);
    of_sb_put(&o, "\x89PNG\r\n\x1a\n", 8);
    uint8_t ih[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
                       (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
    struct { const char *t; const uint8_t *d; size_t n; } C[3] = { { "IHDR", ih, 13 }, { "IDAT", (const uint8_t *)z.s, z.n }, { "IEND", NULL, 0 } };
    for (int i = 0; i < 3; i++) {
        char ln[4] = { (char)(C[i].n >> 24), (char)(C[i].n >> 16), (char)(C[i].n >> 8), (char)C[i].n };
        of_sb_put(&o, ln, 4);
        size_t at = o.n;
        of_sb_put(&o, C[i].t, 4);
        if (C[i].n) of_sb_put(&o, (const char *)C[i].d, C[i].n);
        uint32_t crc = of_crc32(0, o.s + at, o.n - at);
        char cb[4] = { (char)(crc >> 24), (char)(crc >> 16), (char)(crc >> 8), (char)crc };
        of_sb_put(&o, cb, 4);
    }
    bool ok = of_file_write(path, o.s, o.n);
    of_sb_free(&z);
    of_sb_free(&o);
    return ok;
}

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

static void docdump(of_doc_t *d)
{
    for (int i = 0; i < d->np; i++) {
        of_para_t *p = &d->p[i];
        printf("%3d %s%s%s c%-2d ", i, p->heading ? "H" : " ", p->heading ? (char[2]){ (char)('0' + p->heading), 0 } : " ",
               p->locked ? "L" : " ", p->cell);
        if (p->indent) printf("%*s", p->indent * 2, "");
        if (p->align) printf("[%c] ", "LCRJ"[p->align & 3]);
        if (p->label[0]) printf("%s ", p->label);
        for (int k = 0; k < p->len; k++) {
            if (k + 2 < p->len && !memcmp(p->txt + k, OF_OBJ_CHAR, 3)) {
                of_obj_t *o = of_para_obj_at(d, p, k);
                static const char *const K[] = { "hid", "img", "mark", "txt", "brk", "note" };
                printf("<%s%s%s", o ? K[o->kind] : "?", o && o->text ? ":" : "", o && o->text ? o->text : "");
                if (o && o->target) printf(" %s%s %dx%d", o->file ? "file:" : "", o->target, (int)o->w, (int)o->h);
                printf(">");
                k += 2;
                continue;
            }
            of_rstyle_t e;
            of_doc_effective(d, p, p->at[k], &e);
            if (k == 0 || p->at[k] != p->at[k - 1]) {
                printf("%s%s%s%s", e.b == OF_TRI_ON ? "*" : "", e.i == OF_TRI_ON ? "/" : "", e.s == OF_TRI_ON ? "-" : "", e.link >= 0 ? "_" : "");
                if (e.color) printf("{%06X}", (unsigned)(e.color & 0xFFFFFF));
                if (e.link >= 0 && e.link < d->nlinks && (k == 0 || d->rs[p->at[k - 1]].link != e.link)) printf("(%s)", d->links[e.link]);
            }
            putchar(p->txt[k] == '\n' ? '|' : p->txt[k]);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    of_locale_set(true);
    if (getenv("OF_EN")) of_locale_set(false);
    char err[128] = "";
    if (argc >= 6 && !strcmp(argv[1], "render")) {
        of_pdfr_fallback(host_fallback);
        of_pdfr_t *R = of_pdfr_open(argv[2], getenv("OF_PDF_PW"), err, sizeof err);
        if (!R) { printf("open failed %s\n", err); return 1; }
        int pg = atoi(argv[3]) - 1;
        float sc = (float)atof(argv[4]), w, h;
        of_pdfr_size(R, pg, &w, &h);
        int W = (int)(w * sc + 0.5f), H = (int)(h * sc + 0.5f);
        uint16_t *px = malloc((size_t)W * H * 2);
        bool ok = of_pdfr_draw(R, pg, sc, 0, 0, px, W, H, NULL);
        printf("pages %d, page %d: %.0fx%.0f pt -> %dx%d %s\n", of_pdfr_count(R), pg + 1, w, h, W, H, ok ? "ok" : "stopped");
        png_rgb565(argv[5], px, W, H);
        free(px);
        of_pdfr_close(R);
        for (int i = 0; i < 6; i++) of_fprog_free(&s_fb[i]);
        free(s_fb_bm);
        return 0;
    }
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
    if (argc >= 3 && !strcmp(argv[1], "docdump")) {
        of_doc_t *d = of_doc_load(argv[2], err, sizeof err);
        if (!d) { printf("load failed: %s\n", err); return 1; }
        docdump(d);
        of_doc_free(d);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "arc")) {
        of_arc_t *a = of_arc_open(argv[2], err, sizeof err);
        if (!a) { printf("open failed: %s\n", err); return 1; }
        printf("== kind %d, %d entries\n", a->kind, a->n);
        for (int i = 0; i < a->n; i++) {
            printf("%3d %s %8llu %s%s\n", i, a->e[i].dir ? "d" : "-", (unsigned long long)a->e[i].size, a->e[i].name, a->e[i].locked ? " (locked)" : "");
            if (argc >= 4 && !a->e[i].dir) {
                char out[600];
                snprintf(out, sizeof out, "%s/%s", argv[3], a->e[i].name);
                if (!of_arc_extract(a, i, out, err, sizeof err)) printf("    extract failed: %s\n", err);
            }
        }
        of_arc_free(a);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "deck")) {
        /* a presentation: every shape with its place, fill and text */
        of_deck_t *dk = of_deck_load(argv[2], err, sizeof err);
        if (!dk) { printf("load failed: %s\n", err); return 1; }
        printf("== %s %.0fx%.0f pt, %d slides\n", dk->kind, dk->w, dk->h, dk->ns);
        for (int i = 0; i < dk->ns; i++) {
            sl_slide_t *s = &dk->s[i];
            printf("-- slide %d%s bg %06X title \"%s\"%s\n", i + 1, s->hidden ? " (hidden)" : "", (unsigned)(s->bg & 0xFFFFFF), s->title ? s->title : "", s->bg_pic ? " bgpic" : "");
            for (int k = 0; k < s->nsh; k++) {
                sl_shape_t *sh = &s->sh[k];
                static const char *const KN[] = { "text", "pic", "table", "line" };
                printf("  %s%s g%d [%.0f %.0f %.0f %.0f]%s fill %06X line %06X/%.1f%s%s\n", sh->placeholder ? "" : "(m)", KN[sh->kind], sh->geom,
                       sh->x, sh->y, sh->w, sh->h, sh->rot ? " rot" : "", (unsigned)(sh->fill & 0xFFFFFF), (unsigned)(sh->line & 0xFFFFFF), sh->line_w,
                       sh->pic ? " pic=" : "", sh->pic ? sh->pic : "");
                for (int pi = 0; pi < sh->tx.np; pi++) {
                    sl_para_t *p = &sh->tx.p[pi];
                    printf("     L%d a%d m%.0f i%.0f %s%s", p->level, p->align, p->margin, p->indent, p->bullet, p->autonum ? "#" : "");
                    if (p->autonum) printf("%d", p->number);
                    for (int r = 0; r < p->nr; r++)
                        printf(" [%.0f%s%s %06X] %s", p->r[r].size, p->r[r].b ? "b" : "", p->r[r].i ? "i" : "", (unsigned)(p->r[r].color & 0xFFFFFF), p->r[r].t);
                    printf("\n");
                }
                for (int c = 0; c < sh->ncells; c++) {
                    sl_cell_t *cl = &sh->cells[c];
                    printf("     cell r%d c%d%s fill %06X:", cl->row, cl->col, cl->hidden ? " (hidden)" : "", (unsigned)(cl->fill & 0xFFFFFF));
                    for (int pi = 0; pi < cl->tx.np; pi++) for (int r = 0; r < cl->tx.p[pi].nr; r++) printf(" %s", cl->tx.p[pi].r[r].t);
                    printf("\n");
                }
            }
            if (s->notes) printf("  notes: %s\n", s->notes);
        }
        if (argc >= 4) {
            const char *e = of_ext(argv[3]);
            bool ok;
            if (!strcmp(e, "pdf")) ok = of_pdf_write_deck(dk, argv[3], of_basename(argv[2]), false, err, sizeof err);
            else {
                of_doc_t *d = of_deck_to_doc(dk, true);
                ok = d && of_doc_save(d, !strcmp(e, "odt") ? OF_DOC_ODT : !strcmp(e, "txt") ? OF_DOC_TEXT : OF_DOC_DOCX, argv[3], err, sizeof err);
                if (d && getenv("OF_DUMP")) docdump(d);
                of_doc_free(d);
            }
            printf("%s %s\n", ok ? "wrote" : "failed", argv[3]);
        }
        of_deck_free(dk);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "pdf")) {
        /* a document, a sheet or a text, written as PDF */
        const char *e = of_ext(argv[2]);
        bool ok;
        if (of_is_sheet_ext(e)) {
            of_book_t *b = of_book_load(argv[2], err, sizeof err);
            if (!b) { printf("load failed: %s\n", err); return 1; }
            ok = of_pdf_write_book(b, -1, argv[3], of_basename(argv[2]), err, sizeof err);
            of_book_free(b);
        } else if (of_is_doc_ext(e) || of_reader_for(e) || of_is_tree_ext(e)) {
            of_doc_t *d = of_is_doc_ext(e) ? of_doc_load(argv[2], err, sizeof err) : of_read_any(argv[2], err, sizeof err);
            if (!d) { printf("load failed: %s\n", err); return 1; }
            ok = of_pdf_write_doc(d, argv[3], of_basename(argv[2]), false, false, err, sizeof err);
            of_doc_free(d);
        } else {
            size_t n;
            char *t = (char *)of_file_read(argv[2], &n, 64u << 20);
            if (!t) { printf("read failed\n"); return 1; }
            ok = of_pdf_write_text(t, n, argv[3], of_basename(argv[2]), err, sizeof err);
            of_free(t);
        }
        printf("%s %s\n", ok ? "wrote" : "failed", argv[3]);
        return ok ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "read")) {
        of_doc_t *d = getenv("OF_PDF_PW") ? of_pdf_load(argv[2], getenv("OF_PDF_PW"), err, sizeof err) : of_read_any(argv[2], err, sizeof err);
        if (!d) { printf("read failed: %s\n", err); return 1; }
        printf("== %s, %d paragraphs, %d tables%s%s\n", d->kind, d->np, d->ntables, d->note ? ", " : "", d->note ? d->note : "");
        docdump(d);
        if (argc >= 4) {
            const char *e = of_ext(argv[3]);
            int fmt = !strcmp(e, "odt") ? OF_DOC_ODT : !strcmp(e, "txt") ? OF_DOC_TEXT : OF_DOC_DOCX;
            if (!of_doc_save(d, fmt, argv[3], err, sizeof err)) { printf("save failed: %s\n", err); return 1; }
            printf("saved %s\n", argv[3]);
        }
        of_doc_free(d);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "docedit")) {
        of_doc_t *d = of_doc_load(argv[2], err, sizeof err);
        if (!d) { printf("load failed: %s\n", err); return 1; }
        for (int i = 4; i < argc; i++) {
            char op[16];
            int a = 0, b = 0, c = 0, n = 0;
            if (sscanf(argv[i], "%15[a-z]:%d:%d%n", op, &a, &b, &n) >= 2) {
                const char *rest = argv[i] + n;
                if (!strcmp(op, "ins")) of_para_insert(d, a, b < 0 ? d->p[a].len : b, rest + 1, (int)strlen(rest + 1), d->p[a].len ? d->p[a].at[d->p[a].len - 1] : 0);
                else if (!strcmp(op, "del")) { sscanf(rest, ":%d", &c); of_para_delete(d, a, b, c); }
                else if (!strcmp(op, "split")) of_doc_split(d, a, b);
                else if (!strcmp(op, "merge")) printf("merge %d: %s\n", a, of_doc_merge(d, a) ? "ok" : "refused");
                else if (!strcmp(op, "bold") || !strcmp(op, "italic")) {
                    sscanf(rest, ":%d", &c);
                    of_rstyle_t v = { .b = OF_TRI_ON, .i = OF_TRI_ON };
                    of_para_restyle(d, a, b, c, op[0] == 'b' ? OF_RS_B : OF_RS_I, &v);
                } else if (!strcmp(op, "head")) { d->p[a].heading = (uint8_t)b; d->p[a].pmask |= OF_PP_STYLE; d->p[a].dirty = true; }
                else if (!strcmp(op, "align")) { d->p[a].align = (uint8_t)b; d->p[a].pmask |= OF_PP_ALIGN; d->p[a].dirty = true; }
                else printf("bad op %s\n", argv[i]);
            }
        }
        const char *e = of_ext(argv[3]);
        int fmt = !strcmp(e, "odt") ? OF_DOC_ODT : !strcmp(e, "txt") ? OF_DOC_TEXT : OF_DOC_DOCX;
        if (!of_doc_save(d, fmt, argv[3], err, sizeof err)) { printf("save failed: %s\n", err); return 1; }
        of_doc_free(d);
        if (fmt == OF_DOC_TEXT) return 0;
        d = of_doc_load(argv[3], err, sizeof err);
        if (!d) { printf("reload failed: %s\n", err); return 1; }
        printf("-- read back\n");
        docdump(d);
        of_doc_free(d);
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
