/*
 * DIBUJO - the engine, checked on the Mac without the board.
 *
 *   cc -O1 -Wall -Wextra -DDIB_HOST -Iapps/dibujo/main apps/dibujo/tools/dib_harness.c \
 *      apps/dibujo/main/dib_doc.c apps/dibujo/main/dib_paint.c apps/dibujo/main/dib_raster.c \
 *      apps/dibujo/main/dib_png.c -lm -o /tmp/dib_harness && /tmp/dib_harness /tmp/dib
 *   python3 apps/dibujo/tools/dib_pngcheck.py /tmp/dib
 *
 * Paints with every brush, draws every shape, fills, merges and moves
 * layers, and checks: undo brings back exactly the pixels before (and redo
 * the ones after), cancelling a stroke leaves nothing, the history keeps to
 * its memory budget, a .dib comes back from disk pixel for pixel, and the
 * PNG it writes decodes (dib_pngcheck.py, with Python's zlib) to the very
 * rows dib_compose_row_rgba gives. It also leaves the pictures in the
 * folder, to look at.
 */
#include "dib_doc.h"
#include "dib_paint.h"
#include "dib_png.h"
#include "dib_raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A hash of every layer's pixels plus the layer list. */
static uint64_t doc_hash(const dib_doc_t *d)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < d->nlayers; i++) {
        const dib_layer_t *l = &d->layers[i];
        h = (h ^ l->opacity ^ ((uint64_t)l->visible << 8)) * 1099511628211ull;
        for (int y = 0; y < d->h; y++) {
            for (int x = 0; x < d->w; x++) {
                h = (h ^ dib_layer_px(d, i, x, y)) * 1099511628211ull;
            }
        }
    }
    return (h ^ d->bg) * 1099511628211ull;
}

static bool rows_cb(void *user, int y, uint8_t *rgba)
{
    dib_compose_row_rgba((const dib_doc_t *)user, y, rgba);
    return true;
}

static void dump_rgba(const dib_doc_t *d, const char *path)
{
    FILE *f = fopen(path, "wb");
    uint8_t *row = malloc((size_t)d->w * 4);
    for (int y = 0; y < d->h; y++) {
        dib_compose_row_rgba(d, y, row);
        fwrite(row, 1, (size_t)d->w * 4, f);
    }
    free(row);
    fclose(f);
}

static void stroke_line(dib_doc_t *d, const dib_brush_t *b, float x0, float y0, float x1, float y1, int steps)
{
    dib_stroke_t s;
    if (!dib_stroke_begin(&s, d, b, x0, y0)) return;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        /* a wobble, the way a finger draws */
        float wob = sinf(t * 25.0f) * 3.0f;
        dib_stroke_to(&s, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t + wob);
        dib_stroke_hold(&s, 16);
    }
    dib_rect_t r;
    dib_stroke_end(&s);
    while (dib_stroke_take_dirty(&s, &r)) dib_compose(d, r);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/dib";
    mkdir(dir, 0777);
    char path[256];
    clock_t t0 = clock();

    dib_doc_t *d = dib_doc_new(1000, 700, 0xFFFFFFFFu, 64u << 20);
    CHECK(d, "new");
    if (!d) return 1;

    /* ---- every brush ---- */
    dib_brush_t b = { 0 };
    const dib_px_t colors[DIB_BR_COUNT] = { 0xFF202020, 0xFF2E7DD7, 0xFFE0402A, 0xFFFFD60A, 0 };
    for (int k = 0; k < DIB_BR_COUNT; k++) {
        b.kind = k;
        b.size = dib_brush_defs[k].def_size;
        b.opacity = dib_brush_defs[k].def_opacity;
        b.smooth = 0.4f;
        b.color = colors[k];
        uint64_t before = doc_hash(d);
        float y = 80.0f + k * 110.0f;
        stroke_line(d, &b, 60, y, 900, y + 40, 120);
        uint64_t after = doc_hash(d);
        if (k != DIB_BR_ERASER) CHECK(before != after, "brush %d painted nothing", k);
        CHECK(dib_undo(d, NULL), "undo brush %d", k);
        CHECK(doc_hash(d) == before, "undo brush %d is not exact", k);
        CHECK(dib_redo(d, NULL), "redo brush %d", k);
        CHECK(doc_hash(d) == after, "redo brush %d is not exact", k);
    }
    /* the eraser across everything, then the marker over itself */
    b.kind = DIB_BR_ERASER;
    b.size = 40;
    b.opacity = 1;
    stroke_line(d, &b, 500, 20, 520, 680, 200);
    b.kind = DIB_BR_MARKER;
    b.size = 30;
    b.opacity = 0.5f;
    b.color = 0xFF00A050;
    {
        dib_stroke_t s;
        dib_stroke_begin(&s, d, &b, 100, 650);
        for (int i = 0; i < 300; i++) dib_stroke_to(&s, 100 + (i % 100) * 3.0f, 650);
        dib_stroke_end(&s);
        dib_px_t p = dib_layer_px(d, 0, 200, 650);
        /* 50 % on an empty layer, however many passes went over it */
        CHECK(DIB_A(p) >= 126 && DIB_A(p) <= 129 && (p & 0xFFFFFF) == 0x00A050,
              "marker built up over itself: %08X", (unsigned)p);
    }

    /* ---- cancel leaves nothing ---- */
    {
        uint64_t before = doc_hash(d);
        int nundo = d->nundo;
        dib_stroke_t s;
        b.kind = DIB_BR_SOFT;
        b.size = 50;
        b.opacity = 1;
        dib_stroke_begin(&s, d, &b, 300, 300);
        dib_stroke_to(&s, 400, 350);
        dib_stroke_cancel(&s);
        CHECK(doc_hash(d) == before && d->nundo == nundo, "cancel left a trace");
    }

    /* ---- layers and shapes ---- */
    int l2 = dib_layer_add(d, -1, "formas");
    CHECK(l2 == 1 && d->active == 1, "layer add");
    dib_obj_t o;
    memset(&o, 0, sizeof o);
    o.stroke = o.fill = true;
    o.stroke_w = 6;
    o.stroke_c = 0xFF1C1C1E;
    o.fill_c = 0xFF5AC8FA;
    const struct { int kind; float cx, cy, w, h, ang, r; } shapes[] = {
        { DIB_OBJ_RECT, 160, 160, 220, 140, 0.0f, 0 },
        { DIB_OBJ_RECT, 420, 160, 220, 140, 0.3f, 30 },
        { DIB_OBJ_ELLIPSE, 700, 160, 240, 140, -0.4f, 0 },
        { DIB_OBJ_LINE, 160, 420, 260, 0, 0.5f, 0 },
        { DIB_OBJ_ARROW, 440, 420, 260, 0, -0.6f, 0 },
        { DIB_OBJ_POLY, 760, 450, 200, 200, 0.2f, 0 },
    };
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        o.kind = shapes[i].kind;
        o.cx = shapes[i].cx; o.cy = shapes[i].cy; o.w = shapes[i].w; o.h = shapes[i].h;
        o.ang = shapes[i].ang; o.radius = shapes[i].r;
        if (o.kind == DIB_OBJ_POLY) {
            o.npoly = 5;     /* a star's outline would self-cross; a house */
            const dib_pt_t pts[5] = { { -0.5f, -0.1f }, { 0, -0.5f }, { 0.5f, -0.1f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            memcpy(o.poly, pts, sizeof pts);
        }
        dib_rect_t ch = { 0 };
        dib_obj_render(d, &o, &ch);
        dib_compose(d, ch);
        CHECK(!dib_rect_empty(&d->flt_box), "shape %zu drew nothing", i);
        uint64_t before = doc_hash(d);
        ch = (dib_rect_t){ 0 };
        dib_flt_commit(d, &ch);
        dib_compose(d, ch);
        CHECK(doc_hash(d) != before, "shape %zu commit changed nothing", i);
    }
    /* the inside of a ring is a hole */
    {
        dib_px_t c = dib_layer_px(d, 1, 160, 160);
        CHECK(c == 0xFF5AC8FA, "rect fill %08X", (unsigned)c);
        dib_px_t e = dib_layer_px(d, 1, 160 - 110, 160);
        CHECK(e == 0xFF1C1C1E, "rect edge %08X", (unsigned)e);
    }

    /* ---- text-like bitmap, turned ---- */
    {
        dib_obj_t t;
        memset(&t, 0, sizeof t);
        t.kind = DIB_OBJ_BITMAP;
        t.bw = 120; t.bh = 40;
        t.bmp = dib_alloc((size_t)t.bw * t.bh * 4);
        for (int y = 0; y < t.bh; y++)
            for (int x = 0; x < t.bw; x++)
                t.bmp[y * t.bw + x] = ((x / 10 + y / 10) & 1) ? 0xFFD93A6A : 0x00000000;
        t.cx = 300; t.cy = 600; t.w = 240; t.h = 80; t.ang = 0.25f;
        dib_rect_t ch = { 0 };
        dib_obj_render(d, &t, &ch);
        dib_flt_commit(d, &ch);
        dib_compose(d, ch);
        /* and exact at its own size */
        t.cx = 860; t.cy = 640; t.w = 120; t.h = 40; t.ang = 0;
        ch = (dib_rect_t){ 0 };
        dib_obj_render(d, &t, &ch);
        dib_flt_commit(d, &ch);
        dib_compose(d, ch);
        CHECK(dib_layer_px(d, 1, 800 + 15, 620 + 5) == 0xFFD93A6A, "bitmap 1:1 %08X",
              (unsigned)dib_layer_px(d, 1, 815, 625));
        dib_obj_free(&t);
    }

    /* ---- fill ---- */
    {
        dib_layer_add(d, -1, "relleno");
        uint64_t before = doc_hash(d);
        dib_rect_t ch = { 0 };
        bool did = dib_fill(d, 160, 160, 0xFFFF9F0A, 1.0f, 24, true, &ch);
        CHECK(did, "fill did nothing");
        dib_compose(d, ch);
        CHECK(dib_layer_px(d, 2, 160, 160) == 0xFFFF9F0A, "fill missed");
        CHECK(dib_layer_px(d, 2, 5, 5) == 0, "fill leaked out of the rectangle");
        dib_undo(d, &ch);
        CHECK(doc_hash(d) == before, "undo fill");
        dib_redo(d, &ch);
        dib_compose(d, (dib_rect_t){ 0, 0, d->w, d->h });
    }

    /* ---- layer operations round trip ---- */
    {
        uint64_t h0 = doc_hash(d);
        int n0 = d->nlayers;
        dib_layer_props(d, 1, 128, true);
        dib_layer_move(d, 2, 0);
        dib_layer_dup(d, 0);
        dib_layer_merge_down(d, 1);
        dib_layer_delete(d, 0);
        dib_set_bg(d, 0x00000000);
        for (int i = 0; i < 6; i++) CHECK(dib_undo(d, NULL), "undo layer op %d", i);
        CHECK(doc_hash(d) == h0 && d->nlayers == n0, "layer ops did not undo exactly");
        for (int i = 0; i < 6; i++) CHECK(dib_redo(d, NULL), "redo layer op %d", i);
        CHECK(d->bg == 0, "redo bg");
        for (int i = 0; i < 6; i++) dib_undo(d, NULL);
        CHECK(doc_hash(d) == h0, "layer ops second undo");
        dib_compose(d, (dib_rect_t){ 0, 0, d->w, d->h });
    }

    /* ---- a selection moved ---- */
    {
        uint64_t h0 = doc_hash(d);
        d->active = 0;
        dib_rect_t sel = { 40, 40, 300, 240 };
        dib_obj_t s;
        memset(&s, 0, sizeof s);
        s.kind = DIB_OBJ_BITMAP;
        s.bmp = dib_copy_rect(d, sel, true);
        s.bw = s.w = 260; s.bh = s.h = 200;
        s.cx = 170 + 37; s.cy = 140 + 21;
        dib_rect_t ch = { 0 };
        dib_obj_render(d, &s, &ch);
        dib_flt_commit(d, &ch);
        CHECK(doc_hash(d) != h0, "move did nothing");
        dib_undo(d, NULL);
        dib_undo(d, NULL);
        CHECK(doc_hash(d) == h0, "move undo");
        dib_obj_free(&s);
        dib_compose(d, (dib_rect_t){ 0, 0, d->w, d->h });
    }

    /* ---- save and load ---- */
    {
        snprintf(path, sizeof path, "%s/prueba.dib", dir);
        CHECK(dib_save(d, path), "save");
        struct stat st;
        stat(path, &st);
        dib_doc_t *e = dib_load(path, 64u << 20);
        CHECK(e, "load");
        if (e) {
            CHECK(doc_hash(e) == doc_hash(d), "load is not exact");
            CHECK(e->nlayers == d->nlayers && e->active == d->active, "load layers");
            int w, h, pw, ph;
            uint16_t prev[DIB_PREVIEW * DIB_PREVIEW];
            CHECK(dib_peek(path, &w, &h, prev, &pw, &ph) && w == 1000 && h == 700 && pw == 160 && ph == 112,
                  "peek %dx%d %dx%d", w, h, pw, ph);
            dib_doc_free(e);
        }
        printf("dib: %ld bytes for %d layers of %dx%d\n", (long)st.st_size, d->nlayers, d->w, d->h);
    }

    /* ---- PNG ---- */
    {
        snprintf(path, sizeof path, "%s/prueba.png", dir);
        int prog = 0;
        clock_t p0 = clock();
        CHECK(dib_png_write(path, d->w, d->h, rows_cb, d, &prog), "png");
        CHECK(prog == 1000, "png progress %d", prog);
        struct stat st;
        stat(path, &st);
        printf("png: %ld bytes (raw %d), %.0f ms\n", (long)st.st_size, d->w * d->h * 4,
               (double)(clock() - p0) * 1000.0 / CLOCKS_PER_SEC);
        snprintf(path, sizeof path, "%s/prueba.rgba", dir);
        dump_rgba(d, path);
        /* transparent background too */
        dib_set_bg(d, 0);
        snprintf(path, sizeof path, "%s/transparente.png", dir);
        dib_png_write(path, d->w, d->h, rows_cb, d, NULL);
        snprintf(path, sizeof path, "%s/transparente.rgba", dir);
        dump_rgba(d, path);
        dib_undo(d, NULL);
    }

    /* ---- the memory budget ---- */
    {
        dib_doc_t *m = dib_doc_new(1280, 1024, 0xFFFFFFFFu, 8u << 20);
        dib_brush_t big = { DIB_BR_SOFT, 200, 1, 0, 0xFF000000, DIB_SYM_4 };
        for (int i = 0; i < 40; i++) stroke_line(m, &big, 0, (float)(i * 25 % 1024), 1280, (float)(i * 37 % 1024), 60);
        CHECK(m->tile_bytes + m->hist_bytes <= m->budget, "over budget: %zu + %zu > %zu",
              m->tile_bytes, m->hist_bytes, m->budget);
        printf("budget: %d steps kept, %zu KB layers, %zu KB history\n", m->nundo, m->tile_bytes >> 10,
               m->hist_bytes >> 10);
        while (dib_undo(m, NULL)) {
        }
        size_t real = 0;
        for (int t = 0; t < m->ntiles; t++) if (m->layers[0].tiles[t]) real += DIB_TILE_BYTES;
        CHECK(real == m->tile_bytes, "tile accounting %zu != %zu", real, m->tile_bytes);
        dib_doc_free(m);
    }

    printf("%s, %.0f ms\n", fails ? "FAILED" : "ok", (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
    dib_doc_free(d);
    return fails ? 1 : 0;
}
