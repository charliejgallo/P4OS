/*
 * Tortuga - a game that teaches programming the way LOGO did: a turtle, a
 * board and its goal, and the program that takes it there, written with
 * blocks, in the game's own Logo, or in Lua (DESIGN.md). This file is the
 * life cycle: the icon, the preferences, the progress, the screens.
 *
 *     tools/build_apps.sh tortuga          (and the pack: tools/pack_p4.py)
 */
#include "tg_app.h"

#include "aos_fonts.h"
#include "aos_icon_ops.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

tg_app_t *tg;

/* A turtle seen from above: four feet, the head with an eye, the shell
 * with a plate in the middle, on grass. */
static const uint8_t TG_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -24, -20, 17, 17, AIC_CIRCLE, AIC_C_LIT(0x9FDC7C), 255),
    AIC_RECT(AIC_CENTER, -24,  20, 17, 17, AIC_CIRCLE, AIC_C_LIT(0x9FDC7C), 255),
    AIC_RECT(AIC_CENTER,  14, -22, 17, 17, AIC_CIRCLE, AIC_C_LIT(0x9FDC7C), 255),
    AIC_RECT(AIC_CENTER,  14,  22, 17, 17, AIC_CIRCLE, AIC_C_LIT(0x9FDC7C), 255),
    AIC_RECT(AIC_CENTER,  30,   0, 24, 22, AIC_CIRCLE, AIC_C_LIT(0x9FDC7C), 255),
    AIC_RECT(AIC_CENTER,  36,  -5,  6,  6, AIC_CIRCLE, AIC_C_LIT(0x1D1A22), 255),
    AIC_RECT(AIC_CENTER,  -5,   0, 56, 50, AIC_CIRCLE, AIC_C_LIT(0xC7772A), 255),
    AIC_BORDER(4, AIC_C_LIT(0x8A4F24), 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER,   0,   0, 22, 20, AIC_CIRCLE, AIC_C_LIT(0xF2C27A), 255),
    AIC_OUT,
    AIC_RECT(AIC_CENTER,  25,  -1,  9,  4, 2, AIC_C_LIT(0xE8443A), 255),
    AIC_END
};

/* ------------------------------------------------------------ helpers */

lv_obj_t *tg_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

lv_obj_t *tg_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_ver(b, 12, 0);
    if (text) {
        lv_obj_t *l = tg_label(b, text, aos_font_body, AOS_C_TEXT);
        lv_obj_center(l);
    }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *tg_stars_draw(lv_obj_t *parent, int n, int size)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, size / 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++) {
        tg_label(row, AOS_SYM_STAR, size >= 36 ? &aos_sym_44 : &aos_sym_28,
                 i < n ? AOS_C_YELLOW : lv_color_hex(0x48484A));
    }
    return row;
}

lv_color_t tg_lang_color(int lang)
{
    static const uint32_t C[TG_LANG_N] = { 0x34C759, 0xFF9F0A, 0x0A84FF, 0xBF5AF2 };
    return lv_color_hex(C[lang < 0 || lang >= TG_LANG_N ? 0 : lang]);
}

const char *tg_lang_name(int lang)
{
    switch (lang) {
    case TG_LANG_BLOCKS: return _("Bloques");
    case TG_LANG_LOGO: return "Logo";
    case TG_LANG_LUA: return "Lua";
    default: return "Python";
    }
}

bool tg_lang_available(int lang)
{
    return lang != TG_LANG_PY;      /* Python comes as a module of its own, later */
}

/* ------------------------------------------------------------ worlds */

static const char *pack_world(int world, bool idea)
{
    for (int i = 0; i < tg->levels.nwh; i++) {
        if (tg->levels.wh[i].world != world) continue;
        const char *t = tg_level_text(idea ? tg->levels.wh[i].idea : tg->levels.wh[i].title, tg->tlang);
        if (t && t[0]) return t;
    }
    return NULL;
}

int tg_world_max(void)
{
    int m = TG_NWORLD;
    for (int i = 0; i < tg->levels.n; i++) {
        if (tg->levels.lv[i].world > m) m = tg->levels.lv[i].world;
    }
    return m;
}

const char *tg_world_name(int world)
{
    if (world > TG_NWORLD) {
        const char *t = pack_world(world, false);
        static char buf[32];
        if (t) return t;
        snprintf(buf, sizeof buf, _("Mundo %d"), world);
        return buf;
    }
    static const char *const N[TG_NWORLD + 1] = {
        "", N_("Primeros pasos"), N_("Otra vez"), N_("Ponele nombre"), N_("Preguntar"), N_("Hasta que"),
        N_("Contar"), N_("A medida"), N_("El lápiz"), N_("La gran travesía"),
    };
    return world >= 1 && world <= TG_NWORLD ? _(N[world]) : "";
}

const char *tg_world_idea(int world)
{
    if (world > TG_NWORLD) {
        const char *t = pack_world(world, true);
        return t ? t : "";
    }
    static const char *const N[TG_NWORLD + 1] = {
        "", N_("Una orden detrás de otra"), N_("Repetir"), N_("Procedimientos"), N_("Si, sino y sensores"),
        N_("Mientras"), N_("Variables"), N_("Parámetros"), N_("Dibujar como en LOGO"), N_("Todo junto"),
    };
    return world >= 1 && world <= TG_NWORLD ? _(N[world]) : "";
}

int tg_world_first(int world)
{
    for (int i = 0; i < tg->levels.n; i++) {
        if (tg->levels.lv[i].world == world) return i;
    }
    return -1;
}

int tg_world_count(int world)
{
    int n = 0;
    for (int i = 0; i < tg->levels.n; i++) n += tg->levels.lv[i].world == world;
    return n;
}

static int best(const tg_score_t *s, int lang)
{
    if (lang >= 0) return s->stars[lang];
    int b = 0;
    for (int k = 0; k < TG_LANG_N; k++) {
        if (s->stars[k] > b) b = s->stars[k];
    }
    return b;
}

int tg_world_stars(int world, int lang)
{
    int n = 0;
    for (int i = 0; i < tg->levels.n; i++) {
        if (tg->levels.lv[i].world == world) n += best(&tg->score[i], lang);
    }
    return n;
}

int tg_total_stars(void)
{
    int n = 0;
    for (int i = 0; i < tg->levels.n; i++) n += best(&tg->score[i], -1);
    return n;
}

bool tg_world_open(int world)
{
    /* a world opens with half the stars of the one before, in any language,
     * and the first two are always open (the tutorial) */
    if (world <= 2 || world > TG_NWORLD) return true;     /* a pack's worlds are a gift */
    int prev = world - 1;
    int have = tg_world_stars(prev, -1);
    int need = tg_world_count(prev) * 3 / 2;
    return have >= need && tg_world_open(prev);
}

/* ------------------------------------------------------- preferences */

static void load_prefs(void)
{
    int32_t v;
    tg_prefs_t *p = &tg->prefs;
    p->lang = TG_LANG_BLOCKS;
    p->speed = 2;
    p->sound = 1;
    if (aos_hal_pref_get_i32("tg_lang", &v) && v >= 0 && v < TG_LANG_N && tg_lang_available(v)) {
        p->lang = (uint8_t)v;
        p->chosen = 1;
    }
    if (aos_hal_pref_get_i32("tg_hard", &v)) p->hard = v != 0;
    if (aos_hal_pref_get_i32("tg_speed", &v) && v >= 0 && v <= 4) p->speed = (uint8_t)v;
    if (aos_hal_pref_get_i32("tg_sound", &v)) p->sound = v != 0;
}

void tg_save_prefs(void)
{
    tg_prefs_t *p = &tg->prefs;
    if (p->chosen) aos_hal_pref_set_i32("tg_lang", p->lang);
    aos_hal_pref_set_i32("tg_hard", p->hard);
    aos_hal_pref_set_i32("tg_speed", p->speed);
    aos_hal_pref_set_i32("tg_sound", p->sound);
}

/* ---------------------------------------------------------- progress */

static void data_dir(char *out, size_t n, const char *sub)
{
    snprintf(out, n, "%s/tortuga%s%s", aos_hal_path_data(), sub ? "/" : "", sub ? sub : "");
}

static void ensure_dirs(void)
{
    char p[200];
    data_dir(p, sizeof p, NULL);
    mkdir(p, 0775);
    data_dir(p, sizeof p, "prog");
    mkdir(p, 0775);
}

static void load_scores(void)
{
    char p[200];
    data_dir(p, sizeof p, "scores.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        char id[16];
        int s[TG_LANG_N] = { 0 }, medal = 0;
        if (sscanf(line, "%15s %d %d %d %d %d", id, &s[0], &s[1], &s[2], &s[3], &medal) < 5) continue;
        for (int i = 0; i < tg->levels.n; i++) {
            if (strcmp(tg->levels.lv[i].id, id)) continue;
            for (int k = 0; k < TG_LANG_N; k++) tg->score[i].stars[k] = (uint8_t)(s[k] < 0 ? 0 : s[k] > 3 ? 3 : s[k]);
            tg->score[i].medal = (uint8_t)medal;
        }
    }
    fclose(f);
}

void tg_save_scores(void)
{
    char p[200], tmp[210];
    ensure_dirs();
    data_dir(p, sizeof p, "scores.txt");
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < tg->levels.n; i++) {
        const tg_score_t *s = &tg->score[i];
        if (!best(s, -1)) continue;
        fprintf(f, "%s %d %d %d %d %d\n", tg->levels.lv[i].id, s->stars[0], s->stars[1], s->stars[2], s->stars[3],
                s->medal);
    }
    fclose(f);
    remove(p);
    rename(tmp, p);
}

void tg_program_path(char *out, size_t n, const tg_level_t *lv, int lang)
{
    static const char *const EXT[TG_LANG_N] = { "blk", "logo", "lua", "py" };
    char dir[200];
    data_dir(dir, sizeof dir, "prog");
    snprintf(out, n, "%s/%s.%s", dir, lv ? lv->id : "libre", EXT[lang]);
}

char *tg_program_load(const tg_level_t *lv, int lang)
{
    char p[240];
    tg_program_path(p, sizeof p, lv, lang);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 32768) {
        fclose(f);
        return NULL;
    }
    char *b = tg_malloc((size_t)n + 1);
    if (b) b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}

void tg_program_save(const tg_level_t *lv, int lang, const char *text)
{
    char p[240];
    ensure_dirs();
    tg_program_path(p, sizeof p, lv, lang);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* ------------------------------------------------- the levels, and more */

/* The game's levels, then every world the card brings: loose .tl files in
 * apps/tortuga/, and the text of packs named tortuga_<something>_p4.pak
 * (entries "levels..."), which is how the Store can add worlds or stories
 * later without a new app. A pack's levels use worlds 10 and up and ids
 * of their own. */
typedef struct {
    char *buf;
    size_t len, cap;
} text_t;

static void text_add(text_t *t, const char *s, size_t n)
{
    if (t->len + n + 2 > t->cap) {
        size_t cap = (t->len + n + 2) * 2;
        char *b = tg_alloc(cap);
        if (!b) return;
        if (t->buf) memcpy(b, t->buf, t->len);
        tg_free(t->buf);
        t->buf = b;
        t->cap = cap;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len++] = '\n';
    t->buf[t->len] = 0;
}

static bool ends_with(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcmp(s + a - b, end);
}

static bool scan_tl(const aos_dir_entry_t *e, void *ctx)
{
    text_t *t = ctx;
    if (e->dir || e->name[0] == '.' || !ends_with(e->name, ".tl") || e->size > 256 * 1024) return true;
    char p[320];
    snprintf(p, sizeof p, "%s/tortuga/%s", aos_hal_path_apps(), e->name);
    FILE *f = fopen(p, "rb");
    if (!f) return true;
    char *b = tg_alloc(e->size + 1);
    if (b) {
        size_t n = fread(b, 1, e->size, f);
        text_add(t, b, n);
        tg_free(b);
        aos_hal_log("tortuga", "levels from %s", e->name);
    }
    fclose(f);
    return true;
}

static bool scan_pak(const aos_dir_entry_t *e, void *ctx)
{
    text_t *t = ctx;
    if (e->dir || e->name[0] == '.' || strncmp(e->name, "tortuga_", 8) || !ends_with(e->name, "_p4.pak") ||
        !strcmp(e->name, TG_PACK)) {
        return true;
    }
    char p[320];
    snprintf(p, sizeof p, "%s/%s", aos_hal_path_apps(), e->name);
    tg_pak_t pak;
    if (!tg_pak_open(&pak, p)) return true;
    for (int i = 0; i < pak.n; i++) {
        const tg_entry_t *en = &pak.e[i];
        if (en->type != PK_TEXT || strncmp(en->name, "levels", 6)) continue;
        char *b = tg_pak_load(&pak, en);
        if (b) {
            text_add(t, b, en->rawlen);
            tg_free(b);
            aos_hal_log("tortuga", "levels from %s", e->name);
        }
    }
    tg_pak_close(&pak);
    return true;
}

static bool load_levels(void)
{
    text_t t = { 0 };
    text_add(&t, tg_levels_text, tg_levels_len);
    char p[200];
    snprintf(p, sizeof p, "%s/tortuga", aos_hal_path_apps());
    aos_hal_dir_scan(p, scan_tl, &t);
    aos_hal_dir_scan(aos_hal_path_apps(), scan_pak, &t);
    bool ok = t.buf && tg_levels_parse(&tg->levels, t.buf, t.len);
    tg_free(t.buf);
    return ok;
}

/* --------------------------------------------------------------- art */

static void load_art(void)
{
    static const char *const NAME[SH_N] = { "tg_ground", "tg_rock", "tg_lettuce", "tg_key", "tg_door", "tg_goal",
                                            "tg_bridge", "tg_walk", "tg_bump", "tg_eat", "tg_party" };
    char p[160];
    snprintf(p, sizeof p, "%s/%s", aos_hal_path_apps(), TG_PACK);
    if (!tg_pak_open(&tg->pak, p)) {
        aos_hal_log("tortuga", "no pack at %s", p);
        return;
    }
    int ok = 0;
    for (int i = 0; i < SH_N; i++) {
        const tg_entry_t *e = tg_pak_find(&tg->pak, NAME[i]);
        if (!e || e->type != PK_SHEET) continue;
        tg->sheet_data[i] = (uint8_t *)tg_pak_load(&tg->pak, e);
        if (tg->sheet_data[i] && tg_sheet_bind(&tg->sheet[i], tg->sheet_data[i], e->rawlen)) ok++;
    }
    tg_pak_close(&tg->pak);
    tg->have_pak = ok == SH_N;
    aos_hal_log("tortuga", "%d of %d sheets, %u KB", ok, SH_N, (unsigned)(tg_mem_used() / 1024));
}

static void free_art(void)
{
    for (int i = 0; i < SH_N; i++) {
        tg_free(tg->sheet_data[i]);
        tg->sheet_data[i] = NULL;
    }
    tg->have_pak = false;
}

/* ------------------------------------------------------------ screens */

void tg_go(int screen)
{
    /* the level's objects live in the screen about to go: close it first,
     * also when the same level opens again (the screen turned) */
    if (tg->screen == SCR_PLAY) tg_play_close();
    tg->screen = screen;
    if (tg->scr) lv_obj_delete(tg->scr);
    tg->scr = lv_obj_create(tg->root);
    lv_obj_remove_style_all(tg->scr);
    lv_obj_set_size(tg->scr, tg->W, tg->H);
    lv_obj_set_style_bg_color(tg->scr, lv_color_hex(0x101318), 0);
    lv_obj_set_style_bg_opa(tg->scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tg->scr, LV_OBJ_FLAG_SCROLLABLE);
    switch (screen) {
    case SCR_HOME: tg_menu_home(tg->scr); break;
    case SCR_PICK: tg_menu_pick(tg->scr); break;
    case SCR_WORLDS: tg_menu_worlds(tg->scr); break;
    case SCR_LEVELS: tg_menu_levels(tg->scr); break;
    case SCR_PLAY: tg_play_open(tg->scr); break;
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!tg || tg->closing) return;
    aos_pad_update(&tg->pad, lv_tick_get());
    if (tg->screen == SCR_PLAY) {
        tg_play_frame();
        if (tg->pad.connected) tg_play_pad();
    } else {
        tg_menu_tick();
        if (tg->pad.connected) tg_menu_pad();
    }
    tg_audio_pump();
}

static int text_lang(void)
{
    /* Spanish keeps its own words; German has its texts; any other language
     * (en, pt, fr, it...) plays with the English keywords and level texts, as
     * German does with the keywords: programming is written in English
     * everywhere, and the catalogues of those languages say so */
    const char *c = aos_i18n_current();
    if (!c || !strncmp(c, "es", 2)) return TG_ES;
    if (!strncmp(c, "de", 2)) return TG_DE;
    return TG_EN;
}

static void *tg_create(aos_app_t *self, lv_obj_t *root)
{
    tg = tg_malloc(sizeof(tg_app_t));
    if (!tg) return NULL;
    tg->self = self;
    tg->root = root;
    lv_obj_update_layout(root);
    tg->W = lv_obj_get_width(root);
    tg->H = lv_obj_get_height(root);
    tg->land = tg->W > tg->H;
    tg->tlang = text_lang();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101318), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    if (!load_levels()) {
        tg_free(tg);
        tg = NULL;
        return NULL;
    }
    tg->score = tg_malloc(sizeof(tg_score_t) * (size_t)(tg->levels.n ? tg->levels.n : 1));
    load_prefs();
    load_scores();
    load_art();
    tg_audio_open();
    tg->level = -1;
    tg->world = 1;
    tg_go(tg->prefs.chosen ? SCR_HOME : SCR_PICK);
    tg->timer = lv_timer_create(timer_cb, 33, NULL);
    aos_hal_log("tortuga", "%d levels, %s", tg->levels.n, tg->have_pak ? "with art" : "no art");
    return tg;
}

static void tg_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!tg) return;
    tg->closing = true;
    if (tg->timer) lv_timer_delete(tg->timer);
    if (tg->screen == SCR_PLAY) tg_play_close();
    tg_save_prefs();
    tg_audio_close();
    free_art();
    tg_free(tg->score);
    tg_levels_free(&tg->levels);
    tg_free(tg);
    tg = NULL;
}

static bool tg_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!tg) return false;
    if (tg->screen == SCR_PLAY) return tg_play_back();
    return tg_menu_back();
}

static bool tg_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    if (!tg) return false;
    tg->root = root;
    lv_obj_update_layout(tg->root);
    tg->W = lv_obj_get_width(tg->root);
    tg->H = lv_obj_get_height(tg->root);
    tg->land = tg->W > tg->H;
    tg_go(tg->screen);
    return true;
}

static bool tg_init(aos_app_t *app)
{
    app->desc.id       = "demo.tortuga";
    app->desc.name     = "Tortuga";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x5CAE3E;
    app->desc.color_b  = 0x2E7D32;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_KEEP;
    app->desc.order    = 200;
    aos_icon_set_ops(app, TG_ICON, sizeof TG_ICON);

    app->create  = tg_create;
    app->destroy = tg_destroy;
    app->back    = tg_back;
    app->resize  = tg_resize;
    return true;
}

AOS_APP_ENTRY(tg_init);
