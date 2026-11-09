/*
 * P4OS - Modbus: device profiles (mb_prof.h says what one is).
 */
#include "mb_prof.h"

#include "aos_hal.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *const MB_TYPE_NAME[MB_T_COUNT] = { "bit", "u16", "s16", "hex", "u32", "s32", "f32", "u32cdab", "s32cdab", "f32cdab" };
const char *const MB_SIM_NAME[MB_SIM_COUNT] = { "fijo", "seno", "rampa", "deriva", "contador", "alterna", "azar" };
const char *const MB_TABLE_NAME[5] = { "", "coil", "di", "hr", "ir" };

/* ---- the folder ---- */

const char *mb_prof_dir(void)
{
    static char dir[96];
    const char *root = aos_hal_path_sd_root();
    char base[80];
    snprintf(base, sizeof base, "%s/modbus", root ? root : aos_hal_path_data());
    mkdir(base, 0777);
    snprintf(dir, sizeof dir, "%s/perfiles", base);
    mkdir(dir, 0777);
    return dir;
}

static bool is_profile(const char *n)
{
    size_t l = strlen(n);
    return n[0] != '.' && l > 4 && l < 48 && !strcmp(n + l - 4, ".csv");
}

int mb_prof_list(char (*names)[48], int max)
{
    DIR *d = opendir(mb_prof_dir());
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) if (is_profile(e->d_name)) snprintf(names[n++], 48, "%.47s", e->d_name);
    closedir(d);
    /* sorted by name, a few dozen at most: insertion */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[48];
            memcpy(t, names[j], 48);
            memcpy(names[j], names[j - 1], 48);
            memcpy(names[j - 1], t, 48);
        }
    return n;
}

static void path_of(const char *file, char *out, size_t n) { snprintf(out, n, "%s/%s", mb_prof_dir(), file); }

uint32_t mb_prof_mtime(const char *file)
{
    char p[160];
    path_of(file, p, sizeof p);
    struct stat st;
    return stat(p, &st) ? 0 : (uint32_t)st.st_mtime ^ (uint32_t)st.st_size;
}

bool mb_prof_delete(const char *file)
{
    char p[160];
    path_of(file, p, sizeof p);
    return remove(p) == 0;
}

void mb_prof_file_for(const char *name, char *out, size_t n)
{
    char s[40];
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && k < sizeof s - 1; p++) {
        unsigned c = *p;
        if (c >= 0x80) {                    /* an accent or an ñ: its base letter, roughly */
            if (c == 0xC3 && p[1]) {
                unsigned d = p[1];
                const char *map = "aaaaaaaceeeeiiiidnooooo/ouuuuyby";
                c = d >= 0xA0 && d <= 0xBF ? (unsigned char)map[d - 0xA0] : '-';
                p++;
            } else c = '-';
        }
        c = (unsigned)tolower((int)c);
        if (!isalnum((int)c)) c = '-';
        if (c == '-' && (!k || s[k - 1] == '-')) continue;
        s[k++] = (char)c;
    }
    while (k && s[k - 1] == '-') k--;
    s[k] = 0;
    snprintf(out, n, "%s.csv", k ? s : "perfil");
}

/* ---- reading ---- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (*s == '"' && e > s + 1 && e[-1] == '"') { e[-1] = 0; s++; }
    return s;
}

/* Splits a CSV line in place (quotes keep their commas). */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        bool q = false;
        while (*p && (q || *p != ',')) { if (*p == '"') q = !q; p++; }
        if (!*p) break;
        *p++ = 0;
    }
    for (int i = 0; i < n; i++) f[i] = trim(f[i]);
    return n;
}

static double num(const char *s, double def)
{
    if (!s || !*s) return def;
    char *end;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (double)strtoul(s + 2, &end, 16);
    double v = strtod(s, &end);
    return end == s ? def : v;
}

static int table_of(const char *s)
{
    if (!strcasecmp(s, "coil") || !strcasecmp(s, "coils") || !strcmp(s, "0x")) return MB_TAB_COIL;
    if (!strcasecmp(s, "di") || !strcasecmp(s, "discrete") || !strcasecmp(s, "entrada") || !strcasecmp(s, "entradas") || !strcmp(s, "1x")) return MB_TAB_DI;
    if (!strcasecmp(s, "hr") || !strcasecmp(s, "holding") || !strcmp(s, "4x")) return MB_TAB_HR;
    if (!strcasecmp(s, "ir") || !strcasecmp(s, "input") || !strcmp(s, "3x")) return MB_TAB_IR;
    return 0;
}

static int find(const char *const *names, int n, const char *s, int def)
{
    for (int i = 0; i < n; i++) if (!strcasecmp(names[i], s)) return i;
    return def;
}

bool mb_prof_load(const char *file, mb_prof_t *p, char *err, size_t err_len)
{
    char path[160];
    path_of(file, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, err_len, "no se pudo abrir %s", file); return false; }
    memset(p, 0, sizeof *p);
    snprintf(p->file, sizeof p->file, "%s", file);
    snprintf(p->name, sizeof p->name, "%.*s", (int)(strlen(file) > 4 ? strlen(file) - 4 : strlen(file)), file);
    p->unit = 1;
    p->poll_ms = 1000;
    char line[256];
    int ln = 0, skipped = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = trim(line);
        if (ln == 1 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s += 3;   /* a BOM */
        if (!*s || *s == '#') continue;
        char *c[12];
        int nf = split(s, c, 12);
        const char *k = c[0];
        const char *v = nf > 1 ? c[1] : "";
        if (!strcasecmp(k, "nombre") || !strcasecmp(k, "name")) snprintf(p->name, sizeof p->name, "%s", v);
        else if (!strcasecmp(k, "nota") || !strcasecmp(k, "note")) snprintf(p->note, sizeof p->note, "%s", v);
        else if (!strcasecmp(k, "esclavo") || !strcasecmp(k, "slave")) { double u = num(v, 1); p->unit = (uint8_t)(u < 0 ? 0 : u > 247 ? 247 : u); }
        else if (!strcasecmp(k, "velocidad") || !strcasecmp(k, "baud")) p->baud = (uint32_t)num(v, 0);
        else if (!strcasecmp(k, "formato") || !strcasecmp(k, "format")) {
            if (strlen(v) == 3 && v[0] == '8') {
                char par = (char)toupper((unsigned char)v[1]);
                if (par == 'N' || par == 'E' || par == 'O') { p->par = par; p->stop = v[2] == '2' ? 2 : 1; }
            }
        } else if (!strcasecmp(k, "periodo") || !strcasecmp(k, "poll")) {
            double ms = num(v, 1000);
            p->poll_ms = (uint16_t)(ms < 50 ? 50 : ms > 60000 ? 60000 : ms);
        } else if (table_of(k)) {
            if (p->n >= MB_PTS) { skipped++; continue; }
            mb_point_t *pt = &p->pt[p->n];
            memset(pt, 0, sizeof *pt);
            pt->table = (uint8_t)table_of(k);
            double a = num(v, -1);
            if (a < 0 || a > 65535) { skipped++; continue; }
            pt->addr = (uint16_t)a;
            snprintf(pt->name, sizeof pt->name, "%s", nf > 2 && c[2][0] ? c[2] : "?");
            bool bits = pt->table <= MB_TAB_DI;
            pt->type = bits ? MB_T_BIT : (uint8_t)find(MB_TYPE_NAME, MB_T_COUNT, nf > 3 ? c[3] : "", MB_T_U16);
            if (!bits && pt->type == MB_T_BIT) pt->type = MB_T_U16;
            pt->scale = (float)num(nf > 4 ? c[4] : "", 1);
            if (pt->scale == 0) pt->scale = 1;
            snprintf(pt->unit, sizeof pt->unit, "%s", nf > 5 ? c[5] : "");
            pt->rw = nf > 6 && (!strcasecmp(c[6], "rw") || !strcasecmp(c[6], "w") || !strcasecmp(c[6], "le"));
            if (pt->table == MB_TAB_DI || pt->table == MB_TAB_IR) pt->rw = false;    /* the standard has no writes there */
            pt->min = (float)num(nf > 7 ? c[7] : "", 0);
            pt->max = (float)num(nf > 8 ? c[8] : "", bits ? 1 : 0);
            pt->sim = (uint8_t)find(MB_SIM_NAME, MB_SIM_COUNT, nf > 9 ? c[9] : "", MB_SIM_NONE);
            pt->value = (float)num(nf > 10 ? c[10] : "", pt->min);
            p->n++;
        } else skipped++;
    }
    fclose(f);
    if (skipped) snprintf(err, err_len, "%d líneas sin entender", skipped);
    else if (err_len) err[0] = 0;
    return true;
}

/* ---- writing ---- */

static void fnum(char *o, size_t n, double v)
{
    snprintf(o, n, "%.6g", v);
}

static void fstr(FILE *f, const char *s)
{
    if (strchr(s, ',') || strchr(s, '"')) {
        fputc('"', f);
        for (; *s; s++) if (*s != '"') fputc(*s, f);
        fputc('"', f);
    } else fputs(s, f);
}

bool mb_prof_save(const mb_prof_t *p)
{
    char path[160], tmp[168];
    path_of(p->file, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs("# P4OS Modbus profile (apps/modbus/README.md, \"Perfiles\")\n", f);
    fputs("nombre,", f); fstr(f, p->name); fputc('\n', f);
    if (p->note[0]) { fputs("nota,", f); fstr(f, p->note); fputc('\n', f); }
    fprintf(f, "esclavo,%u\n", p->unit);
    if (p->baud) fprintf(f, "velocidad,%lu\n", (unsigned long)p->baud);
    if (p->par) fprintf(f, "formato,8%c%u\n", p->par, p->stop ? p->stop : 1);
    fprintf(f, "periodo,%u\n", p->poll_ms);
    fputs("# tabla,direccion,nombre,tipo,escala,unidad,acceso,min,max,simulacion,valor\n", f);
    for (int i = 0; i < p->n; i++) {
        const mb_point_t *pt = &p->pt[i];
        char sc[24], mn[24], mx[24], va[24];
        fnum(sc, sizeof sc, pt->scale);
        fnum(mn, sizeof mn, pt->min);
        fnum(mx, sizeof mx, pt->max);
        fnum(va, sizeof va, pt->value);
        fprintf(f, "%s,%u,", MB_TABLE_NAME[pt->table], pt->addr);
        fstr(f, pt->name);
        fprintf(f, ",%s,%s,", MB_TYPE_NAME[pt->type], pt->type == MB_T_BIT ? "" : sc);
        fstr(f, pt->unit);
        fprintf(f, ",%s,%s,%s,%s,%s\n", pt->rw ? "rw" : "r", mn, mx, pt->sim ? MB_SIM_NAME[pt->sim] : "", va);
    }
    bool ok = fclose(f) == 0;
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    return ok;
}

/* ---- the examples ---- */

static const struct { const char *file, *text; } EXAMPLES[] = {
    { "variador-esp32.csv",
      "# P4OS Modbus profile: the drive the bench's ESP32 plays (tools/rs485_test)\n"
      "nombre,Variador del banco (ESP32)\n"
      "nota,El ESP32 de tools/rs485_test como esclavo: marcha en la coil 0\n"
      "esclavo,1\nvelocidad,9600\nformato,8N1\nperiodo,500\n"
      "# tabla,direccion,nombre,tipo,escala,unidad,acceso,min,max,simulacion,valor\n"
      "hr,0,Consigna,u16,1,rpm,rw,0,1500,,1450\n"
      "hr,1,Rampa de subida,u16,0.1,s,rw,0,60,,5\n"
      "hr,2,Rampa de bajada,u16,0.1,s,rw,0,60,,3\n"
      "hr,3,Límite de corriente,u16,0.1,A,rw,0,20,,16\n"
      "ir,0,Velocidad,u16,1,rpm,r,0,1450,rampa,0\n"
      "ir,1,Corriente,u16,0.1,A,r,2,10,deriva,2\n"
      "ir,2,Bus de continua,u16,1,V,r,560,570,seno,565\n"
      "ir,3,Temperatura,u16,0.1,°C,r,28,45,deriva,30\n"
      "ir,4,Horas de marcha,u16,1,h,r,0,0,,0\n"
      "coil,0,Marcha,bit,,,rw,,,,0\n"
      "coil,1,Reversa,bit,,,rw,,,,0\n"
      "coil,3,Relé 1,bit,,,rw,,,,0\n"
      "di,0,Girando,bit,,,r,,,alterna,0\n"
      "di,1,En velocidad,bit,,,r,,,,0\n"
      "di,2,En límite de corriente,bit,,,r,,,,0\n" },
    { "multisensor.csv",
      "# P4OS Modbus profile: a generic multisensor, an example\n"
      "nombre,Multisensor\n"
      "nota,\"Temperatura, humedad, presión, CO2 y luz (un ejemplo genérico)\"\n"
      "esclavo,1\nvelocidad,9600\nformato,8N1\nperiodo,1000\n"
      "# tabla,direccion,nombre,tipo,escala,unidad,acceso,min,max,simulacion,valor\n"
      "ir,0,Temperatura,s16,0.1,°C,r,18,27,seno,22\n"
      "ir,1,Humedad,u16,0.1,%,r,40,70,deriva,55\n"
      "ir,2,Presión,u16,0.1,hPa,r,1008,1016,deriva,1013\n"
      "ir,3,CO2,u16,1,ppm,r,420,1200,seno,600\n"
      "ir,4,Luz,u16,1,lx,r,0,800,seno,300\n"
      "ir,5,Ruido,u16,0.1,dB,r,35,60,azar,40\n"
      "di,0,Movimiento,bit,,,r,,,alterna,0\n"
      "hr,0,Dirección Modbus,u16,1,,rw,1,247,,1\n"
      "hr,1,Período de medición,u16,1,s,rw,1,60,,5\n"
      "coil,0,LED,bit,,,rw,,,,0\n" },
    { "medidor-energia.csv",
      "# P4OS Modbus profile: a single-phase energy meter laid out as the usual\n"
      "# DIN-rail ones (Eastron SDM120 style: floats in input registers, ABCD).\n"
      "# Check the addresses against your meter's manual.\n"
      "nombre,Medidor de energía monofásico\n"
      "nota,\"Floats en input, como los medidores de riel DIN (estilo SDM120)\"\n"
      "esclavo,1\nvelocidad,2400\nformato,8N1\nperiodo,1000\n"
      "# tabla,direccion,nombre,tipo,escala,unidad,acceso,min,max,simulacion,valor\n"
      "ir,0,Tensión,f32,1,V,r,218,232,seno,225\n"
      "ir,6,Corriente,f32,1,A,r,0.2,8,deriva,2\n"
      "ir,12,Potencia activa,f32,1,W,r,40,1800,deriva,450\n"
      "ir,18,Potencia aparente,f32,1,VA,r,50,1900,deriva,480\n"
      "ir,30,Factor de potencia,f32,1,,r,0.85,0.99,deriva,0.95\n"
      "ir,70,Frecuencia,f32,1,Hz,r,49.9,50.1,seno,50\n"
      "ir,72,Energía importada,f32,1,kWh,r,0,0.5,contador,1234.5\n"
      "ir,342,Energía total,f32,1,kWh,r,0,0.5,contador,1250\n" },
    { "temp-humedad.csv",
      "# P4OS Modbus profile: the common temperature and humidity sensors on\n"
      "# RS485 (XY-MD02 / SHT20 style). Check the map against your sensor's.\n"
      "nombre,Sensor de temperatura y humedad\n"
      "nota,Estilo XY-MD02: input 1 y 2; dirección y velocidad en holding\n"
      "esclavo,1\nvelocidad,9600\nformato,8N1\nperiodo,1000\n"
      "# tabla,direccion,nombre,tipo,escala,unidad,acceso,min,max,simulacion,valor\n"
      "ir,1,Temperatura,s16,0.1,°C,r,19,26,seno,22.5\n"
      "ir,2,Humedad,u16,0.1,%,r,45,65,deriva,52\n"
      "hr,0x101,Dirección Modbus,u16,1,,rw,1,247,,1\n"
      "hr,0x102,Velocidad (0 9600 / 1 14400 / 2 19200),u16,1,,rw,0,2,,0\n"
      "hr,0x103,Corrección de temperatura,s16,0.1,°C,rw,-10,10,,0\n"
      "hr,0x104,Corrección de humedad,s16,0.1,%,rw,-10,10,,0\n" },
};

void mb_prof_examples(void)
{
    char names[1][48];
    if (mb_prof_list(names, 1) > 0) return;
    for (size_t i = 0; i < sizeof EXAMPLES / sizeof EXAMPLES[0]; i++) {
        char p[160];
        path_of(EXAMPLES[i].file, p, sizeof p);
        FILE *f = fopen(p, "w");
        if (!f) return;
        fputs(EXAMPLES[i].text, f);
        fclose(f);
    }
}

/* ---- values ---- */

int mb_pt_words(const mb_point_t *pt)
{
    switch (pt->type) {
    case MB_T_U32: case MB_T_S32: case MB_T_F32: case MB_T_U32R: case MB_T_S32R: case MB_T_F32R: return 2;
    default: return 1;
    }
}

static uint32_t join(const mb_point_t *pt, const uint16_t *r)
{
    bool swap = pt->type >= MB_T_U32R;
    return swap ? ((uint32_t)r[1] << 16 | r[0]) : ((uint32_t)r[0] << 16 | r[1]);
}

double mb_pt_decode(const mb_point_t *pt, const uint16_t *r)
{
    double s = pt->scale ? pt->scale : 1;
    switch (pt->type) {
    case MB_T_BIT: return r[0] ? 1 : 0;
    case MB_T_S16: return (int16_t)r[0] * s;
    case MB_T_U32: case MB_T_U32R: return join(pt, r) * s;
    case MB_T_S32: case MB_T_S32R: return (int32_t)join(pt, r) * s;
    case MB_T_F32: case MB_T_F32R: { uint32_t w = join(pt, r); float f; memcpy(&f, &w, 4); return f * s; }
    default: return r[0] * s;
    }
}

static long lrnd(double v) { return (long)(v < 0 ? v - 0.5 : v + 0.5); }

void mb_pt_encode(const mb_point_t *pt, double v, uint16_t *r)
{
    double s = pt->scale ? pt->scale : 1;
    uint32_t w = 0;
    switch (pt->type) {
    case MB_T_BIT: r[0] = v != 0; return;
    case MB_T_S16: { long x = lrnd(v / s); r[0] = (uint16_t)(int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); return; }
    case MB_T_U16: case MB_T_HEX: { long x = lrnd(v / s); r[0] = (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x); return; }
    case MB_T_U32: case MB_T_U32R: { double x = v / s; w = x < 0 ? 0 : x > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)(x + 0.5); break; }
    case MB_T_S32: case MB_T_S32R: w = (uint32_t)(int32_t)lrnd(v / s); break;
    default: { float f = (float)(v / s); memcpy(&w, &f, 4); break; }
    }
    bool swap = pt->type >= MB_T_U32R;
    r[swap ? 1 : 0] = (uint16_t)(w >> 16);
    r[swap ? 0 : 1] = (uint16_t)w;
}

int mb_pt_decimals(const mb_point_t *pt)
{
    if (pt->type == MB_T_BIT || pt->type == MB_T_HEX) return 0;
    double s = fabs(pt->scale ? pt->scale : 1);
    if (pt->type == MB_T_F32 || pt->type == MB_T_F32R) return s >= 1 ? 2 : 3;
    if (s >= 1) return 0;
    int d = 0;
    while (s < 0.999 && d < 4) { s *= 10; d++; }
    return d;
}

void mb_pt_format(const mb_point_t *pt, double v, char *o, size_t n)
{
    if (pt->type == MB_T_BIT) { snprintf(o, n, "%s", v ? "ON" : "OFF"); return; }
    if (pt->type == MB_T_HEX) { snprintf(o, n, "0x%04X", (unsigned)lrnd(v) & 0xFFFF); return; }
    char t[32];
    snprintf(t, sizeof t, "%.*f", mb_pt_decimals(pt), v);
    for (char *p = t; *p; p++) if (*p == '.') *p = ',';
    snprintf(o, n, "%s%s%s", t, pt->unit[0] ? " " : "", pt->unit);
}

static uint32_t s_rng = 0x9E3779B9;
static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xFFFFFF) / 16777216.0f;
}

double mb_pt_sim(const mb_point_t *pt, double last, float t, float dt)
{
    double lo = pt->min, hi = pt->max, mid = (lo + hi) / 2, amp = (hi - lo) / 2;
    float phase = (float)(pt->addr % 7);            /* so two points do not move together */
    switch (pt->sim) {
    case MB_SIM_SINE: return mid + amp * sinf(t * 6.2831853f / 60.0f + phase);
    case MB_SIM_RAMP: { float f = fmodf(t + phase * 8, 60.0f) / 60.0f; return lo + (hi - lo) * f; }
    case MB_SIM_DRIFT: {
        double v = last + (frand() - 0.5f) * (hi - lo) * 0.02;
        return v < lo ? lo : v > hi ? hi : v;
    }
    case MB_SIM_COUNTER: return last + (hi > 0 ? hi / 3600.0 : 1.0) * dt;   /* max per hour, or 1 a second */
    case MB_SIM_TOGGLE: {
        float period = 2.0f + phase;
        bool on = fmodf(t, 2 * period) >= period;
        return pt->type == MB_T_BIT ? on : (on ? hi : lo);
    }
    case MB_SIM_RANDOM: {
        /* a new value every second, not every tick */
        if (fmodf(t, 1.0f) >= dt) return last;
        return pt->type == MB_T_BIT ? (frand() < 0.5f) : lo + (hi - lo) * frand();
    }
    default: return last;
    }
}

/* ---- the reads ---- */

int mb_prof_blocks(const mb_prof_t *p, mb_block_t *b, int max, int *blk_of)
{
    int idx[MB_PTS], n = p->n;
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            const mb_point_t *a = &p->pt[idx[j - 1]], *c = &p->pt[idx[j]];
            if (a->table < c->table || (a->table == c->table && a->addr <= c->addr)) break;
            int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    int nb = 0;
    for (int k = 0; k < n; k++) {
        const mb_point_t *pt = &p->pt[idx[k]];
        bool bits = pt->table <= MB_TAB_DI;
        int lim = bits ? 1000 : 120, gap = bits ? 64 : 8;
        uint32_t end = (uint32_t)pt->addr + (uint32_t)mb_pt_words(pt);     /* one past */
        if (nb) {
            mb_block_t *l = &b[nb - 1];
            uint32_t lend = (uint32_t)l->addr + l->n;
            if (l->table == pt->table && pt->addr <= lend + (uint32_t)gap && (end > lend ? end : lend) - l->addr <= (uint32_t)lim) {
                if (end > lend) l->n = (uint16_t)(end - l->addr);
                blk_of[idx[k]] = nb - 1;
                continue;
            }
        }
        if (nb >= max) { blk_of[idx[k]] = -1; continue; }
        b[nb].table = pt->table;
        b[nb].addr = pt->addr;
        b[nb].n = (uint16_t)(end - pt->addr);
        blk_of[idx[k]] = nb++;
    }
    return nb;
}
