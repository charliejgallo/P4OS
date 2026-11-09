/*
 * P4OS - Luces: the portal's page is a second console (docs/PORTAL-PAGES.md,
 * "Live data"). A phone on the same network does what the screen does.
 *
 * The page sends lines of key=value (one or several per message):
 *
 *   salida=1|0   puerto=uart.a   blackout=1|0   master=0..255
 *   elegir=0,3,4          the selected fixtures (by place); "elegir=" none
 *   dim=0..255   color=r,g,b[,w]   strobe=0..255   pos=pan,tilt (0..65535)
 *   efecto=0..3  velocidad=1..100
 *   escena=i     fundido=ms   guardar=name   regrabar=i   borrar=i
 *   renombrar=i,name
 *   secuencia=1|0   bpm=n   tap=1   corte=0..100 (the chase's fade, %)
 *   agregar=type,n         n fixtures of that type at the next free addresses
 *   aparato=i,addr,type,name   change one     quitar=i
 *   tipos=1               read the card's types again
 *
 * and reads one key while it looks, "estado", a JSON with all of it and the
 * 512 channels as hex, four times a second.
 */
#include "lz_ui.h"

#include "aos_hal.h"
#include "aos_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_APP "aos.luces"

/* ---- a small JSON writer into a fixed buffer ---- */

typedef struct { char *b; size_t n, k; } jw_t;

static void jw_raw(jw_t *w, const char *s)
{
    size_t l = strlen(s);
    if (w->k + l + 1 >= w->n) return;
    memcpy(w->b + w->k, s, l);
    w->k += l;
    w->b[w->k] = 0;
}

static void jw_fmt(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jw_fmt(jw_t *w, const char *fmt, ...)
{
    if (w->k + 2 >= w->n) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(w->b + w->k, w->n - w->k, fmt, ap);
    va_end(ap);
    if (r > 0) w->k = w->k + (size_t)r < w->n ? w->k + (size_t)r : w->n - 1;
}

static void jw_str(jw_t *w, const char *s)
{
    if (w->k + 3 >= w->n) return;
    w->b[w->k++] = '"';
    for (; *s && w->k + 8 < w->n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { w->b[w->k++] = '\\'; w->b[w->k++] = (char)c; }
        else if (c < 0x20) w->k += (size_t)snprintf(w->b + w->k, w->n - w->k, "\\u%04x", c);
        else w->b[w->k++] = (char)c;
    }
    w->b[w->k++] = '"';
    w->b[w->k] = 0;
}

/* ---- the orders ---- */

static bool flag(const char *v) { return v[0] == '1' || !strcmp(v, "on") || !strcmp(v, "si"); }

static int clampi(long v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

/* up to n numbers separated by commas; how many there were */
static int numbers(const char *v, long *out, int n)
{
    int k = 0;
    const char *p = v;
    char *end;
    while (k < n && *p) {
        long x = strtol(p, &end, 0);
        if (end == p) break;
        out[k++] = x;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return k;
}

static void patch_changed(void)
{
    lz_patch_save();
    lz_scenes_save();
    LZ.changed++;
}

static void order(const char *key, char *v)
{
    long n[4];
    if (!strcmp(key, "salida")) { LZU.want_open = flag(v); LZU.err[0] = 0; }
    else if (!strcmp(key, "puerto")) { if (v[0]) lz_set_port(v); }
    else if (!strcmp(key, "blackout")) LZ.blackout = flag(v);
    else if (!strcmp(key, "master")) LZ.master = (uint8_t)clampi(atol(v), 0, 255);
    else if (!strcmp(key, "elegir")) {
        memset(LZ.sel, 0, sizeof LZ.sel);
        for (char *p = v, *next; p && *p; p = next) {
            next = strchr(p, ',');
            if (next) *next++ = 0;
            int i = atoi(p);
            if (i >= 0 && i < LZ.nfix) LZ.sel[i] = true;
        }
    } else if (!strcmp(key, "dim")) lz_set_dim((uint8_t)clampi(atol(v), 0, 255));
    else if (!strcmp(key, "color")) {
        int k = numbers(v, n, 4);
        if (k >= 3) lz_set_rgb((uint8_t)clampi(n[0], 0, 255), (uint8_t)clampi(n[1], 0, 255), (uint8_t)clampi(n[2], 0, 255), k > 3 ? (uint8_t)clampi(n[3], 0, 255) : 0);
    } else if (!strcmp(key, "strobe")) lz_set_strobe((uint8_t)clampi(atol(v), 0, 255));
    else if (!strcmp(key, "pos")) {
        if (numbers(v, n, 2) == 2) lz_set_pos((uint16_t)clampi(n[0], 0, 65535), (uint16_t)clampi(n[1], 0, 65535));
    } else if (!strcmp(key, "efecto")) lz_effect(clampi(atol(v), 0, LZ_FX_COUNT - 1));
    else if (!strcmp(key, "velocidad")) LZ.fx_speed = (uint8_t)clampi(atol(v), 1, 100);
    else if (!strcmp(key, "escena")) lz_scene_recall(atoi(v), LZ.scene_fade_ms);
    else if (!strcmp(key, "fundido")) LZ.scene_fade_ms = (uint16_t)clampi(atol(v), 0, 30000);
    else if (!strcmp(key, "guardar")) lz_scene_store(v);
    else if (!strcmp(key, "regrabar")) {
        int i = atoi(v);
        if (LZ.scenes && i >= 0 && i < LZ.nscenes) {
            memcpy(LZ.scenes[i].look, LZ.look, sizeof LZ.look);
            LZ.cur_scene = i;
            lz_scenes_save();
        }
    } else if (!strcmp(key, "borrar")) {
        lz_scene_delete(atoi(v));
        if (!LZ.nscenes) LZ.chase = false;
    } else if (!strcmp(key, "renombrar")) {
        char *c = strchr(v, ',');
        int i = atoi(v);
        if (c && c[1] && LZ.scenes && i >= 0 && i < LZ.nscenes) {
            snprintf(LZ.scenes[i].name, sizeof LZ.scenes[i].name, "%.23s", c + 1);
            lz_scenes_save();
            LZ.changed++;
        }
    } else if (!strcmp(key, "secuencia")) lz_chase_set(flag(v));
    else if (!strcmp(key, "bpm")) LZ.bpm = (uint16_t)clampi(atol(v), 20, 300);
    else if (!strcmp(key, "tap")) lz_tap();
    else if (!strcmp(key, "corte")) LZ.chase_fade = (uint8_t)clampi(atol(v), 0, 100);
    else if (!strcmp(key, "agregar")) {
        if (numbers(v, n, 2) == 2 && n[0] >= 0 && n[0] < LZ.ntypes) {
            const lz_type_t *t = &LZ.types[n[0]];
            char base[24];
            snprintf(base, sizeof base, "%.20s", t->name);
            char *p = strstr(base, " (");
            if (p) *p = 0;
            int have = 0;
            for (int i = 0; i < LZ.nfix; i++) have += LZ.fix[i].type == n[0];
            for (int k = 0; k < clampi(n[1], 1, LZ_FIX); k++) {
                int a = lz_next_free(t->n);
                char nm[24];
                snprintf(nm, sizeof nm, "%.18s %u", base, (unsigned)(uint8_t)(have + k + 1));
                if (a < 0 || lz_fix_add((int)n[0], (uint16_t)a, nm) < 0) break;
            }
            patch_changed();
        }
    } else if (!strcmp(key, "aparato")) {
        /* i,addr,type,name: the name is the rest, commas and all */
        char *p = v;
        long f[3];
        int k = 0;
        for (; k < 3 && p; k++) {
            f[k] = strtol(p, NULL, 0);
            p = strchr(p, ',');
            if (p) p++;
        }
        if (k == 3 && f[0] >= 0 && f[0] < LZ.nfix) {
            lz_fix_t *x = &LZ.fix[f[0]];
            if (f[1] >= 1 && f[1] <= 512) x->addr = (uint16_t)f[1];
            if (f[2] >= 0 && f[2] < LZ.ntypes) x->type = (int)f[2];
            if (p && *p) snprintf(x->name, sizeof x->name, "%.23s", p);
            patch_changed();
        }
    } else if (!strcmp(key, "quitar")) {
        lz_fix_remove(atoi(v));
        patch_changed();
    } else if (!strcmp(key, "tipos")) lz_types_reload();
}

static void take_orders(void)
{
    char msg[1100];
    int n, guard = 0;
    bool any = false;
    while (guard++ < 16 && (n = aos_hal_live_take(LIVE_APP, msg, sizeof msg - 1)) > 0) {
        msg[n] = 0;
        for (char *line = msg, *next; line && *line; line = next) {      /* strtok is not exported */
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            order(line, eq + 1);
            any = true;
        }
    }
    if (!any) return;
    lz_save();
    lz_bar_refresh();
}

/* ---- the state ---- */

static void put_state(void)
{
    static char buf[24576];
    jw_t w = { buf, sizeof buf, 0 };
    jw_fmt(&w, "{\"salida\":{\"quiere\":%s,\"abierta\":%s,\"puerto\":", LZU.want_open ? "true" : "false", LZU.h ? "true" : "false");
    jw_str(&w, LZU.port);
    jw_fmt(&w, ",\"hz\":%.1f,\"paquetes\":%lu,\"error\":", (double)LZU.st.hz, (unsigned long)LZU.st.packets);
    jw_str(&w, LZU.err);
    jw_raw(&w, ",\"puertos\":[");
    bool first = true;
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind != AOS_PORT_UART) continue;
        if (!first) jw_raw(&w, ",");
        jw_str(&w, p->name);
        first = false;
    }
    jw_fmt(&w, "]},\"blackout\":%s,\"master\":%u,\"efecto\":%d,\"velocidad\":%u,\"escena\":%d,\"fundido\":%u,"
               "\"secuencia\":%s,\"bpm\":%u,\"corte\":%u,\"cambios\":%lu",
           LZ.blackout ? "true" : "false", LZ.master, LZ.fx, LZ.fx_speed, LZ.cur_scene, LZ.scene_fade_ms,
           LZ.chase ? "true" : "false", LZ.bpm, LZ.chase_fade, (unsigned long)LZ.changed);
    jw_raw(&w, ",\"aparatos\":[");
    for (int i = 0; i < LZ.nfix; i++) {
        const lz_fix_t *f = &LZ.fix[i];
        const lz_look_t *l = &LZ.look[i];
        uint8_t r, g, b;
        lz_shown_color(i, &r, &g, &b);
        jw_raw(&w, i ? ",{\"n\":" : "{\"n\":");
        jw_str(&w, f->name);
        jw_fmt(&w, ",\"t\":%d,\"a\":%u,\"sel\":%s,\"pisa\":%s,\"ve\":\"#%02x%02x%02x\",\"dim\":%u,\"rgb\":[%u,%u,%u,%u],\"strobe\":%u,\"pan\":%u,\"tilt\":%u}",
               f->type, f->addr, LZ.sel[i] ? "true" : "false", lz_fix_overlaps(i) ? "true" : "false", r, g, b, l->dim, l->r, l->g, l->b, l->w,
               l->strobe, l->pan, l->tilt);
    }
    jw_raw(&w, "],\"escenas\":[");
    for (int i = 0; i < LZ.nscenes; i++) { if (i) jw_raw(&w, ","); jw_str(&w, LZ.scenes[i].name); }
    jw_raw(&w, "],\"tipos\":[");
    for (int i = 0; i < LZ.ntypes; i++) {
        const lz_type_t *t = &LZ.types[i];
        jw_raw(&w, i ? ",{\"n\":" : "{\"n\":");
        jw_str(&w, t->name);
        jw_raw(&w, ",\"archivo\":");
        jw_str(&w, t->file);
        jw_fmt(&w, ",\"color\":%s,\"strobe\":%s,\"pos\":%s,\"canales\":[", t->has_color ? "true" : "false", t->has_strobe ? "true" : "false",
               t->has_pos ? "true" : "false");
        for (int c = 0; c < t->n; c++) {
            if (c) jw_raw(&w, ",");
            if (t->attr[c] == LZ_A_CONST) jw_fmt(&w, "\"%u\"", t->def[c]);
            else jw_str(&w, lz_attr_name(t->attr[c]));
        }
        jw_raw(&w, "]}");
    }
    jw_raw(&w, "],\"universo\":\"");
    if (w.k + 1030 < w.n) {
        static const char HEX[] = "0123456789abcdef";
        for (int c = 1; c <= 512; c++) { w.b[w.k++] = HEX[LZ.uni[c] >> 4]; w.b[w.k++] = HEX[LZ.uni[c] & 15]; }
        w.b[w.k] = 0;
    }
    jw_raw(&w, "\"}");
    aos_hal_live_put(LIVE_APP, "estado", "application/json", buf, w.k);
}

void lz_remote_tick(void)
{
    static uint32_t n;
    n++;
    take_orders();
    if (aos_hal_live_idle_ms(LIVE_APP) > 4000) return;     /* nobody looks */
    if (n % 6 == 0) put_state();                            /* the timer is 40 ms: four times a second */
}
