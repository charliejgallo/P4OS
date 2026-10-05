/*
 * P4OS - Infrarrojo: the devices on the card.
 *
 * One JSON file per device in /sdcard/ir, readable and editable by hand and
 * by the portal's page:
 *
 *   {
 *     "name": "Tele del living",
 *     "kind": "tv",                         tv audio ac fan light other
 *     "buttons": [
 *       { "name": "Encender", "role": "power", "proto": "NEC", "addr": 4, "cmd": 8 },
 *       { "name": "HDMI 1", "freq": 38000, "raw": [9000, 4500, 560, 1690, ...] }
 *     ]
 *   }
 *
 * A button is a code of a known protocol (proto, addr, cmd, and "vendor" for
 * Kaseikyo) or raw durations in microseconds, mark first, with the carrier.
 * An air conditioner from SmartIR's library keeps no codes, only which one
 * it is and the state the app last sent:
 *
 *   { "name": "Aire", "kind": "ac", "smartir": 1000,
 *     "state": { "on": true, "mode": "cool", "levels": ["auto"], "temp": 24 } }
 */
#include "ir.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif

/* ------------------------------------------------------------------ memory */

void *ir_alloc(size_t n)
{
    if (!n) n = 1;
#if defined(AOS_SIM)
    return calloc(1, n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, n);
    return p;
#endif
}

void *ir_realloc(void *p, size_t n)
{
    if (!n) n = 1;
#if defined(AOS_SIM)
    return realloc(p, n);
#else
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

void ir_free(void *p)
{
#if defined(AOS_SIM)
    free(p);
#else
    if (p) heap_caps_free(p);
#endif
}

char *ir_strdup(const char *s)
{
    size_t n = strlen(s ? s : "");
    char *d = ir_alloc(n + 1);
    if (d && s) memcpy(d, s, n);
    return d;
}

void ir_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    snprintf(dst, n, "%s", src ? src : "");
    /* not in the middle of a UTF-8 character */
    size_t l = strlen(dst);
    if (src && strlen(src) > l) {
        while (l > 0 && ((unsigned char)dst[l - 1] & 0xC0) == 0x80) l--;
        if (l > 0 && ((unsigned char)dst[l - 1] & 0xC0) == 0xC0) l--;
        dst[l] = 0;
    }
}

static bool sb_room(ir_sb_t *b, int more)
{
    if (b->n + more + 1 <= b->cap) return true;
    int cap = b->cap ? b->cap : 256;
    while (cap < b->n + more + 1) cap *= 2;
    char *s = ir_realloc(b->s, (size_t)cap);
    if (!s) return false;
    b->s = s;
    b->cap = cap;
    return true;
}

void ir_sb_put(ir_sb_t *b, const char *s, int n)
{
    if (n < 0) n = (int)strlen(s);
    if (!sb_room(b, n)) return;
    memcpy(b->s + b->n, s, (size_t)n);
    b->n += n;
    b->s[b->n] = 0;
}

void ir_sb_printf(ir_sb_t *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n < (int)sizeof tmp) { ir_sb_put(b, tmp, n); return; }
    if (!sb_room(b, n)) return;
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += n;
}

void ir_sb_json(ir_sb_t *b, const char *s)
{
    ir_sb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; ir_sb_put(b, e, 2); }
        else if (*p == '\n') ir_sb_put(b, "\\n", 2);
        else if (*p < 0x20) ir_sb_printf(b, "\\u%04x", *p);
        else ir_sb_put(b, (const char *)p, 1);
    }
    ir_sb_put(b, "\"", 1);
}

void ir_sb_free(ir_sb_t *b)
{
    ir_free(b->s);
    memset(b, 0, sizeof *b);
}

/* ------------------------------------------------------------ kinds, roles */

static const struct { const char *key, *label, *glyph; } KINDS[IR_K_COUNT] = {
    { "tv",    N_("Tele"),       AOS_SYM_TELEVISION },
    { "audio", N_("Audio"),      AOS_SYM_SPEAKER },
    { "ac",    N_("Aire"),       AOS_SYM_AIR_CONDITIONER },
    { "fan",   N_("Ventilador"), AOS_SYM_FAN },
    { "light", N_("Luz"),        AOS_SYM_LIGHTBULB },
    { "other", N_("Otro"),       AOS_SYM_LED_ON },
};

int         ir_kind_count(void) { return IR_K_COUNT; }
const char *ir_kind_key(int k) { return KINDS[k >= 0 && k < IR_K_COUNT ? k : IR_K_OTHER].key; }
const char *ir_kind_label(int k) { return _(KINDS[k >= 0 && k < IR_K_COUNT ? k : IR_K_OTHER].label); }
const char *ir_kind_glyph(int k) { return KINDS[k >= 0 && k < IR_K_COUNT ? k : IR_K_OTHER].glyph; }

int ir_kind_from(const char *key)
{
    for (int k = 0; k < IR_K_COUNT; k++) if (key && !strcmp(KINDS[k].key, key)) return k;
    return IR_K_OTHER;
}

static const ir_role_t ROLES[] = {
    { "",          N_("Otro botón"),  NULL },
    { "power",     N_("Encender/apagar"), AOS_SYM_POWER },
    { "power_on",  N_("Encender"),    NULL },
    { "power_off", N_("Apagar"),      NULL },
    { "input",     N_("Fuente"),      NULL },
    { "mute",      N_("Silencio"),    AOS_SYM_VOLUME_OFF },
    { "vol_up",    N_("Volumen +"),   AOS_SYM_PLUS },
    { "vol_down",  N_("Volumen −"),   AOS_SYM_MINUS },
    { "ch_up",     N_("Canal +"),     AOS_SYM_CHEVRON_UP },
    { "ch_down",   N_("Canal −"),     AOS_SYM_CHEVRON_DOWN },
    { "up",        N_("Arriba"),      AOS_SYM_CHEVRON_UP },
    { "down",      N_("Abajo"),       AOS_SYM_CHEVRON_DOWN },
    { "left",      N_("Izquierda"),   AOS_SYM_CHEVRON_LEFT },
    { "right",     N_("Derecha"),     AOS_SYM_CHEVRON_RIGHT },
    { "ok",        N_("OK"),          NULL },
    { "back",      N_("Volver"),      NULL },
    { "home",      N_("Inicio"),      NULL },
    { "menu",      N_("Menú"),        NULL },
    { "info",      N_("Info"),        AOS_SYM_INFORMATION_OUTLINE },
    { "guide",     N_("Guía"),        NULL },
    { "play",      N_("Reproducir"),  AOS_SYM_PLAY },
    { "pause",     N_("Pausa"),       AOS_SYM_PAUSE },
    { "play_pause", N_("Reproducir/pausa"), AOS_SYM_PLAY_PAUSE },
    { "stop",      N_("Detener"),     AOS_SYM_STOP },
    { "rew",       N_("Retroceder"),  "\xC2\xAB" },
    { "ffwd",      N_("Adelantar"),   "\xC2\xBB" },
    { "prev",      N_("Anterior"),    AOS_SYM_SKIP_PREVIOUS },
    { "next",      N_("Siguiente"),   AOS_SYM_SKIP_NEXT },
    { "rec",       N_("Grabar"),      AOS_SYM_RECORD_REC },
    { "red",       N_("Rojo"),        NULL },
    { "green",     N_("Verde"),       NULL },
    { "yellow",    N_("Amarillo"),    NULL },
    { "blue",      N_("Azul"),        NULL },
    { "d1", "1", NULL }, { "d2", "2", NULL }, { "d3", "3", NULL },
    { "d4", "4", NULL }, { "d5", "5", NULL }, { "d6", "6", NULL },
    { "d7", "7", NULL }, { "d8", "8", NULL }, { "d9", "9", NULL },
    { "d0", "0", NULL },
};
#define NROLES ((int)(sizeof ROLES / sizeof ROLES[0]))

int              ir_role_count(void) { return NROLES; }
const ir_role_t *ir_role_at(int i) { return (i >= 0 && i < NROLES) ? &ROLES[i] : NULL; }

const ir_role_t *ir_role_find(const char *role)
{
    for (int i = 0; i < NROLES; i++) if (role && !strcmp(ROLES[i].role, role)) return &ROLES[i];
    return &ROLES[0];
}

/* ----------------------------------------------------------------- buttons */

void ir_button_clear(ir_button_t *b)
{
    ir_free(b->raw);
    b->raw = NULL;
    b->nraw = 0;
}

void ir_button_set_raw(ir_button_t *b, const uint32_t *d, int n, uint32_t freq)
{
    ir_button_clear(b);
    memset(&b->code, 0, sizeof b->code);
    b->freq = freq;
    if (n <= 0) return;
    b->raw = ir_alloc((size_t)n * sizeof *d);
    if (!b->raw) return;
    memcpy(b->raw, d, (size_t)n * sizeof *d);
    b->nraw = n;
}

int ir_button_pulses(const ir_button_t *b, bool repeat, uint32_t *out, int max, uint32_t *carrier)
{
    const ir_proto_t *p = ir_proto_find(b->code.proto);
    if (!p) {
        *carrier = b->freq ? b->freq : 38000;
        int n = b->nraw < max ? b->nraw : max;
        memcpy(out, b->raw, (size_t)n * sizeof *out);
        return n;
    }
    /* the toggle of RC5 and RC6 flips on every new press */
    static uint8_t toggle;
    if (!repeat) toggle ^= 1;
    *carrier = p->carrier;
    int frames = repeat ? 1 : p->frames, n = 0, start = 0;
    for (int f = 0; f < frames; f++) {
        if (n) {
            /* the gap that keeps the protocol's period, start to start */
            uint32_t len = ir_total_us(out + start, n - start), period = (uint32_t)p->period_ms * 1000;
            if (n >= max) break;
            out[n++] = period > len + 8000 ? period - len : 8000;
        }
        start = n;
        int k = ir_encode(&b->code, repeat || (f > 0 && !strcmp(p->name, "JVC")), toggle, out + n, max - n);
        if (!k) return 0;
        n += k;
    }
    return n;
}

/* -------------------------------------------------------------------- JSON */

void ir_dev_json(const ir_dev_t *d, ir_sb_t *sb)
{
    ir_sb_put(sb, "{\n  \"name\": ", -1);
    ir_sb_json(sb, d->name);
    ir_sb_printf(sb, ",\n  \"kind\": \"%s\"", ir_kind_key(d->kind));
    if (d->smartir) {
        ir_sb_printf(sb, ",\n  \"smartir\": %d,\n  \"state\": { \"on\": %s, \"mode\": ", d->smartir,
                     d->ac_on ? "true" : "false");
        ir_sb_json(sb, d->ac_mode);
        ir_sb_put(sb, ", \"levels\": [", -1);
        int last = IR_AC_LEVELS - 1;
        while (last >= 0 && !d->ac_sel[last][0]) last--;
        for (int i = 0; i <= last; i++) {
            if (i) ir_sb_put(sb, ", ", 2);
            ir_sb_json(sb, d->ac_sel[i]);
        }
        /* one decimal, by hand */
        int tt = (int)(d->ac_temp * 10 + 0.5f);
        if (tt % 10) ir_sb_printf(sb, "], \"temp\": %d.%d }", tt / 10, tt % 10);
        else ir_sb_printf(sb, "], \"temp\": %d }", tt / 10);
    }
    ir_sb_put(sb, ",\n  \"buttons\": [", -1);
    for (int i = 0; i < d->nbtn; i++) {
        const ir_button_t *b = &d->btn[i];
        ir_sb_put(sb, i ? ",\n    { \"name\": " : "\n    { \"name\": ", -1);
        ir_sb_json(sb, b->name);
        if (b->role[0]) { ir_sb_put(sb, ", \"role\": ", -1); ir_sb_json(sb, b->role); }
        if (ir_proto_find(b->code.proto)) {
            ir_sb_printf(sb, ", \"proto\": \"%s\", \"addr\": %u, \"cmd\": %u", b->code.proto,
                         (unsigned)b->code.addr, (unsigned)b->code.cmd);
            if (!strcmp(b->code.proto, "Kaseikyo")) ir_sb_printf(sb, ", \"vendor\": %u", (unsigned)b->code.extra);
        } else {
            ir_sb_printf(sb, ", \"freq\": %u, \"raw\": [", (unsigned)(b->freq ? b->freq : 38000));
            for (int k = 0; k < b->nraw; k++) ir_sb_printf(sb, k ? ",%u" : "%u", (unsigned)b->raw[k]);
            ir_sb_put(sb, "]", 1);
        }
        ir_sb_put(sb, " }", 2);
    }
    ir_sb_put(sb, d->nbtn ? "\n  ]\n}\n" : "]\n}\n", -1);
}

ir_button_t *ir_dev_add_button(ir_dev_t *d)
{
    ir_button_t *b = ir_realloc(d->btn, (size_t)(d->nbtn + 1) * sizeof *b);
    if (!b) return NULL;
    d->btn = b;
    memset(&b[d->nbtn], 0, sizeof *b);
    return &b[d->nbtn++];
}

void ir_dev_remove_button(ir_dev_t *d, int i)
{
    if (i < 0 || i >= d->nbtn) return;
    ir_button_clear(&d->btn[i]);
    memmove(&d->btn[i], &d->btn[i + 1], (size_t)(d->nbtn - i - 1) * sizeof *d->btn);
    d->nbtn--;
}

bool ir_dev_from_json(ir_dev_t *d, const ij_doc_t *j)
{
    if (!j->len || j->n[0].type != IJ_OBJ) return false;
    ir_copy(d->name, sizeof d->name, ij_str(j, ij_get(j, 0, "name"), "?"));
    d->kind = ir_kind_from(ij_str(j, ij_get(j, 0, "kind"), "other"));
    d->smartir = (int)ij_num(j, ij_get(j, 0, "smartir"), 0);
    int st = ij_get(j, 0, "state");
    if (st >= 0) {
        d->ac_on = ij_num(j, ij_get(j, st, "on"), 0) != 0;
        ir_copy(d->ac_mode, sizeof d->ac_mode, ij_str(j, ij_get(j, st, "mode"), ""));
        d->ac_temp = (float)ij_num(j, ij_get(j, st, "temp"), 24);
        int lv = ij_get(j, st, "levels"), k = 0;
        IJ_EACH(j, lv, c) {
            if (k < IR_AC_LEVELS) ir_copy(d->ac_sel[k++], sizeof d->ac_sel[0], ij_str(j, c, ""));
        }
    }
    int bs = ij_get(j, 0, "buttons");
    IJ_EACH(j, bs, c) {
        ir_button_t *b = ir_dev_add_button(d);
        if (!b) return false;
        ir_copy(b->name, sizeof b->name, ij_str(j, ij_get(j, c, "name"), "?"));
        ir_copy(b->role, sizeof b->role, ij_str(j, ij_get(j, c, "role"), ""));
        const ir_proto_t *p = ir_proto_find(ij_str(j, ij_get(j, c, "proto"), ""));
        if (p) {
            ir_copy(b->code.proto, sizeof b->code.proto, p->name);
            b->code.addr = (uint32_t)ij_num(j, ij_get(j, c, "addr"), 0);
            b->code.cmd = (uint32_t)ij_num(j, ij_get(j, c, "cmd"), 0);
            b->code.extra = (uint32_t)ij_num(j, ij_get(j, c, "vendor"), 0);
            continue;
        }
        b->freq = (uint32_t)ij_num(j, ij_get(j, c, "freq"), 38000);
        int raw = ij_get(j, c, "raw");
        if (raw >= 0 && j->n[raw].count > 0) {
            b->raw = ir_alloc((size_t)j->n[raw].count * sizeof *b->raw);
            if (!b->raw) return false;
            IJ_EACH(j, raw, r) {
                double v = ij_num(j, r, 0);
                if (v < 0) v = -v;      /* ESPHome writes spaces negative */
                if (v >= 1 && b->nraw < IR_TX_MAX) b->raw[b->nraw++] = (uint32_t)v;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------- files */

const char *ir_store_dir(void)
{
    static char dir[96];
    if (!dir[0]) {
        const char *root = aos_hal_path_sd_root();
        snprintf(dir, sizeof dir, "%s/ir", root ? root : "/sdcard");
    }
    return dir;
}

void ir_dev_clear(ir_dev_t *d)
{
    for (int i = 0; i < d->nbtn; i++) ir_button_clear(&d->btn[i]);
    ir_free(d->btn);
    memset(d, 0, sizeof *d);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n > 0 && n < 4 * 1024 * 1024) ? ir_alloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { ir_free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

bool ir_dev_load(const char *file, ir_dev_t *d)
{
    char path[320];
    snprintf(path, sizeof path, "%s/%s", ir_store_dir(), file);
    size_t len = 0;
    char *text = read_file(path, &len);
    if (!text) return false;
    ij_doc_t j;
    bool ok = ij_parse(&j, text, len);
    ir_free(text);
    if (!ok) { aos_hal_log("ir", "%s: not JSON", file); return false; }
    memset(d, 0, sizeof *d);
    ok = ir_dev_from_json(d, &j);
    ij_free(&j);
    ir_copy(d->file, sizeof d->file, file);
    if (!ok) ir_dev_clear(d);
    return ok;
}

static void slug(const char *name, char *out, size_t n)
{
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && k + 1 < n; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[k++] = (char)c;
        else if (c == 0xC3 && p[1]) {
            /* the accented vowels and the eñe of Latin-1 */
            unsigned char x = *++p;
            const char *map = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
            char m = (x >= 0xA0 && x < 0xC0) ? map[x - 0xA0] : (x >= 0x80 && x < 0xA0) ? map[x - 0x80] : '-';
            out[k++] = (m >= 'a' && m <= 'z') ? m : '-';
        } else if (k && out[k - 1] != '-') out[k++] = '-';
    }
    while (k && out[k - 1] == '-') k--;
    out[k] = 0;
    if (!k) snprintf(out, n, "aparato");
}

bool ir_dev_save(ir_dev_t *d)
{
    mkdir(ir_store_dir(), 0777);
    if (!d->file[0]) {
        char base[32], path[320];
        slug(d->name, base, sizeof base);
        struct stat st;
        for (int i = 1; i < 100; i++) {
            if (i == 1) snprintf(d->file, sizeof d->file, "%s.json", base);
            else snprintf(d->file, sizeof d->file, "%s-%d.json", base, i);
            snprintf(path, sizeof path, "%s/%s", ir_store_dir(), d->file);
            if (stat(path, &st) != 0) break;
        }
    }
    char path[320], tmp[328];
    snprintf(path, sizeof path, "%s/%s", ir_store_dir(), d->file);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    ir_sb_t sb = { 0 };
    ir_dev_json(d, &sb);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && sb.s && fwrite(sb.s, 1, (size_t)sb.n, f) == (size_t)sb.n;
    if (f) ok = (fclose(f) == 0) && ok;
    ir_sb_free(&sb);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) { remove(tmp); aos_hal_log("ir", "could not write %s", path); }
    return ok;
}

bool ir_dev_delete(const ir_dev_t *d)
{
    char path[320];
    snprintf(path, sizeof path, "%s/%s", ir_store_dir(), d->file);
    return remove(path) == 0;
}

static bool json_name(const char *n)
{
    size_t l = strlen(n);
    return n[0] != '.' && l > 5 && !strcmp(n + l - 5, ".json");
}

uint32_t ir_store_signature(void)
{
    DIR *dir = opendir(ir_store_dir());
    if (!dir) return 0;
    uint32_t sig = 2166136261u;
    struct dirent *e;
    char path[320];
    while ((e = readdir(dir))) {
        if (!json_name(e->d_name)) continue;
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", ir_store_dir(), e->d_name);
        if (stat(path, &st) != 0) continue;
        uint32_t h = 2166136261u;
        for (const char *p = e->d_name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        /* order-free: a sum of the files' own hashes */
        sig += h ^ (uint32_t)st.st_mtime ^ (uint32_t)st.st_size * 2654435761u;
    }
    closedir(dir);
    return sig;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const ir_dev_t *)a)->name, ((const ir_dev_t *)b)->name);
}

void ir_store_free_all(ir_dev_t *devs, int n)
{
    for (int i = 0; i < n; i++) ir_dev_clear(&devs[i]);
    ir_free(devs);
}

int ir_store_scan(ir_dev_t **out)
{
    *out = NULL;
    DIR *dir = opendir(ir_store_dir());
    if (!dir) return 0;
    ir_dev_t *devs = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!json_name(e->d_name)) continue;
        ir_dev_t *grown = ir_realloc(devs, (size_t)(n + 1) * sizeof *devs);
        if (!grown) break;
        devs = grown;
        if (ir_dev_load(e->d_name, &devs[n])) n++;
    }
    closedir(dir);
    if (n > 1) qsort(devs, (size_t)n, sizeof *devs, by_name);
    *out = devs;
    return n;
}
