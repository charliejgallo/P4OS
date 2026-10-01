/*
 * P4OS - modules.txt as a text: check, read, save (aos_modules.h).
 */
#include "aos_modules.h"
#include "aos_io.h"
#include "aos_sensors.h"
#include "aos_hal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strncpy that always terminates, and that GCC's format-truncation check
 * leaves alone (it flags every snprintf of a longer buffer into a shorter) */
static void scpy(char *d, size_t n, const char *s)
{
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

#define FILE_MAX 8192           /* what aos_io_load() reads */

/* aos_io.c's words, and what they mean */
static const struct { const char *why; int code; } WHY[] = {
    { "line too long", AOS_MOD_E_LONG },
    { "expected: port|module <name> ...", AOS_MOD_E_SYNTAX },
    { "too many ports", AOS_MOD_E_TOO_MANY_PORTS },
    { "port name too long", AOS_MOD_E_PORT_NAME },
    { "port names start with uart, i2c, spi or gpio", AOS_MOD_E_PORT_KIND },
    { "a GPIO that is not on the header", AOS_MOD_E_NOT_HEADER },
    { "a reserved pin (console, BOOT)", AOS_MOD_E_RESERVED },
    { "missing pin", AOS_MOD_E_MISSING_PIN },
    { "two ports with the same name", AOS_MOD_E_DUP_PORT },
    { "too many modules", AOS_MOD_E_TOO_MANY_MODULES },
    { "module needs a port", AOS_MOD_E_MODULE_PORT },
    { "unknown line (port, module)", AOS_MOD_E_UNKNOWN },
};

typedef struct { char name[AOS_IO_PORT_NAME_MAX + 8]; int kind; int pins[4]; int line; } lport_t;

static void warn(aos_mod_check_t *o, int code, int line, int gpio, const char *a, const char *b)
{
    if (o->nwarn >= AOS_MOD_WARN_MAX) return;
    aos_mod_msg_t *w = &o->warn[o->nwarn++];
    memset(w, 0, sizeof *w);
    w->code = code;
    w->line = line;
    w->gpio = gpio;
    scpy(w->a, sizeof w->a, a ? a : "");
    scpy(w->b, sizeof w->b, b ? b : "");
}

void aos_modules_check(const char *text, size_t len, aos_mod_check_t *o)
{
    memset(o, 0, sizeof *o);
    char err[96] = "";
    o->ok = aos_io_validate(text, len, err, sizeof err);
    if (!o->ok) {
        int line = 0, off = 0;
        if (sscanf(err, "line %d: %n", &line, &off) >= 1 && off > 0) {
            o->err.line = line;
            o->err.code = AOS_MOD_E_OTHER;
            for (size_t i = 0; i < sizeof WHY / sizeof WHY[0]; i++)
                if (!strcmp(err + off, WHY[i].why)) o->err.code = WHY[i].code;
            scpy(o->err.raw, sizeof o->err.raw, err + off);
        } else {
            o->err.code = AOS_MOD_E_OTHER;
            scpy(o->err.raw, sizeof o->err.raw, err);
        }
    }

    /* a second pass of our own: counts, duplicates, and the warnings */
    lport_t *ports = calloc(AOS_IO_PORT_MAX + 4, sizeof *ports);
    if (!ports) return;
    int np = 0, line_no = 0;
    struct { char name[24], port[24]; int line; bool sensor; } mods[AOS_IO_MODULE_MAX + 4];
    int nm = 0;
    for (size_t pos = 0; pos < len; ) {
        char line[160];
        size_t n = 0;
        line_no++;
        while (pos < len && text[pos] != '\n') { if (n + 1 < sizeof line) line[n++] = text[pos]; pos++; }
        pos++;
        line[n] = 0;
        char *c = line;
        while (*c == ' ' || *c == '\t') c++;
        if (!*c || *c == '#') continue;
        char kw[12], name[32], rest[24];
        int off = 0;
        if (sscanf(c, "%11s %31s %n", kw, name, &off) < 2) continue;
        if (!strcmp(kw, "port") && np < AOS_IO_PORT_MAX + 4) {
            lport_t *p = &ports[np];
            scpy(p->name, sizeof p->name, name);
            p->line = line_no;
            p->kind = !strncmp(name, "uart", 4) ? AOS_PORT_UART : !strncmp(name, "i2c", 3) ? AOS_PORT_I2C
                    : !strncmp(name, "spi", 3) ? AOS_PORT_SPI : AOS_PORT_GPIO;
            static const char *const K[4][4] = { { "tx", "rx", "de", NULL }, { "sda", "scl", NULL, NULL },
                                                 { "sck", "mosi", "miso", "cs" }, { "pin", NULL, NULL, NULL } };
            for (int k = 0; k < 4; k++) p->pins[k] = K[p->kind][k] ? aos_io_arg_int(c + off, K[p->kind][k], -1) : -1;
            for (int i = 0; i < np && o->ok; i++)
                if (!strcmp(ports[i].name, p->name)) {
                    /* aos_io_validate() lets this through, aos_io_load() does not */
                    o->ok = false;
                    o->err.code = AOS_MOD_E_DUP_PORT;
                    o->err.line = line_no;
                    scpy(o->err.a, sizeof o->err.a, p->name);
                }
            np++;
        } else if (!strcmp(kw, "module") && nm < AOS_IO_MODULE_MAX + 4) {
            if (sscanf(c + off, "%23s", rest) < 1) continue;
            scpy(mods[nm].name, sizeof mods[nm].name, name);
            scpy(mods[nm].port, sizeof mods[nm].port, rest);
            mods[nm].line = line_no;
            char chip[24] = "";
            const char *cp = strstr(c + off, "chip=");
            if (cp) sscanf(cp + 5, "%23[^ \t#]", chip);
            mods[nm].sensor = aos_sensor_chip_find(chip[0] ? chip : name) != NULL;
            nm++;
        }
    }
    o->ports = np;
    o->modules = nm;
    /* a GPIO in two ports (i2c.board is the board's own and never claimed) */
    for (int i = 0; i < np; i++)
        for (int k = 0; k < 4; k++) {
            int g = ports[i].pins[k];
            if (g < 0) continue;
            for (int j = i + 1; j < np; j++)
                for (int m = 0; m < 4; m++)
                    if (ports[j].pins[m] == g) warn(o, AOS_MOD_W_PIN_TWICE, ports[j].line, g, ports[i].name, ports[j].name);
        }
    for (int i = 0; i < nm; i++) {
        int pk = -1;
        for (int j = 0; j < np; j++) if (!strcmp(ports[j].name, mods[i].port)) pk = ports[j].kind;
        if (pk < 0) warn(o, AOS_MOD_W_NO_PORT, mods[i].line, -1, mods[i].name, mods[i].port);
        else if (mods[i].sensor && pk != AOS_PORT_I2C) warn(o, AOS_MOD_W_NOT_I2C, mods[i].line, -1, mods[i].name, mods[i].port);
    }
    free(ports);
}

/* What is loaded, written out as a modules.txt */
static void dump(char *out, size_t n)
{
    size_t o = 0;
#define ADD(...) do { int w_ = snprintf(out + o, n - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_ < n - o ? (size_t)w_ : n - o - 1; } while (0)
    ADD("# P4OS: qué hay conectado al header (docs/MODULES.md)\n");
    ADD("# port <nombre> <pines>   module <nombre> <puerto> [addr= chip= ...]\n\n");
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        switch (p->kind) {
        case AOS_PORT_UART:
            ADD("port %s tx=%d rx=%d", p->name, p->pins[0], p->pins[1]);
            if (p->pins[2] >= 0) ADD(" de=%d", p->pins[2]);
            ADD(" baud=%u\n", (unsigned)p->freq);
            break;
        case AOS_PORT_I2C: ADD("port %s sda=%d scl=%d freq=%u\n", p->name, p->pins[0], p->pins[1], (unsigned)p->freq); break;
        case AOS_PORT_SPI:
            ADD("port %s sck=%d mosi=%d", p->name, p->pins[0], p->pins[1]);
            if (p->pins[2] >= 0) ADD(" miso=%d", p->pins[2]);
            if (p->pins[3] >= 0) ADD(" cs=%d", p->pins[3]);
            ADD(" freq=%u\n", (unsigned)p->freq);
            break;
        default: ADD("port %s pin=%d\n", p->name, p->pins[0]); break;
        }
    }
    if (aos_io_module_count()) ADD("\n");
    for (int i = 0; i < aos_io_module_count(); i++) {
        const aos_io_module_t *m = aos_io_module_at(i);
        ADD("module %s %s%s%s\n", m->name, m->port, m->args[0] ? " " : "", m->args);
    }
#undef ADD
}

char *aos_modules_read(bool *from_file)
{
    char *buf = malloc(FILE_MAX + 1);
    if (!buf) return NULL;
    if (from_file) *from_file = false;
    const char *root = aos_hal_path_sd_root();
    if (root) {
        char path[256];
        snprintf(path, sizeof path, "%s/modules.txt", root);
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t n = fread(buf, 1, FILE_MAX, f);
            fclose(f);
            buf[n] = 0;
            if (from_file) *from_file = true;
            return buf;
        }
    }
    dump(buf, FILE_MAX + 1);
    return buf;
}

bool aos_modules_save(const char *text, size_t len, aos_mod_check_t *out)
{
    aos_mod_check_t tmp;
    if (!out) out = &tmp;
    aos_modules_check(text, len, out);
    if (!out->ok) return false;
    if (len > FILE_MAX) { out->ok = false; out->err.code = AOS_MOD_E_LONG; return false; }
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    char path[256];
    snprintf(path, sizeof path, "%s/modules.txt", root);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, len, f) == len;
    if (len && text[len - 1] != '\n') ok = ok && fputc('\n', f) != EOF;
    ok = fclose(f) == 0 && ok;
    if (!ok) return false;
    aos_io_load();
    aos_sensors_rescan();
    aos_hal_log("io", "modules.txt saved: %d ports, %d modules", out->ports, out->modules);
    return true;
}

bool aos_modules_append(const char *line, aos_mod_check_t *out)
{
    char *t = aos_modules_read(NULL);
    if (!t) return false;
    size_t n = strlen(t), l = strlen(line);
    char *u = realloc(t, n + l + 3);
    if (!u) { free(t); return false; }
    if (n && u[n - 1] != '\n') u[n++] = '\n';
    memcpy(u + n, line, l);
    u[n + l] = '\n';
    u[n + l + 1] = 0;
    bool ok = aos_modules_save(u, n + l + 1, out);
    free(u);
    return ok;
}

void aos_modules_free_name(const char *base, char *out, size_t n)
{
    scpy(out, n, base);
    for (int k = 2; aos_io_module_find(out) && k < 100; k++) snprintf(out, n, "%s_%d", base, k);
}
