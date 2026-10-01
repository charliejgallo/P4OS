/*
 * P4OS - the Macro pad's layout (a JSON document on the card) and the runner
 * that plays a button's action. See aos_macropad.h for the format.
 *
 * The layout: a cJSON tree loaded once and kept (it is a few KB), guarded
 * by a mutex, saved whole on every change. The default one is written in C
 * below and only reaches the card when something changes.
 *
 * The runner: a button becomes a list of steps (a key, one character of a
 * text, a pause, a mouse move, an HA call...) played by an lv_timer every
 * 10 ms, one step per tick. Every key blocks the USB HAL a few ms on the
 * board while the computer takes the press and the release; one per tick
 * keeps the LVGL task breathing through a long text, and lets "stop" stop.
 * The steps are copies: the document can change under a running sequence.
 */
#include "aos_macropad.h"
#include "aos_ha.h"
#include "aos_mqtt.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "macropad"

/* -------------------------------------------------------------------------- */
/* Glyphs                                                                      */
/* -------------------------------------------------------------------------- */

#define G(n) { #n, AOS_SYM_##n }
static const struct { const char *name, *sym; } GLYPHS[] = {
    G(KEYBOARD), G(KEYBOARD_OUTLINE), G(MOUSE), G(CURSOR_DEFAULT_CLICK), G(GESTURE_TAP_BUTTON),
    G(CONTENT_COPY), G(CONTENT_CUT), G(CONTENT_PASTE), G(MONITOR_SCREENSHOT), G(MONITOR),
    G(FULLSCREEN), G(MAGNIFY), G(SWAP_HORIZONTAL), G(SWAP_VERTICAL), G(LOCK), G(LOCK_OPEN_VARIANT),
    G(POWER), G(RESTART), G(UPDATE), G(APPS), G(VIEW_GRID_OUTLINE), G(APPLICATION_BRACKETS),
    G(ROCKET_LAUNCH), G(SCRIPT_TEXT), G(FILE_CODE_OUTLINE), G(FILE_DOCUMENT_OUTLINE), G(FOLDER),
    G(FOLDER_OPEN), G(SEND), G(MESSAGE_TEXT_OUTLINE), G(WEB), G(EARTH), G(DOWNLOAD),
    G(PLAY), G(PAUSE), G(PLAY_PAUSE), G(STOP), G(SKIP_PREVIOUS), G(SKIP_NEXT),
    G(VOLUME_HIGH), G(VOLUME_MEDIUM), G(VOLUME_LOW), G(VOLUME_OFF), G(MICROPHONE), G(MICROPHONE_OFF),
    G(MUSIC), G(RADIO), G(SPEAKER), G(VIDEO), G(VIDEO_OFF), G(CAMERA), G(RECORD_REC),
    G(BRIGHTNESS_5), G(BRIGHTNESS_7), G(HOME_ASSISTANT), G(LIGHTBULB), G(LIGHTBULB_OUTLINE),
    G(LIGHTBULB_GROUP), G(AUTO_FIX), G(TOGGLE_SWITCH), G(POWER_PLUG), G(POWER_PLUG_OFF), G(FAN),
    G(FAN_OFF), G(AIR_CONDITIONER), G(RADIATOR), G(SNOWFLAKE), G(FIRE), G(THERMOMETER),
    G(WATER_PERCENT), G(TELEVISION), G(SOFA), G(DOOR), G(DOOR_OPEN), G(GARAGE), G(GARAGE_OPEN),
    G(BLINDS), G(BLINDS_OPEN), G(WINDOW_SHUTTER), G(ROBOT_VACUUM), G(CCTV), G(MOTION_SENSOR),
    G(BELL), G(BELL_RING), G(ACCESS_POINT), G(LAN), G(WIFI), G(SERVER_NETWORK), G(ROUTER_WIRELESS),
    G(USB), G(CHIP), G(DEVELOPER_BOARD), G(SERIAL_PORT), G(MEMORY), G(LIGHTNING_BOLT), G(FLASH),
    G(GAUGE), G(SINE_WAVE), G(CHART_LINE), G(LED_ON), G(ELECTRIC_SWITCH), G(RESISTOR), G(ENGINE),
    G(ROBOT), G(CAT), G(STAR), G(TIMER_OUTLINE), G(ALARM), G(CALENDAR), G(ARROW_UP_BOLD),
    G(ARROW_DOWN_BOLD), G(ARROW_LEFT_BOLD), G(ARROW_RIGHT_BOLD), G(CHECK), G(CLOSE), G(PLUS),
    G(MINUS), G(PENCIL), G(DELETE), G(COG), G(TROPHY), G(GAMEPAD_VARIANT),
};
#undef G
#define N_GLYPHS ((int)(sizeof GLYPHS / sizeof GLYPHS[0]))

int aos_macropad_glyph_count(void) { return N_GLYPHS; }
const char *aos_macropad_glyph_name(int i) { return i >= 0 && i < N_GLYPHS ? GLYPHS[i].name : ""; }

const char *aos_macropad_glyph(const char *name)
{
    if (name) for (int i = 0; i < N_GLYPHS; i++) if (!strcmp(GLYPHS[i].name, name)) return GLYPHS[i].sym;
    return AOS_SYM_GESTURE_TAP_BUTTON;
}

/* -------------------------------------------------------------------------- */
/* The document                                                                */
/* -------------------------------------------------------------------------- */

static void    *s_mx;
static cJSON   *s_doc;
static uint32_t s_version = 1;
static bool     s_saved;

void aos_macropad_init(void)   { if (!s_mx) s_mx = aos_hal_mutex_create(); }
void aos_macropad_lock(void)   { aos_macropad_init(); aos_hal_mutex_lock(s_mx); }
void aos_macropad_unlock(void) { aos_hal_mutex_unlock(s_mx); }
cJSON *aos_macropad_doc(void)  { return s_doc; }
uint32_t aos_macropad_version(void) { return s_version; }
bool aos_macropad_saved(void) { return s_saved; }

static bool file_path(char *out, size_t n)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    snprintf(out, n, "%s/macropad.json", root);
    return true;
}

static cJSON *btn(const char *label, const char *glyph, uint32_t color, const char *type)
{
    cJSON *b = cJSON_CreateObject();
    char c[8];
    snprintf(c, sizeof c, "#%06X", (unsigned)(color & 0xFFFFFF));
    cJSON_AddStringToObject(b, "label", label);
    cJSON_AddStringToObject(b, "glyph", glyph);
    cJSON_AddStringToObject(b, "color", c);
    cJSON_AddStringToObject(b, "type", type);
    return b;
}

static cJSON *key_btn(const char *label, const char *glyph, uint32_t color, const char *key)
{
    cJSON *b = btn(label, glyph, color, "key");
    cJSON_AddStringToObject(b, "key", key);
    return b;
}

static cJSON *app_btn(const char *label, const char *glyph, uint32_t color, const char *app)
{
    cJSON *b = btn(label, glyph, color, "app");
    cJSON_AddStringToObject(b, "app", app);
    return b;
}

static cJSON *mqtt_btn(const char *label, const char *glyph, uint32_t color, const char *topic,
                       const char *payload, const char *state)
{
    cJSON *b = btn(label, glyph, color, "mqtt");
    cJSON_AddStringToObject(b, "topic", topic);
    cJSON_AddStringToObject(b, "payload", payload);
    if (state) cJSON_AddStringToObject(b, "state", state);
    return b;
}

static cJSON *page_new(cJSON *pages, const char *name)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddArrayToObject(p, "buttons");
    cJSON_AddItemToArray(pages, p);
    return p;
}

/* The default layout: the Mac's everyday shortcuts, the house (filled from
 * HA the first time it answers), and the bench. */
static cJSON *default_doc(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "version", 1);
    cJSON *pages = cJSON_AddArrayToObject(d, "pages");

    cJSON *mac = cJSON_GetObjectItem(page_new(pages, "Mac"), "buttons");
    const uint32_t BLUE = 0x0A84FF, GRAY = 0x3A3A3C, INDIGO = 0x5E5CE6, PINK = 0xFF375F, TEAL = 0x0E7490;
    cJSON_AddItemToArray(mac, key_btn("Copiar", "CONTENT_COPY", BLUE, "cmd+c"));
    cJSON_AddItemToArray(mac, key_btn("Pegar", "CONTENT_PASTE", BLUE, "cmd+v"));
    cJSON_AddItemToArray(mac, key_btn("Cortar", "CONTENT_CUT", BLUE, "cmd+x"));
    cJSON_AddItemToArray(mac, key_btn("Deshacer", "RESTART", GRAY, "cmd+z"));
    cJSON_AddItemToArray(mac, key_btn("Captura", "MONITOR_SCREENSHOT", INDIGO, "cmd+shift+4"));
    cJSON_AddItemToArray(mac, key_btn("Pantalla", "FULLSCREEN", INDIGO, "cmd+shift+3"));
    cJSON_AddItemToArray(mac, key_btn("Spotlight", "MAGNIFY", GRAY, "cmd+space"));
    cJSON_AddItemToArray(mac, key_btn("Cambiar app", "SWAP_HORIZONTAL", GRAY, "cmd+tab"));
    cJSON_AddItemToArray(mac, key_btn("Bloquear", "LOCK", 0xFF453A, "ctrl+cmd+q"));
    cJSON_AddItemToArray(mac, key_btn("Silencio", "VOLUME_OFF", TEAL, "mute"));
    cJSON_AddItemToArray(mac, key_btn("Anterior", "SKIP_PREVIOUS", PINK, "prev"));
    cJSON_AddItemToArray(mac, key_btn("Play", "PLAY_PAUSE", PINK, "play"));
    cJSON_AddItemToArray(mac, key_btn("Siguiente", "SKIP_NEXT", PINK, "next"));
    cJSON_AddItemToArray(mac, key_btn("Volumen -", "VOLUME_LOW", TEAL, "voldown"));
    cJSON_AddItemToArray(mac, key_btn("Volumen +", "VOLUME_HIGH", TEAL, "volup"));

    cJSON *casa = page_new(pages, "Casa");
    cJSON_AddStringToObject(casa, "auto", "ha");

    cJSON *t = cJSON_GetObjectItem(page_new(pages, "Taller"), "buttons");
    const uint32_t AMBER = 0xF59E0B, GREEN = 0x10B981, VIOLET = 0x8B5CF6;
    cJSON_AddItemToArray(t, app_btn("Banco", "SINE_WAVE", 0xFF9F0A, "aos.bench"));
    cJSON_AddItemToArray(t, app_btn("Terminal", "SERIAL_PORT", GRAY, "aos.serial"));
    cJSON_AddItemToArray(t, app_btn("Programador", "CHIP", VIOLET, "aos.flasher"));
    cJSON_AddItemToArray(t, app_btn("Modbus", "LAN", GREEN, "aos.modbus"));
    cJSON_AddItemToArray(t, app_btn("Bus", "DEVELOPER_BOARD", AMBER, "aos.bus"));
    cJSON_AddItemToArray(t, app_btn("MQTT", "ACCESS_POINT", 0x0891B2, "aos.mqtt"));
    /* a Tasmota plug for the soldering station and the fume extractor:
     * TOGGLE on cmnd/, the state comes back on stat/ */
    cJSON_AddItemToArray(t, mqtt_btn("Soldador", "FIRE", 0xFF453A, "cmnd/soldador/POWER", "TOGGLE", "stat/soldador/POWER"));
    cJSON_AddItemToArray(t, mqtt_btn("Extractor", "FAN", 0x30D158, "cmnd/extractor/POWER", "TOGGLE", "stat/extractor/POWER"));
    cJSON_AddItemToArray(t, mqtt_btn("Luz banco", "LED_ON", 0xFFD60A, "cmnd/luzbanco/POWER", "TOGGLE", "stat/luzbanco/POWER"));
    /* a trigger for HA automations: publishes and forgets */
    cJSON_AddItemToArray(t, mqtt_btn("Aviso", "BELL_RING", PINK, "p4os/macropad/aviso", "press", NULL));
    cJSON *s = btn("idf build", "ROCKET_LAUNCH", INDIGO, "seq");
    cJSON_AddStringToObject(s, "seq", "# Terminal en la Mac y a compilar\nKEY cmd+space\nDELAY 300\nSTRING terminal\nKEY enter\nDELAY 900\nSTRING idf.py build\nKEY enter");
    cJSON_AddItemToArray(t, s);
    /* what leaves idf.py monitor */
    cJSON_AddItemToArray(t, key_btn("Salir monitor", "CLOSE", GRAY, "ctrl+]"));
    return d;
}

static void save_locked(void)
{
    char path[160];
    s_saved = false;
    if (!s_doc || !file_path(path, sizeof path)) return;
    char *txt = cJSON_Print(s_doc);
    if (!txt) return;
    char tmp[176];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        size_t n = strlen(txt);
        bool ok = fwrite(txt, 1, n, f) == n;
        ok = fclose(f) == 0 && ok;
        if (ok) {
            remove(path);
            s_saved = rename(tmp, path) == 0;
        }
    }
    if (!s_saved) aos_hal_log(TAG, "could not save %s", path);
    free(txt);
}

/* What a document must look like to be taken: pages, each with a name and
 * a buttons array of objects or nulls, within the limits. Fixes what can be
 * fixed quietly (a missing "buttons"). */
static const char *validate(cJSON *d)
{
    if (!cJSON_IsObject(d)) return "no es un objeto JSON";
    cJSON *pages = cJSON_GetObjectItem(d, "pages");
    if (!cJSON_IsArray(pages)) return "falta \"pages\"";
    int np = cJSON_GetArraySize(pages);
    if (np < 1) return "hace falta al menos una página";
    if (np > AOS_MP_PAGES) return "demasiadas páginas (8 como mucho)";
    cJSON *p;
    cJSON_ArrayForEach(p, pages) {
        if (!cJSON_IsObject(p)) return "una página no es un objeto";
        if (!cJSON_IsString(cJSON_GetObjectItem(p, "name"))) return "una página no tiene nombre";
        cJSON *b = cJSON_GetObjectItem(p, "buttons");
        if (!b) b = cJSON_AddArrayToObject(p, "buttons");
        if (!cJSON_IsArray(b)) return "\"buttons\" no es una lista";
        if (cJSON_GetArraySize(b) > AOS_MP_SLOTS) return "una página tiene más de 15 botones";
        cJSON *x;
        cJSON_ArrayForEach(x, b) if (!cJSON_IsObject(x) && !cJSON_IsNull(x)) return "un botón no es un objeto ni null";
    }
    return NULL;
}

void aos_macropad_load(void)
{
    aos_macropad_lock();
    if (!s_doc) {
        char path[160];
        FILE *f = file_path(path, sizeof path) ? fopen(path, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *txt = n > 0 && n < 256 * 1024 ? malloc((size_t)n + 1) : NULL;
            if (txt && fread(txt, 1, (size_t)n, f) == (size_t)n) {
                txt[n] = 0;
                cJSON *d = cJSON_Parse(txt);
                const char *why = d ? validate(d) : "no se entiende el JSON";
                if (why) {
                    aos_hal_log(TAG, "%s: %s; using the default layout", path, why);
                    cJSON_Delete(d);
                } else {
                    s_doc = d;
                    s_saved = true;
                }
            }
            free(txt);
            fclose(f);
        }
        if (!s_doc) s_doc = default_doc();
        s_version++;
    }
    aos_macropad_unlock();
}

int aos_macropad_page_count(void)
{
    return s_doc ? cJSON_GetArraySize(cJSON_GetObjectItem(s_doc, "pages")) : 0;
}

cJSON *aos_macropad_page(int page)
{
    return s_doc ? cJSON_GetArrayItem(cJSON_GetObjectItem(s_doc, "pages"), page) : NULL;
}

cJSON *aos_macropad_button(int page, int slot)
{
    cJSON *b = cJSON_GetArrayItem(cJSON_GetObjectItem(aos_macropad_page(page), "buttons"), slot);
    return cJSON_IsObject(b) && cJSON_GetObjectItem(b, "type") ? b : NULL;
}

void aos_macropad_set_button(int page, int slot, cJSON *button)
{
    cJSON *arr = cJSON_GetObjectItem(aos_macropad_page(page), "buttons");
    if (!cJSON_IsArray(arr) || slot < 0 || slot >= AOS_MP_SLOTS) { cJSON_Delete(button); return; }
    while (cJSON_GetArraySize(arr) <= slot) cJSON_AddItemToArray(arr, cJSON_CreateNull());
    cJSON_ReplaceItemInArray(arr, slot, button ? button : cJSON_CreateNull());
    /* trailing empties say nothing */
    int n = cJSON_GetArraySize(arr);
    while (n > 0 && cJSON_IsNull(cJSON_GetArrayItem(arr, n - 1))) cJSON_DeleteItemFromArray(arr, --n);
}

void aos_macropad_changed(void)
{
    save_locked();
    s_version++;
}

char *aos_macropad_print(void)
{
    aos_macropad_load();
    aos_macropad_lock();
    char *s = s_doc ? cJSON_Print(s_doc) : NULL;
    aos_macropad_unlock();
    return s;
}

bool aos_macropad_replace(const char *json, char *err, size_t err_len)
{
    cJSON *d = json ? cJSON_Parse(json) : NULL;
    const char *why = d ? validate(d) : "no se entiende el JSON";
    if (why) {
        if (err) snprintf(err, err_len, "%s", why);
        cJSON_Delete(d);
        return false;
    }
    aos_macropad_load();
    aos_macropad_lock();
    cJSON_Delete(s_doc);
    s_doc = d;
    aos_macropad_changed();
    aos_macropad_unlock();
    return true;
}

void aos_macropad_reset(void)
{
    aos_macropad_load();
    aos_macropad_lock();
    cJSON_Delete(s_doc);
    s_doc = default_doc();
    aos_macropad_changed();
    aos_macropad_unlock();
}

/* -------------------------------------------------------------------------- */
/* Buttons                                                                     */
/* -------------------------------------------------------------------------- */

static const char *const TYPE_NAMES[] = { "", "key", "text", "seq", "ha", "mqtt", "app" };

aos_mp_type_t aos_macropad_type(const cJSON *b)
{
    const cJSON *t = cJSON_GetObjectItem(b, "type");
    if (cJSON_IsString(t))
        for (int i = 1; i < (int)(sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]); i++)
            if (!strcmp(t->valuestring, TYPE_NAMES[i])) return (aos_mp_type_t)i;
    return AOS_MP_NONE;
}

const char *aos_macropad_type_name(aos_mp_type_t t)
{
    return (unsigned)t < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0] ? TYPE_NAMES[t] : "";
}

const char *aos_macropad_str(const cJSON *b, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(b, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

uint32_t aos_macropad_color(const cJSON *b)
{
    const char *c = aos_macropad_str(b, "color");
    if (*c == '#') c++;
    if (strlen(c) != 6) return 0x3A3A3C;
    char *end;
    unsigned long v = strtoul(c, &end, 16);
    return *end ? 0x3A3A3C : (uint32_t)v;
}

const char *aos_macropad_check(const cJSON *b)
{
    switch (aos_macropad_type(b)) {
    case AOS_MP_KEY:
        if (!*aos_macropad_str(b, "key")) return _("Falta la combinación de teclas");
        if (!aos_hal_usb_key_valid(aos_macropad_str(b, "key"))) return _("No conozco esa tecla");
        return NULL;
    case AOS_MP_TEXT: return *aos_macropad_str(b, "text") ? NULL : _("Falta el texto");
    case AOS_MP_SEQ:  return *aos_macropad_str(b, "seq") ? NULL : _("La secuencia está vacía");
    case AOS_MP_HA:   return strchr(aos_macropad_str(b, "entity"), '.') ? NULL : _("Falta la entidad de Home Assistant");
    case AOS_MP_MQTT: {
        const char *t = aos_macropad_str(b, "topic");
        if (!*t) return _("Falta el tópico");
        if (strpbrk(t, "+#")) return _("El tópico no puede tener + ni #");
        return NULL;
    }
    case AOS_MP_APP: return aos_ui_app_find(aos_macropad_str(b, "app")) ? NULL : _("Esa app no está");
    default: return _("Botón sin acción");
    }
}

/* -------------------------------------------------------------------------- */
/* Filling "Casa" from Home Assistant                                          */
/* -------------------------------------------------------------------------- */

static bool autofill_page(cJSON *page)
{
    cJSON *arr = cJSON_GetObjectItem(page, "buttons");
    if (!cJSON_IsArray(arr)) return false;
    /* scenes first (a tap does a lot), then lights, then the rest */
    static const struct { uint8_t domain; const char *glyph; uint32_t color; } ORDER[] = {
        { HA_SCENE, "AUTO_FIX", 0xBF5AF2 }, { HA_LIGHT, "LIGHTBULB", 0xFFD60A },
        { HA_SWITCH, "TOGGLE_SWITCH", 0x30D158 }, { HA_FAN, "FAN", 0x40C8E0 },
        { HA_COVER, "BLINDS", 0x0A84FF }, { HA_SCRIPT, "SCRIPT_TEXT", 0x5E5CE6 },
        { HA_INPUT_BOOLEAN, "TOGGLE_SWITCH", 0x8E8E93 }, { HA_LOCK, "LOCK", 0xFF9F0A },
    };
    int n = 0;
    aos_ha_lock();
    for (size_t o = 0; o < sizeof ORDER / sizeof ORDER[0] && n < AOS_MP_SLOTS; o++) {
        int per = 0;
        for (int i = 0; i < aos_ha_count() && n < AOS_MP_SLOTS; i++) {
            const aos_ha_entity_t *e = aos_ha_at(i);
            if (!e || e->domain != ORDER[o].domain) continue;
            if (ORDER[o].domain == HA_SCENE && per >= 6) break;     /* leave room for lights */
            cJSON *b = btn(e->name[0] ? e->name : e->id, ORDER[o].glyph, ORDER[o].color, "ha");
            cJSON_AddStringToObject(b, "entity", e->id);
            cJSON_AddItemToArray(arr, b);
            n++;
            per++;
        }
    }
    aos_ha_unlock();
    return n > 0;
}

bool aos_macropad_autofill(void)
{
    if (aos_ha_state() != AOS_HA_READY) return false;
    bool changed = false;
    aos_macropad_lock();
    for (int i = 0; i < aos_macropad_page_count(); i++) {
        cJSON *p = aos_macropad_page(i);
        if (strcmp(aos_macropad_str(p, "auto"), "ha")) continue;
        cJSON_DeleteItemFromObject(p, "auto");      /* once: after that the page is the user's */
        if (cJSON_GetArraySize(cJSON_GetObjectItem(p, "buttons")) == 0) autofill_page(p);
        changed = true;
    }
    if (changed) aos_macropad_changed();
    aos_macropad_unlock();
    return changed;
}

/* -------------------------------------------------------------------------- */
/* The runner                                                                  */
/* -------------------------------------------------------------------------- */

typedef enum { ST_STRING, ST_KEY, ST_DELAY, ST_MOUSE, ST_SCROLL, ST_CLICK, ST_HA, ST_MQTT, ST_OPEN } step_kind_t;

typedef struct {
    uint8_t kind;
    int     a, b;
    char   *s1, *s2, *s3;       /* malloc'd: text / key / topic / entity; payload / service; data */
} step_t;

#define MAX_STEPS  600
#define TICK_MS    10
#define MOUSE_CHUNK 120

static struct {
    step_t     *steps;
    int         n, cur, char_pos, total;
    bool        waiting;
    uint32_t    wait_until;
    lv_timer_t *timer;
    int         typed, skipped;     /* characters sent and not typeable, of this run */
    char        what[64];           /* the button's name, for the status */
    int         page, slot;
} R;

static aos_mp_status_t s_st = { .page = -1, .slot = -1 };

void aos_macropad_say(bool ok, const char *msg)
{
    R.page = R.slot = -1;
    snprintf(s_st.msg, sizeof s_st.msg, "%s", msg);
    s_st.ok = ok;
    s_st.page = s_st.slot = -1;
    s_st.seq++;
}

void aos_macropad_status(aos_mp_status_t *out)
{
    *out = s_st;
    out->busy = R.timer != NULL;
    out->pct = R.total ? R.cur * 100 / R.total : 0;
}

static void say(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_st.msg, sizeof s_st.msg, fmt, ap);
    va_end(ap);
    s_st.ok = ok;
    s_st.page = R.page;
    s_st.slot = R.slot;
    s_st.seq++;
}

static char *dup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void steps_free(void)
{
    for (int i = 0; i < R.n; i++) { free(R.steps[i].s1); free(R.steps[i].s2); free(R.steps[i].s3); }
    R.n = R.cur = R.total = 0;
}

static step_t *add(step_kind_t k, const char *s1, const char *s2, const char *s3, int a, int b)
{
    if (!R.steps) R.steps = calloc(MAX_STEPS, sizeof *R.steps);    /* PSRAM on the board */
    if (!R.steps || R.n >= MAX_STEPS) return NULL;
    step_t *st = &R.steps[R.n++];
    *st = (step_t){ .kind = (uint8_t)k, .a = a, .b = b, .s1 = dup(s1), .s2 = dup(s2), .s3 = dup(s3) };
    R.total++;
    return st;
}

static void add_mouse(int dx, int dy)
{
    do {
        int cx = dx > MOUSE_CHUNK ? MOUSE_CHUNK : dx < -MOUSE_CHUNK ? -MOUSE_CHUNK : dx;
        int cy = dy > MOUSE_CHUNK ? MOUSE_CHUNK : dy < -MOUSE_CHUNK ? -MOUSE_CHUNK : dy;
        if (!add(ST_MOUSE, NULL, NULL, NULL, cx, cy)) return;
        dx -= cx;
        dy -= cy;
    } while (dx || dy);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* "HA light.taller" / "HA light.toggle light.taller {json}" */
static void add_ha(char *arg)
{
    char *save = NULL, *a = strtok_r(arg, " \t", &save), *b = strtok_r(NULL, " \t", &save);
    char *rest = save ? trim(save) : NULL;
    if (!a) return;
    if (b && strchr(b, '.')) add(ST_HA, b, a, rest && *rest ? rest : NULL, 0, 0);
    else add(ST_HA, a, NULL, NULL, 0, 0);
}

/* Pato goma's language (aos_macropad.h), line by line; an unknown line is
 * skipped, so a typo loses a line and never the script. */
static void parse_seq(const char *script)
{
    char *text = dup(script);
    if (!text) return;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *arg = s;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        char verb[12];
        size_t vl = (size_t)(arg - s);
        if (vl >= sizeof verb) continue;
        for (size_t i = 0; i < vl; i++) verb[i] = (char)toupper((unsigned char)s[i]);
        verb[vl] = 0;
        arg = trim(arg);
        if (!strcmp(verb, "STRING") || !strcmp(verb, "TYPE")) { if (*arg) add(ST_STRING, arg, NULL, NULL, 0, 0); }
        else if (!strcmp(verb, "KEY") || !strcmp(verb, "PRESS")) { if (*arg) add(ST_KEY, arg, NULL, NULL, 0, 0); }
        else if (!strcmp(verb, "DELAY") || !strcmp(verb, "WAIT")) {
            int ms = atoi(arg);
            add(ST_DELAY, NULL, NULL, NULL, ms < 0 ? 0 : ms > 60000 ? 60000 : ms, 0);
        } else if (!strcmp(verb, "MOUSE") || !strcmp(verb, "MOVE")) {
            int dx = 0, dy = 0;
            sscanf(arg, "%d %d", &dx, &dy);
            add_mouse(dx, dy);
        } else if (!strcmp(verb, "SCROLL")) {
            int n = atoi(arg);
            add(ST_SCROLL, NULL, NULL, NULL, n > 127 ? 127 : n < -127 ? -127 : n, 0);
        } else if (!strcmp(verb, "CLICK")) add(ST_CLICK, NULL, NULL, NULL, atoi(arg) == 2 ? 2 : 1, 0);
        else if (!strcmp(verb, "REPEAT")) {
            int n = atoi(arg);
            if (R.n == 0 || n <= 0) continue;
            step_t src = R.steps[R.n - 1];
            for (int i = 0; i < n; i++) if (!add((step_kind_t)src.kind, src.s1, src.s2, src.s3, src.a, src.b)) break;
        } else if (!strcmp(verb, "HA")) add_ha(arg);
        else if (!strcmp(verb, "MQTT")) {
            char *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            char *payload = *sp ? trim(sp + 1) : sp;
            *sp = 0;
            if (*arg) add(ST_MQTT, arg, payload, NULL, 0, 0);
        } else if (!strcmp(verb, "OPEN")) { if (*arg) add(ST_OPEN, arg, NULL, NULL, 0, 0); }
        if (R.n >= MAX_STEPS) break;
    }
    free(text);
}

static bool needs_usb(void)
{
    for (int i = R.cur; i < R.n; i++) {
        uint8_t k = R.steps[i].kind;
        if (k != ST_DELAY && k != ST_HA && k != ST_MQTT && k != ST_OPEN) return true;
    }
    return false;
}

/* Why the keyboard is not there, for the status line. */
static void say_no_usb(void)
{
    if (aos_hal_usb_busy()) say(false, "%s", _("El USB se está preparando: probá de nuevo en un segundo"));
    else say(false, "%s", _("Sin computadora: conectá el puerto OTG a la computadora"));
}

static bool do_ha(const step_t *st)
{
    bool ok;
    if (st->s2 && *st->s2) {
        char dom[32];
        const char *dot = strchr(st->s2, '.');
        if (!dot || dot - st->s2 >= (long)sizeof dom) { say(false, _("Servicio inválido: %s"), st->s2); return false; }
        memcpy(dom, st->s2, (size_t)(dot - st->s2));
        dom[dot - st->s2] = 0;
        ok = aos_ha_call(dom, dot + 1, st->s1, st->s3 ? st->s3 : "{}");
    } else {
        ok = aos_ha_tap(st->s1);
    }
    aos_hal_log(TAG, "ha %s %s: %s", st->s2 ? st->s2 : "tap", st->s1, ok ? "queued" : "refused");
    if (!ok) {
        say(false, "%s", aos_ha_state() == AOS_HA_UNCONFIGURED ? _("Home Assistant no está configurado")
                         : aos_ha_state() == AOS_HA_READY ? _("Home Assistant no aceptó la llamada")
                         : _("Home Assistant no está conectado"));
        return false;
    }
    if (st->s2 && *st->s2) say(true, "Home Assistant: %s %s", st->s2, st->s1);
    else say(true, "Home Assistant: %s", st->s1);
    return true;
}

static bool do_mqtt(const step_t *st, int qos, bool retain)
{
    bool ok = aos_mqtt_publish(st->s1, st->s2 ? st->s2 : "", qos, retain);
    aos_hal_log(TAG, "mqtt %s <- %s: %s", st->s1, st->s2 ? st->s2 : "", ok ? "queued" : "refused");
    if (ok) say(true, "MQTT: %s \xE2\x86\x90 %s", st->s1, st->s2 ? st->s2 : "");
    else say(false, "%s", aos_mqtt_state() == AOS_MQTT_UNCONFIGURED || aos_mqtt_state() == AOS_MQTT_OFF
                          ? _("MQTT no está configurado") : _("MQTT no pudo publicar"));
    return ok;
}

static int s_mqtt_qos;
static bool s_mqtt_retain;

static void finish(void)
{
    if (R.timer) { lv_timer_delete(R.timer); R.timer = NULL; }
    steps_free();
}

/* One step per call. false: stop here (the reason is in the status). */
static bool step_once(void)
{
    step_t *st = &R.steps[R.cur];
    bool advance = true;
    bool hid = st->kind != ST_DELAY && st->kind != ST_HA && st->kind != ST_MQTT && st->kind != ST_OPEN;
    if (hid && !aos_hal_usb_keys_ready()) { say_no_usb(); return false; }
    switch (st->kind) {
    case ST_STRING: {
        int len = (int)strlen(st->s1);
        if (R.char_pos < len) {
            char one[2] = { st->s1[R.char_pos], 0 };
            /* 0 with the keyboard still there: a character a US layout
             * does not have (an accent, a ñ), skipped */
            if (aos_hal_usb_type(one) == 1) R.typed++;
            else if (aos_hal_usb_keys_ready()) R.skipped++;
            else { say_no_usb(); return false; }
            R.char_pos++;
        }
        advance = R.char_pos >= len;
        if (advance) aos_hal_log(TAG, "typed \"%s\"", st->s1);
        break;
    }
    case ST_KEY:
        if (!aos_hal_usb_key_valid(st->s1)) { say(false, _("No conozco la tecla «%s»"), st->s1); return false; }
        if (!aos_hal_usb_key(st->s1)) { say_no_usb(); return false; }
        aos_hal_log(TAG, "key %s", st->s1);
        say(true, _("Enviado: %s"), st->s1);
        break;
    case ST_DELAY:
        if (!R.waiting) {
            R.waiting = true;
            R.wait_until = (uint32_t)aos_hal_uptime_ms() + (uint32_t)st->a;
            advance = false;
        } else if ((int32_t)((uint32_t)aos_hal_uptime_ms() - R.wait_until) >= 0) R.waiting = false;
        else advance = false;
        break;
    case ST_MOUSE:  aos_hal_usb_mouse(st->a, st->b, 0); break;
    case ST_SCROLL: aos_hal_usb_mouse(0, 0, st->a); break;
    case ST_CLICK:  aos_hal_usb_click(st->a); aos_hal_log(TAG, "click %d", st->a); break;
    case ST_HA:     if (!do_ha(st)) return false; break;
    case ST_MQTT:   if (!do_mqtt(st, s_mqtt_qos, s_mqtt_retain)) return false; break;
    case ST_OPEN:
        if (!aos_ui_app_find(st->s1)) { say(false, _("No encuentro la app %s"), st->s1); return false; }
        say(true, _("Abriendo %s"), st->s1);
        R.cur++;                        /* before: opening may hide us and come back */
        aos_ui_open(st->s1);
        return true;
    }
    if (advance) { R.cur++; R.char_pos = 0; }
    return true;
}

static void run_done(void)
{
    bool one_text = R.total == 1 && R.n == 1 && R.steps[0].kind == ST_STRING;
    if (R.skipped) say(true, _("Escrito: %d caracteres (%d no existen en un teclado US)"), R.typed, R.skipped);
    else if (one_text && R.page >= 0) say(true, _("Escrito: «%.60s»"), R.steps[0].s1);
    else if (R.page < 0 && R.typed) say(true, _("Escrito: %d caracteres"), R.typed);
    else if (R.total > 1 && R.what[0]) say(true, _("Listo: %s"), R.what);
    /* otherwise the step's own message stands: "Enviado: cmd+c" */
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    /* a few quick steps per tick; a key or a character is one tick */
    for (int budget = 0; budget < 8 && R.cur < R.n; budget++) {
        uint8_t k = R.steps[R.cur].kind;
        if (!step_once()) { finish(); return; }
        if (!R.timer) return;                       /* stopped from inside (an app opened) */
        if (k == ST_STRING || k == ST_KEY || k == ST_CLICK || k == ST_DELAY) break;
    }
    if (R.cur >= R.n) { run_done(); finish(); }
}

/* The port becomes the keyboard when something needs it; on the P4 that
 * costs nothing (the console is on the UART), so it is simply asked for. */
static bool usb_ready(void)
{
    if (aos_hal_usb_keys_ready()) return true;
    if (aos_hal_usb_mode() == AOS_HAL_USB_CONSOLE) aos_hal_usb_mode_set(AOS_HAL_USB_KEYS);
    return aos_hal_usb_keys_ready();
}

static void start(void)
{
    if (!R.n) return;
    if (needs_usb() && !usb_ready()) {
        say_no_usb();
        finish();
        return;
    }
    if (!R.timer) R.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    tick_cb(R.timer);                               /* the first step at once: the finger is still on it */
}

static void begin_run(const char *what, int page, int slot)
{
    finish();
    R.typed = R.skipped = 0;
    R.page = page;
    R.slot = slot;
    snprintf(R.what, sizeof R.what, "%s", what ? what : "");
}

static bool run_json(const cJSON *b, int page, int slot)
{
    begin_run(aos_macropad_str(b, "label"), page, slot);
    const char *why = aos_macropad_check(b);
    if (why) { say(false, "%s", why); return false; }
    s_mqtt_qos = 0;
    s_mqtt_retain = false;
    switch (aos_macropad_type(b)) {
    case AOS_MP_KEY:  add(ST_KEY, aos_macropad_str(b, "key"), NULL, NULL, 0, 0); break;
    case AOS_MP_TEXT: add(ST_STRING, aos_macropad_str(b, "text"), NULL, NULL, 0, 0); break;
    case AOS_MP_SEQ:
        parse_seq(aos_macropad_str(b, "seq"));
        if (!R.n) { say(false, "%s", _("La secuencia no tiene pasos que entienda")); return false; }
        break;
    case AOS_MP_HA: {
        const char *svc = aos_macropad_str(b, "service"), *data = aos_macropad_str(b, "data");
        add(ST_HA, aos_macropad_str(b, "entity"), *svc ? svc : NULL, *data ? data : NULL, 0, 0);
        break;
    }
    case AOS_MP_MQTT: {
        const cJSON *q = cJSON_GetObjectItem(b, "qos");
        s_mqtt_qos = cJSON_IsNumber(q) && q->valueint == 1 ? 1 : 0;
        s_mqtt_retain = cJSON_IsTrue(cJSON_GetObjectItem(b, "retain"));
        add(ST_MQTT, aos_macropad_str(b, "topic"), aos_macropad_str(b, "payload"), NULL, 0, 0);
        break;
    }
    case AOS_MP_APP: add(ST_OPEN, aos_macropad_str(b, "app"), NULL, NULL, 0, 0); break;
    default: return false;
    }
    start();
    return true;
}

bool aos_macropad_run_button(const cJSON *b)
{
    return b && run_json(b, -1, -1);
}

bool aos_macropad_run(int page, int slot)
{
    aos_macropad_load();
    aos_macropad_lock();
    cJSON *b = aos_macropad_button(page, slot);
    cJSON *copy = b ? cJSON_Duplicate(b, true) : NULL;
    aos_macropad_unlock();
    if (!copy) return false;
    bool ok = run_json(copy, page, slot);
    cJSON_Delete(copy);
    return ok;
}

/* the queue of the trackpad page: appended to whatever is playing */
static void queue_start(void)
{
    if (!usb_ready()) {
        say_no_usb();
        steps_free();
        return;
    }
    if (!R.timer) {
        R.page = R.slot = -1;
        R.what[0] = 0;
        R.typed = R.skipped = 0;
        R.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    }
}

static void compact(void)
{
    /* drop the steps already played, so a long typing session never fills the array */
    if (R.cur == 0) return;
    for (int i = 0; i < R.cur; i++) { free(R.steps[i].s1); free(R.steps[i].s2); free(R.steps[i].s3); }
    memmove(R.steps, R.steps + R.cur, (size_t)(R.n - R.cur) * sizeof *R.steps);
    R.n -= R.cur;
    R.total -= R.cur;
    R.cur = 0;
}

void aos_macropad_queue_key(const char *name)
{
    if (!R.timer) steps_free();
    else compact();
    add(ST_KEY, name, NULL, NULL, 0, 0);
    queue_start();
}

void aos_macropad_queue_text(const char *text)
{
    if (!text || !*text) return;
    if (!R.timer) steps_free();
    else compact();
    add(ST_STRING, text, NULL, NULL, 0, 0);
    queue_start();
}

void aos_macropad_stop(void)
{
    if (!R.timer) return;
    finish();
    say(false, "%s", _("Detenido"));
}

static void run_req_cb(void *arg)
{
    intptr_t v = (intptr_t)arg;
    aos_macropad_run((int)(v >> 8), (int)(v & 0xFF));
}

void aos_macropad_request_run(int page, int slot)
{
    aos_ui_request_call(run_req_cb, (void *)(intptr_t)((page << 8) | (slot & 0xFF)));
}
