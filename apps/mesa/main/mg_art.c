/*
 * Juegos de mesa - the art. Each set is a pack in /apps: mesa_p4.pak the
 * wooden one, which comes with the app and has every game; mesa_marmol,
 * mesa_vidrio and mesa_metal the others, chosen in the settings. Whatever
 * the chosen set lacks is taken from the wooden one, so a set may bring
 * only some games. A module's game has its own pack, mesa_<word>_p4.pak,
 * which stands in for the wooden one.
 *
 * For a board called B (a game's board() says it: "aj", "dm10"):
 *   B_bg    the table with the board, a JPEG, seen by the 3D camera
 *   B_geo   where the board is on it: "S=80 E=55 rot=0 cx=640 cy=700"
 *   B_bgp, B_geop   the same upright, when it differs (backgammon turns)
 *   B_fl, B_flgeo   the flat view's, from above
 *   B_<kind>        a kind's sheet of frames (the kind's sprite name), and
 *   B_<kind>_f      its flat one
 *   B_over          what is drawn over the pieces (four in a row's frame)
 *   dado, dado_f    the six faces of a die
 *   <prefix>_th     the game's picture for the menu (RGB565)
 */
#include "mg_app.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const SET_FILE[SET_N] = { "mesa_p4.pak", "mesa_marmol_p4.pak", "mesa_vidrio_p4.pak",
                                            "mesa_metal_p4.pak" };

static void pack_path(char *out, size_t n, const char *file)
{
    snprintf(out, n, "%s/%s", aos_hal_path_apps(), file);
}

bool mg_art_have_set(int set)
{
    if (set < 0 || set >= SET_N) return false;
    char p[160];
    pack_path(p, sizeof p, SET_FILE[set]);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static const mg_slot_t *slot_of(const mesa_game_t *g)
{
    for (int i = 0; i < mg_ngames; i++) {
        if (mg_games[i].g == g) return &mg_games[i];
    }
    return NULL;
}

/* the second pack: the module's own, or the wooden set */
static void base_file(const mesa_game_t *g, char *out, size_t n)
{
    const mg_slot_t *s = slot_of(g);
    if (s && s->file[0]) snprintf(out, n, "mesa_%.20s_p4.pak", g->word);
    else snprintf(out, n, "%s", SET_FILE[SET_MADERA]);
}

static const mg_entry_t *find(mg_art_t *a, const char *name, mg_pak_t **pak)
{
    for (int i = 0; i < 2; i++) {
        if (!a->open[i]) continue;
        const mg_entry_t *e = mg_pak_find(&a->pak[i], name);
        if (e) {
            *pak = &a->pak[i];
            return e;
        }
    }
    return NULL;
}

static void *load(mg_art_t *a, const char *name, const mg_entry_t **out)
{
    mg_pak_t *p = NULL;
    const mg_entry_t *e = find(a, name, &p);
    if (out) *out = e;
    return e ? mg_pak_load(p, e) : NULL;
}

static void parse_geo(const char *s, mg_geo_t *g)
{
    memset(g, 0, sizeof *g);
    g->S = 80;
    g->E = 55;
    while (s && *s) {
        char key[8];
        float v;
        int k = 0;
        while (*s == ' ' || *s == '\n') s++;
        while (*s && *s != '=' && k < 7) key[k++] = *s++;
        key[k] = 0;
        if (*s != '=') break;
        s++;
        v = strtof(s, (char **)&s);
        if (!strcmp(key, "S")) g->S = v;
        else if (!strcmp(key, "E")) g->E = v;
        else if (!strcmp(key, "rot")) g->rot = (int)v & 3;
        else if (!strcmp(key, "cx")) g->cx = v;
        else if (!strcmp(key, "cy")) g->cy = v;
    }
}

static bool decode_jpeg(mg_art_t *a, const char *name)
{
    const mg_entry_t *e;
    uint8_t *raw = (uint8_t *)load(a, name, &e);
    if (!raw) return false;
    bool ok = false;
    aos_jpeg_t *j = aos_hal_jpeg_open();
    if (j) {
        int w = 0, h = 0, stride = 0;
        const uint16_t *px = aos_hal_jpeg_decode(j, raw, e->rawlen, &w, &h, &stride);
        if (px && w > 0 && h > 0) {
            a->pic = (uint16_t *)mg_malloc((size_t)w * (size_t)h * 2);
            if (a->pic) {
                for (int y = 0; y < h; y++) memcpy(a->pic + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 2);
                a->geo.pw = w;
                a->geo.ph = h;
                ok = true;
            }
        }
        aos_hal_jpeg_close(j);
    }
    mg_free(raw);
    return ok;
}

static bool sheet(mg_art_t *a, const char *name, mg_sheet_t *s)
{
    const mg_entry_t *e;
    uint8_t *raw = (uint8_t *)load(a, name, &e);
    if (!raw) return false;
    if (e->type != PK_SHEET || !mg_sheet_bind(s, raw, e->rawlen)) {
        mg_free(raw);
        memset(s, 0, sizeof *s);
        return false;
    }
    return true;
}

static void free_pics(mg_art_t *a)
{
    mg_free(a->pic);
    a->pic = NULL;
    a->have_pic = false;
    for (int i = 0; i < 32; i++) {
        if (a->kind[i].ok) mg_free(a->kind[i].sheet.data);
        memset(&a->kind[i], 0, sizeof a->kind[i]);
    }
    if (a->have_over) mg_free(a->over.data);
    a->have_over = false;
    if (a->have_dice) mg_free(a->dice.data);
    a->have_dice = false;
}

void mg_art_free(mg_art_t *a)
{
    free_pics(a);
    for (int i = 0; i < 2; i++) {
        if (a->open[i]) mg_pak_close(&a->pak[i]);
        a->open[i] = false;
    }
    a->game = NULL;
}

void mg_art_load(mg_art_t *a, const mesa_game_t *g, const char *board, int set, bool flat, bool portrait)
{
    char want[40];
    base_file(g, want, sizeof want);
    /* the packs stay open from one game to the next of the same set */
    if (a->game != g || a->set != set) {
        mg_art_free(a);
        char p[200];
        if (set != SET_MADERA && mg_art_have_set(set)) {
            pack_path(p, sizeof p, SET_FILE[set]);
            a->open[0] = mg_pak_open(&a->pak[0], p);
        }
        pack_path(p, sizeof p, want);
        a->open[1] = mg_pak_open(&a->pak[1], p);
        if (!a->open[1]) aos_hal_log("mesa", "no pack %s", p);
        a->game = g;
        a->set = set;
    }
    free_pics(a);
    snprintf(a->board, sizeof a->board, "%s", board);
    a->flat = flat;
    char n[MG_NAME_LEN];
    const char *bg = flat ? "fl" : "bg", *geo = flat ? "flgeo" : "geo";
    /* upright, a board may have a picture of its own */
    bool done = false;
    if (portrait && !flat) {
        snprintf(n, sizeof n, "%s_bgp", board);
        if (decode_jpeg(a, n)) {
            snprintf(n, sizeof n, "%s_geop", board);
            done = true;
        }
    }
    if (!done) {
        snprintf(n, sizeof n, "%s_%s", board, bg);
        done = decode_jpeg(a, n);
        snprintf(n, sizeof n, "%s_%s", board, geo);
    }
    a->have_pic = done;
    int pw = a->geo.pw, ph = a->geo.ph;
    char *txt = (char *)load(a, n, NULL);
    parse_geo(txt, &a->geo);
    mg_free(txt);
    a->geo.pw = pw;
    a->geo.ph = ph;
    if (flat) a->geo.E = 90;
    for (int k = 0; k < g->nkinds && k < 32; k++) {
        snprintf(n, sizeof n, "%s_%s%s", board, g->kinds[k].sprite, flat ? "_f" : "");
        a->kind[k].ok = sheet(a, n, &a->kind[k].sheet);
    }
    snprintf(n, sizeof n, "%s_over%s", board, flat ? "_f" : "");
    a->have_over = sheet(a, n, &a->over);
    a->have_dice = sheet(a, flat ? "dado_f" : "dado", &a->dice);
}

bool mg_art_thumb(const mesa_game_t *g, lv_image_dsc_t *d)
{
    char file[40], p[200], n[MG_NAME_LEN];
    base_file(g, file, sizeof file);
    pack_path(p, sizeof p, file);
    mg_pak_t pak;
    if (!mg_pak_open(&pak, p)) return false;
    snprintf(n, sizeof n, "%s_th", g->prefix);
    const mg_entry_t *e = mg_pak_find(&pak, n);
    uint8_t *raw = e && e->type == PK_RGB565 ? (uint8_t *)mg_pak_load(&pak, e) : NULL;
    uint32_t w = e ? e->w : 0, h = e ? e->h : 0, len = e ? e->rawlen : 0;
    mg_pak_close(&pak);
    if (!raw) return false;
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * 2;
    d->data = raw;
    d->data_size = len;
    return true;
}
