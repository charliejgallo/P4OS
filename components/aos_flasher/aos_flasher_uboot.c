/*
 * P4OS - Programmer: anything with U-Boot on a serial console - an IP
 * camera, a router, a set-top box - its flash read out through U-Boot's own
 * commands, as a person would at the console. Only reading: nothing here
 * writes to the device's flash or to its environment (no saveenv, no sf
 * erase or write, no mw outside the working RAM it reads into).
 *
 * Detect: the port at the console's speed (115200 unless the job says
 * otherwise), and the user asked to power the device on, or reset it by EN
 * when the port has the line. Ctrl+C and Enter go out every 25 ms until a
 * prompt shows: a short line ending in "#", ">" or "=>" ("hisilicon # ",
 * "=> ", "U-Boot> "), confirmed when an Enter brings the same line back.
 * "version" must then say "U-Boot": a Linux shell's "# " is not mistaken for
 * it. A console that will not stop (no countdown, a key or a password asked
 * for) is said, and nothing more is tried. Then version, printenv (bootargs,
 * mtdparts, loadaddr), bdinfo (where the DRAM starts) and "sf probe 0"; the
 * flash's name, JEDEC ID and size come from what it and the boot log say,
 * in the forms U-Boot's versions print them ("16384 KiB hi_fmc at 0:0",
 * "Block:64KB Chip:16MB Name:\"MX25L128XX\"", "SF: Detected ... total 16
 * MiB"), or else from the sum of mtdparts.
 *
 * Read: 64 KB at a time, "sf read" into the working RAM (opts.ram; else
 * loadaddr; else the DRAM's start + 32 MB; else 0x82000000), then "crc32"
 * of it and "md.l" of it, whose lines are parsed back into bytes (U-Boot
 * takes every number in hex; a garbled line is just not taken). The piece
 * is kept when its CRC-32 is U-Boot's own; otherwise U-Boot's crc32 of each
 * 4 KB of it says which came wrong, and only those are printed again (one
 * bad character in 64 KB of md - some 256 thousand - no longer costs the
 * whole piece); three rounds at most. A piece whose
 * CRC is that of all 0xFF is not printed at all (an erased flash is mostly
 * that). A U-Boot without crc32 has each piece printed twice and compared.
 * At 115200 md.l moves about 3.9 characters a byte - 16 MB in some 70
 * minutes - so first the console goes faster: "setenv baudrate 921600"
 * (then 460800, 230400), which HiSilicon's U-Boot 2010.06 answers with
 * "press ENTER" at the new speed; never saveenv, so a restart brings the
 * speed back, and the job brings it back itself at the end.
 *
 * The boot log and what the commands said go in a .txt beside the dump (or,
 * for a detect, on their own in firmware/volcados): that is where the
 * device's Wi-Fi chip, flash and partitions are named. The device stays
 * stopped at the prompt: "reset" in the Terminal, or its power, boots it.
 *
 * HAL-clean: aos_io and the core, so the simulator runs it against
 * tools/fake_uboot.py.
 */
#include "aos_flasher_priv.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LOG_CAP     (96 * 1024)     /* the boot log and the commands' answers */
#define CHUNK       (64 * 1024)
#define LINE_MAX    320
#define PROMPT_MAX  24
#define STOP_MS     60000           /* the time the user has to power the device on */

typedef struct ub ub_t;
typedef void (*line_fn)(ub_t *s, const char *line, void *ctx);

struct ub {
    aos_io_uart_t *u;
    bool usb, lines;
    uint32_t base_baud, baud;
    char prompt[PROMPT_MAX];
    /* what comes in */
    char cur[LINE_MAX];
    int cur_n;
    uint32_t lines_in;              /* complete lines since the last command went out */
    char *log;
    uint32_t log_n;
    bool logging;
    bool saw_countdown, saw_linux;
    uint32_t heard;                 /* bytes, all of them */
    /* what U-Boot said */
    char ver[64], soc[16], flash_name[32], flash_id[40], baudrate_env[12];
    char bootargs[480], mtdparts[240];
    uint32_t flash_size, loadaddr, dram, ram;
    bool has_crc, probed;
    uint8_t *a, *b;                 /* CHUNK each */
};

/* -------------------------------------------------------------------------- */
/* CRC-32 (zlib's, U-Boot's crc32 command)                                     */
/* -------------------------------------------------------------------------- */

static uint32_t crc32_add(uint32_t crc, const uint8_t *p, uint32_t n)
{
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = T[crc & 15] ^ (crc >> 4);
        crc = T[crc & 15] ^ (crc >> 4);
    }
    return ~crc;
}

static uint32_t crc32_ff(uint32_t n)
{
    static const uint8_t FF[256] = { [0 ... 255] = 0xFF };
    uint32_t c = 0;
    for (uint32_t o = 0; o < n; o += 256) c = crc32_add(c, FF, n - o < 256 ? n - o : 256);
    return c;
}

/* -------------------------------------------------------------------------- */
/* The wire                                                                    */
/* -------------------------------------------------------------------------- */

static void log_add(ub_t *s, const char *p, uint32_t n)
{
    if (!s->logging || !s->log) return;
    if (n > LOG_CAP - 1 - s->log_n) n = LOG_CAP - 1 - s->log_n;
    memcpy(s->log + s->log_n, p, n);
    s->log_n += n;
}

static bool tx(ub_t *s, const char *p, int n)
{
    int done = 0;
    uint64_t end = aos_hal_uptime_ms() + 1000;
    while (done < n) {
        int w = aos_io_uart_write(s->u, p + done, n - done);
        if (w < 0) return false;
        done += w;
        if (done < n) {
            if (aos_hal_uptime_ms() > end) return false;
            aos_hal_sleep_ms(2);
        }
    }
    return true;
}

/* What comes in for up to ms (less once something did and it went quiet
 * for 'idle' ms; 0: the whole time): each complete line to fn, the line
 * being received in s->cur. True if anything came. */
static bool pump(ub_t *s, int ms, int idle, line_fn fn, void *ctx)
{
    char buf[256];
    bool any = false;
    uint64_t end = aos_hal_uptime_ms() + (uint64_t)ms, last = 0;
    while (!fl_cancel) {
        uint64_t now = aos_hal_uptime_ms();
        if (now >= end || (idle && any && now - last >= (uint64_t)idle)) break;
        int left = (int)(end - now);
        int n = aos_io_uart_read(s->u, buf, sizeof buf, left < 20 ? left : 20);
        if (n < 0) return any;
        if (!n) continue;
        any = true;
        last = aos_hal_uptime_ms();
        s->heard += (uint32_t)n;
        log_add(s, buf, (uint32_t)n);
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                s->cur[s->cur_n] = 0;
                s->lines_in++;
                if (fn) fn(s, s->cur, ctx);
                s->cur_n = 0;
            } else if (c == '\r' || c == 0) {
                continue;
            } else if (c == '\b') {
                if (s->cur_n) s->cur_n--;
            } else if (s->cur_n < LINE_MAX - 1) {
                s->cur[s->cur_n++] = (unsigned char)c < 0x20 || (unsigned char)c >= 0x7F ? '.' : c;
            }
        }
        s->cur[s->cur_n] = 0;
    }
    return any;
}

static bool at_prompt(const ub_t *s)
{
    return s->prompt[0] && s->cur_n == (int)strlen(s->prompt) && !memcmp(s->cur, s->prompt, (size_t)s->cur_n);
}

/* Lines that look like a prompt: short, and ending in "#", ">" or "=>"
 * (a space after them or not), with no "login" in them */
static bool prompt_like(const char *l)
{
    size_t n = strlen(l);
    if (n < 1 || n >= PROMPT_MAX) return false;
    size_t e = n;
    while (e && l[e - 1] == ' ') e--;
    if (!e) return false;
    char c = l[e - 1];
    if (c != '#' && c != '>') return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)l[i] < 0x20 || l[i] == '.') return false;
    return !strcasestr(l, "login");
}

/* One command: what it prints, line by line, to fn; true when the prompt
 * came back. 'idle' ms with nothing coming in ends it. */
static bool run_cmd(ub_t *s, const char *cmd, int idle, line_fn fn, void *ctx)
{
    pump(s, 30, 0, NULL, NULL);                 /* what was still coming */
    char line[96];
    int n = snprintf(line, sizeof line, "%s\r", cmd);
    s->lines_in = 0;
    s->cur_n = 0;
    s->cur[0] = 0;
    if (!tx(s, line, n)) return false;
    uint64_t last = aos_hal_uptime_ms();
    while (!fl_cancel) {
        if (pump(s, 20, 0, fn, ctx)) last = aos_hal_uptime_ms();
        if (s->lines_in && at_prompt(s)) return true;
        if (aos_hal_uptime_ms() - last > (uint64_t)idle) return false;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* What U-Boot says                                                            */
/* -------------------------------------------------------------------------- */

static void copy_word(char *out, size_t cap, const char *p)
{
    size_t k = 0;
    while (*p && *p != ' ' && *p != '"' && k + 1 < cap) out[k++] = *p++;
    out[k] = 0;
}

/* The flash, from whichever of U-Boot's ways of saying it this line is */
static void flash_line(ub_t *s, const char *l)
{
    const char *p;
    unsigned v;
    if ((p = strstr(l, " KiB ")) && strstr(l, "current device")) {
        const char *q = p;
        while (q > l && isdigit((unsigned char)q[-1])) q--;
        if (sscanf(q, "%u", &v) == 1 && v) s->flash_size = v << 10;
    }
    if ((p = strstr(l, "Chip:")) && sscanf(p + 5, "%u", &v) == 1 && v) s->flash_size = v << 20;
    if ((p = strstr(l, "total size:")) && sscanf(p + 11, " %u", &v) == 1 && v) s->flash_size = v << 20;
    if ((p = strstr(l, "total ")) && strstr(p, "MiB") && sscanf(p + 6, "%u", &v) == 1 && v) s->flash_size = v << 20;
    if ((p = strstr(l, "Name:\""))) copy_word(s->flash_name, sizeof s->flash_name, p + 6);
    if ((p = strstr(l, "SF: Detected "))) copy_word(s->flash_name, sizeof s->flash_name, p + 13);
    if ((p = strstr(l, "ID:")) && strstr(l, "0x")) {
        p += 3;
        while (*p == ' ') p++;
        snprintf(s->flash_id, sizeof s->flash_id, "%.39s", p);
    }
}

/* "hi3518ev200" anywhere: the SoC, as HiSilicon writes it */
static void soc_from(ub_t *s, const char *l)
{
    if (s->soc[0]) return;
    const char *p = strcasestr(l, "hi35");
    if (!p) p = strcasestr(l, "hi37");
    if (!p) return;
    size_t k = 0;
    s->soc[k++] = 'H';
    s->soc[k++] = 'i';
    for (p += 2; isalnum((unsigned char)*p) && k + 1 < sizeof s->soc; p++) s->soc[k++] = (char)toupper((unsigned char)*p);
    s->soc[k] = 0;
}

/* Every line of the boot, and of every command but md */
static void any_line(ub_t *s, const char *l, void *ctx)
{
    (void)ctx;
    if (strcasestr(l, "autoboot") || strcasestr(l, "any key")) s->saw_countdown = true;
    if (strstr(l, "Starting kernel") || strstr(l, "Linux version") || strstr(l, "Uncompressing Linux")) s->saw_linux = true;
    flash_line(s, l);
    soc_from(s, l);
}

static void version_line(ub_t *s, const char *l, void *ctx)
{
    any_line(s, l, ctx);
    const char *p = strstr(l, "U-Boot");
    if (p && !s->ver[0]) {
        /* "U-Boot 2010.06 (Mar 02 2018 - 15:10:50)": the name and number */
        const char *q = strchr(p + 7, ' ');
        int n = q ? (int)(q - p) : (int)strlen(p);
        snprintf(s->ver, sizeof s->ver, "%.*s", n > 63 ? 63 : n, p);
    }
}

static void env_line(ub_t *s, const char *l, void *ctx)
{
    any_line(s, l, ctx);
    if (!strncmp(l, "bootargs=", 9)) snprintf(s->bootargs, sizeof s->bootargs, "%s", l + 9);
    else if (!strncmp(l, "mtdparts=", 9)) snprintf(s->mtdparts, sizeof s->mtdparts, "%s", l + 9);
    else if (!strncmp(l, "loadaddr=", 9)) s->loadaddr = (uint32_t)strtoul(l + 9, NULL, 16);
    else if (!strncmp(l, "baudrate=", 9)) snprintf(s->baudrate_env, sizeof s->baudrate_env, "%.11s", l + 9);
}

static void bd_line(ub_t *s, const char *l, void *ctx)
{
    any_line(s, l, ctx);
    const char *p = strstr(l, "start");
    const char *x = p ? strstr(p, "0x") : NULL;
    if (x && !s->dram) s->dram = (uint32_t)strtoul(x, NULL, 16);
}

static void probe_line(ub_t *s, const char *l, void *ctx)
{
    bool *bad = ctx;
    any_line(s, l, NULL);
    if (strstr(l, "Unknown command") || strstr(l, "Failed") || strstr(l, "failed") || strstr(l, "No SPI")) *bad = true;
}

typedef struct { uint32_t crc; bool got, bad; } crc_t;

static void crc_line(ub_t *s, const char *l, void *ctx)
{
    (void)s;
    crc_t *c = ctx;
    const char *p = strstr(l, "==>");
    if (p) {
        char *e;
        unsigned long v = strtoul(p + 3, &e, 16);
        if (e != p + 3) { c->crc = (uint32_t)v; c->got = true; }
    }
}

/* mtdparts=hi_sfc:256K(boot),64K(env),...: the sum, when no part is "-" */
static uint32_t mtd_total(const char *m)
{
    const char *p = strchr(m, ':');
    if (!p) return 0;
    uint32_t sum = 0;
    for (p++; *p && *p != ' ' && *p != ';'; ) {
        if (*p == '-') return 0;
        char *e;
        unsigned long v = strtoul(p, &e, 0);
        if (e == p) return 0;
        if (*e == 'K' || *e == 'k') v <<= 10, e++;
        else if (*e == 'M' || *e == 'm') v <<= 20, e++;
        sum += (uint32_t)v;
        p = e;
        if (*p == '@') { strtoul(p + 1, &e, 0); p = e; }
        if (*p == '(') { while (*p && *p != ')') p++; if (*p) p++; }
        if (*p == 'r' && p[1] == 'o') p += 2;
        if (*p == ',') p++;
        else break;
    }
    return sum;
}

/* -------------------------------------------------------------------------- */
/* Getting to the prompt                                                       */
/* -------------------------------------------------------------------------- */

static bool stop_autoboot(ub_t *s)
{
    fl_phase(AOS_FLASHER_CONNECTING);
    int en = -1, boot = -1;
    if (s->lines && aos_io_port_lines(J.port, &en, &boot)) {
        fl_logf(L_INFO, _("EN (GPIO%d) en bajo un momento, por si va al reset del aparato"), en);
        aos_io_uart_lines(s->u, 0, -1);
        aos_hal_sleep_ms(100);
        aos_io_uart_lines(s->u, 1, -1);
    }
    fl_logf(L_INFO, "%s", _("Prendé o reiniciá el aparato: se le frena el arranque"));
    uint64_t end = aos_hal_uptime_ms() + STOP_MS;
    bool ctrl = true;
    while (!fl_cancel && aos_hal_uptime_ms() < end) {
        tx(s, ctrl ? "\x03" : "\r", 1);
        ctrl = !ctrl;
        pump(s, 25, 0, any_line, NULL);
        if (!prompt_like(s->cur)) continue;
        /* settled, and an Enter brings the same line back */
        char cand[PROMPT_MAX];
        snprintf(cand, sizeof cand, "%s", s->cur);
        pump(s, 150, 0, any_line, NULL);
        for (int k = 0; k < 2; k++) {
            s->lines_in = 0;
            tx(s, "\r", 1);
            pump(s, 300, 120, any_line, NULL);
            if (s->lines_in && !strcmp(s->cur, cand)) {
                snprintf(s->prompt, sizeof s->prompt, "%s", cand);
                return true;
            }
        }
    }
    if (fl_cancel) return false;
    if (s->saw_linux) return fl_fail("%s", _("Arrancó sin dejarse frenar: la consola puede estar cerrada (queda la pinza SOIC8)"));
    if (s->saw_countdown) return fl_fail("%s", _("Vi la cuenta del autoboot pero no se frenó: puede pedir otra tecla o una clave"));
    if (s->heard) return fl_fail(_("Llegaron %u bytes sin un prompt: ¿la velocidad no es %u, o falta GND?"), (unsigned)s->heard, (unsigned)s->baud);
    return fl_fail("%s", _("No llegó nada: revisá TX/RX cruzados, GND y que el aparato se haya prendido"));
}

static bool read_info(ub_t *s)
{
    fl_phase(AOS_FLASHER_INFO);
    if (!run_cmd(s, "version", 2000, version_line, NULL) || !s->ver[0])
        return fl_fail(_("\"%s\" no es U-Boot: no contestó a version"), s->prompt);
    run_cmd(s, "printenv", 3000, env_line, NULL);
    if (!s->mtdparts[0]) {
        const char *m = strstr(s->bootargs, "mtdparts=");
        if (m) copy_word(s->mtdparts, sizeof s->mtdparts, m + 9);
    }
    run_cmd(s, "bdinfo", 2000, bd_line, NULL);
    bool bad = false;
    s->probed = run_cmd(s, "sf probe 0", 5000, probe_line, &bad) && !bad;
    if (!s->flash_size && s->mtdparts[0]) s->flash_size = mtd_total(s->mtdparts);
    s->ram = J.opts.ram ? J.opts.ram : s->loadaddr ? s->loadaddr : s->dram ? s->dram + 0x2000000u : 0x82000000u;
    /* crc32 there: a small one says whether the command exists */
    char cmd[48];
    snprintf(cmd, sizeof cmd, "crc32 0x%x 0x4", (unsigned)s->ram);
    crc_t c = { 0, false, false };
    run_cmd(s, cmd, 2000, crc_line, &c);
    s->has_crc = c.got;
    if (!s->soc[0] && strcasestr(s->prompt, "hisilicon")) snprintf(s->soc, sizeof s->soc, "HiSilicon");

    fl_lock();
    fl_st.chip_known = true;
    snprintf(fl_st.chip, sizeof fl_st.chip, "%.15s", s->soc[0] ? s->soc : "U-Boot");
    fl_st.flash_size = s->flash_size;
    snprintf(fl_st.boot, sizeof fl_st.boot, "%.20s, %u", s->ver, (unsigned)s->baud);
    fl_st.baud = s->baud;
    fl_st.lines = s->lines;
    fl_unlock();
    char fl[16] = "?";
    if (s->flash_size) snprintf(fl, sizeof fl, "%u MB", (unsigned)(s->flash_size >> 20));
    fl_logf(L_OK, "%s, %s: flash %s %s", s->soc[0] ? s->soc : "?", s->ver, fl, s->flash_name);
    if (s->flash_id[0]) fl_logf(L_INFO, "ID %.60s", s->flash_id);
    if (s->mtdparts[0]) fl_logf(L_INFO, "mtdparts %.80s", s->mtdparts);
    fl_logf(L_INFO, _("Prompt \"%s\", RAM de trabajo 0x%08X, %s"), s->prompt, (unsigned)s->ram,
            s->has_crc ? _("con crc32") : _("sin crc32: cada pedazo se lee dos veces"));
    if (!s->probed) fl_logf(L_WARN, "%s", _("sf probe 0 no anduvo: la flash no se puede leer por acá"));
    return true;
}

/* -------------------------------------------------------------------------- */
/* Speed                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct { bool press, refused; } baud_ans_t;

static void baud_line(ub_t *s, const char *l, void *ctx)
{
    baud_ans_t *a = ctx;
    any_line(s, l, NULL);
    if (strstr(l, "press ENTER") || strstr(l, "Switch baudrate")) a->press = true;
    if (strstr(l, "not supported") || strstr(l, "Unknown")) a->refused = true;
}

/* U-Boot to 'b' and this end with it: false if it said no (the console as
 * it was), fail if it went and does not answer there */
static bool switch_to(ub_t *s, uint32_t b, bool *lost)
{
    char cmd[40];
    snprintf(cmd, sizeof cmd, "setenv baudrate %u", (unsigned)b);
    pump(s, 30, 0, NULL, NULL);
    s->lines_in = 0;
    if (!tx(s, cmd, (int)strlen(cmd)) || !tx(s, "\r", 1)) return false;
    baud_ans_t a = { false, false };
    uint64_t end = aos_hal_uptime_ms() + 1500;
    while (!fl_cancel && aos_hal_uptime_ms() < end && !a.press) {
        pump(s, 20, 0, baud_line, &a);
        if (a.refused || (s->lines_in && at_prompt(s))) return false;   /* said no, or took it with no handshake */
    }
    if (!a.press) return false;
    aos_hal_sleep_ms(120);
    aos_io_uart_set_baud(s->u, b);
    for (int k = 0; k < 4 && !fl_cancel; k++) {
        aos_hal_sleep_ms(60);
        s->lines_in = 0;
        tx(s, "\r", 1);
        pump(s, 400, 150, NULL, NULL);
        if (at_prompt(s)) {
            s->baud = b;
            return true;
        }
    }
    *lost = true;
    return false;
}

static bool go_fast(ub_t *s)
{
    if (J.opts.baud && J.opts.baud <= s->baud) return true;
    static const uint32_t TRY[] = { 921600, 460800, 230400 };
    for (size_t i = 0; i < sizeof TRY / sizeof TRY[0]; i++) {
        uint32_t b = J.opts.baud ? J.opts.baud : TRY[i];
        bool lost = false;
        if (switch_to(s, b, &lost)) {
            fl_logf(L_OK, _("Consola a %u baudios (sin saveenv: al reiniciar vuelve a %u)"), (unsigned)b, (unsigned)s->base_baud);
            fl_lock();
            fl_st.baud = b;
            fl_unlock();
            return true;
        }
        if (lost) return fl_fail(_("La consola pasó a %u baudios y no contesta ahí: reiniciá el aparato"), (unsigned)b);
        if (J.opts.baud) break;
    }
    fl_logf(L_INFO, _("La consola se queda a %u baudios"), (unsigned)s->baud);
    return true;
}

static void back_to_base(ub_t *s)
{
    if (s->baud == s->base_baud) return;
    bool lost = false;
    if (switch_to(s, s->base_baud, &lost)) fl_logf(L_INFO, _("Consola de vuelta a %u baudios"), (unsigned)s->base_baud);
}

/* -------------------------------------------------------------------------- */
/* Reading                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;                   /* where the RAM at 'base' goes */
    uint32_t base, n;
    uint32_t got;                   /* words taken */
} md_t;

/* "82000000: 56190527 00000000 4dc0593f 50677bd2    'V....." - a line that
 * is not that, or out of the range, is not taken */
static void md_line(ub_t *s, const char *l, void *ctx)
{
    (void)s;
    md_t *m = ctx;
    char *e;
    unsigned long addr = strtoul(l, &e, 16);
    if (e - l != 8 || *e != ':' || (addr & 15) || addr < m->base || addr >= m->base + m->n) return;
    uint32_t w[4];
    const char *p = e + 1;
    int k = 0;
    for (; k < 4; k++) {
        if (p[0] != ' ' || !isxdigit((unsigned char)p[1])) break;
        char *q;
        w[k] = (uint32_t)strtoul(p + 1, &q, 16);
        if (q - (p + 1) != 8) return;
        p = q;
    }
    if (k != 4 && addr + 4u * (uint32_t)k != m->base + m->n) return;     /* short only at the end */
    for (int i = 0; i < k; i++) {
        uint8_t *d = m->buf + (addr - m->base) + 4 * i;
        d[0] = (uint8_t)w[i];
        d[1] = (uint8_t)(w[i] >> 8);
        d[2] = (uint8_t)(w[i] >> 16);
        d[3] = (uint8_t)(w[i] >> 24);
    }
    m->got += (uint32_t)k;
}

/* The RAM at ram+off, n bytes, into buf+off */
static bool md_read(ub_t *s, uint8_t *buf, uint32_t off, uint32_t n)
{
    md_t m = { buf + off, s->ram + off, n, 0 };
    char cmd[48];
    snprintf(cmd, sizeof cmd, "md.l 0x%x 0x%x", (unsigned)(s->ram + off), (unsigned)(n / 4));
    return run_cmd(s, cmd, 3000, md_line, &m);
}

/* U-Boot's CRC-32 of the RAM at ram+off */
static bool crc_of(ub_t *s, uint32_t off, uint32_t n, uint32_t *out)
{
    char cmd[48];
    crc_t c = { 0, false, false };
    snprintf(cmd, sizeof cmd, "crc32 0x%x 0x%x", (unsigned)(s->ram + off), (unsigned)n);
    if (!run_cmd(s, cmd, 5000, crc_line, &c) || !c.got) return false;
    *out = c.crc;
    return true;
}

static void sf_line(ub_t *s, const char *l, void *ctx)
{
    (void)s;
    bool *bad = ctx;
    if (strstr(l, "ERROR") || strstr(l, "rror") || strstr(l, "Unknown") || strstr(l, "No SPI") || strstr(l, "Usage")) *bad = true;
}

#define PART 4096                    /* what is printed again when a piece's CRC is wrong */

/* One piece, read until it is right (three rounds at most) */
static bool read_piece(ub_t *s, uint32_t off, uint32_t n)
{
    char cmd[64];
    for (int t = 0; t < 3 && !fl_cancel; t++) {
        if (t) fl_logf(L_WARN, _("0x%X: otra vez (%d)"), (unsigned)off, t + 1);
        bool bad = false;
        snprintf(cmd, sizeof cmd, "sf read 0x%x 0x%x 0x%x", (unsigned)s->ram, (unsigned)off, (unsigned)n);
        if (!run_cmd(s, cmd, 5000, sf_line, &bad) || bad) continue;
        if (!s->has_crc) {
            /* twice, and the same both times */
            if (md_read(s, s->a, 0, n) && md_read(s, s->b, 0, n) && !memcmp(s->a, s->b, n)) return true;
            continue;
        }
        uint32_t want;
        if (!crc_of(s, 0, n, &want)) continue;
        if (want == crc32_ff(n)) {
            memset(s->a, 0xFF, n);
            return true;
        }
        memset(s->a, 0, n);
        md_read(s, s->a, 0, n);
        if (crc32_add(0, s->a, n) == want) return true;
        /* which 4 KB came wrong, and those again */
        uint32_t redone = 0;
        for (uint32_t o = 0; o < n && !fl_cancel; o += PART) {
            uint32_t m = n - o < PART ? n - o : PART, w;
            if (!crc_of(s, o, m, &w)) break;
            for (int k = 0; k < 4 && crc32_add(0, s->a + o, m) != w && !fl_cancel; k++, redone++) md_read(s, s->a, o, m);
        }
        if (redone) fl_logf(L_INFO, _("0x%X: %u pedazos de 4 KB repetidos"), (unsigned)off, (unsigned)redone);
        if (crc32_add(0, s->a, n) == want) return true;
    }
    return false;
}

static bool write_txt(ub_t *s, const char *bin_path)
{
    char path[200];
    if (bin_path && bin_path[0]) {
        size_t k = strlen(bin_path);
        snprintf(path, sizeof path, "%.*s.txt", (int)(k > 4 ? k - 4 : k), bin_path);
    } else if (!fl_dump_note_path(path, sizeof path, "-consola.txt")) {
        return false;
    }
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# U-Boot por consola en %s a %u baudios: el arranque y lo que contestaron version, printenv, bdinfo, sf probe 0\n",
            J.port, (unsigned)s->base_baud);
    fwrite(s->log, 1, s->log_n, f);
    fclose(f);
    const char *b = strrchr(path, '/');
    fl_logf(L_INFO, _("El registro de la consola: %s"), b ? b + 1 : path);
    return true;
}

/* a value for the .json: quotes and backslashes out */
static void json_clean(char *out, size_t cap, const char *in)
{
    size_t k = 0;
    for (; *in && k + 1 < cap; in++) out[k++] = *in == '"' || *in == '\\' || (unsigned char)*in < 0x20 ? '\'' : *in;
    out[k] = 0;
}

static bool read_flash(ub_t *s)
{
    if (!s->probed) return fl_fail("%s", _("Sin sf probe no se puede leer la flash por U-Boot"));
    uint32_t from = J.opts.read_from, len = J.opts.read_len;
    if (!s->flash_size && !len) return fl_fail("%s", _("No se sabe el tamaño de la flash: indicá cuánto leer"));
    if (s->flash_size && from >= s->flash_size)
        return fl_fail(_("0x%X queda fuera de la flash (%u MB)"), (unsigned)from, (unsigned)(s->flash_size >> 20));
    if (!len || (s->flash_size && len > s->flash_size - from)) len = s->flash_size - from;
    if (!go_fast(s)) return false;
    if (!fl_dump_open(from, len)) return false;
    fl_phase(AOS_FLASHER_READING);
    s->logging = false;             /* md's output is not the log's */
    uint32_t blank = 0;
    for (uint32_t o = 0; o < len && !fl_cancel; o += CHUNK) {
        uint32_t n = len - o < CHUNK ? len - o : CHUNK;
        uint64_t t0 = aos_hal_uptime_ms();
        bool ok = read_piece(s, from + o, n);
        J.io_ms += aos_hal_uptime_ms() - t0;
        J.io_bytes += n;
        if (!ok) {
            fl_dump_close(false, false, NULL);
            return fl_cancel ? false : fl_fail(_("0x%X: no salió bien en tres intentos"), (unsigned)(from + o));
        }
        if (s->a[0] == 0xFF && s->a[n - 1] == 0xFF && crc32_add(0, s->a, n) == crc32_ff(n)) blank++;
        if (!fl_dump_write(s->a, n)) { fl_dump_close(false, false, NULL); return false; }
    }
    s->logging = true;
    if (fl_cancel) { fl_dump_close(false, false, NULL); return false; }
    fl_logf(L_INFO, _("%u pedazos de 64 KB vacíos (todo 0xFF), sin imprimir"), (unsigned)blank);
    char *extra = malloc(1024);
    if (extra) {
        char m[240], b[480], v[64], fn[32], id[40];
        json_clean(m, sizeof m, s->mtdparts);
        json_clean(b, sizeof b, s->bootargs);
        json_clean(v, sizeof v, s->ver);
        json_clean(fn, sizeof fn, s->flash_name);
        json_clean(id, sizeof id, s->flash_id);
        snprintf(extra, 1024, "\"family\":\"uboot\",\"uboot\":\"%s\",\"flash\":\"%s\",\"flash_id\":\"%s\",\"mtdparts\":\"%s\","
                 "\"bootargs\":\"%s\",\"ram\":\"0x%08x\",\"crc32\":%s", v, fn, id, m, b, (unsigned)s->ram, s->has_crc ? "true" : "false");
    }
    fl_dump_close(true, true, extra);
    free(extra);
    write_txt(s, fl_st.out_path);
    return true;
}

/* -------------------------------------------------------------------------- */
/* The job                                                                     */
/* -------------------------------------------------------------------------- */

static bool run(ub_t *s)
{
    if (J.job != AOS_FLASHER_JOB_DETECT && J.job != AOS_FLASHER_JOB_READ)
        return fl_fail("%s", _("Por la consola de U-Boot sólo se lee: no se graba ni se borra"));
    fl_phase(AOS_FLASHER_OPENING);
    const aos_io_port_t *pt = aos_io_port_find(J.port);
    s->usb = !strncmp(J.port, "usb", 3);
    if (!s->usb && (!pt || pt->kind != AOS_PORT_UART)) return fl_fail(_("No hay un puerto serie %s"), J.port);
    s->base_baud = s->baud = J.opts.console_baud ? J.opts.console_baud : 115200;
    s->u = aos_io_uart_open(J.port, s->baud, false, AOS_FLASHER_OWNER);
    for (int i = 0; !s->u && s->usb && i < 30 && !fl_cancel; i++) {
        aos_hal_sleep_ms(100);
        s->u = aos_io_uart_open(J.port, s->baud, false, AOS_FLASHER_OWNER);
    }
    if (!s->u) {
        if (s->usb) return fl_fail(_("No se pudo abrir %s: ¿está enchufado, o abierto en la Terminal?"), J.port);
        const char *o = aos_io_owner(pt->pins[0]);
        if (!o) o = aos_io_owner(pt->pins[1]);
        return o ? fl_fail(_("%s está ocupado: lo tiene %s"), J.port, o) : fl_fail(_("No se pudo abrir %s"), J.port);
    }
    s->lines = !s->usb && aos_io_uart_lines(s->u, -1, -1);
    fl_logf(L_INFO, _("%s a %u baudios"), aos_io_uart_desc(s->u), (unsigned)s->baud);
    s->logging = true;
    if (!stop_autoboot(s) || !read_info(s)) {
        if (s->log_n && !fl_cancel) write_txt(s, NULL);
        return false;
    }
    if (J.job == AOS_FLASHER_JOB_DETECT) {
        write_txt(s, NULL);
        return true;
    }
    return read_flash(s);
}

bool fl_uboot_run(void)
{
    ub_t *s = calloc(1, sizeof *s);
    if (!s) return fl_fail("%s", _("Sin memoria"));
    s->log = malloc(LOG_CAP);
    s->a = malloc(CHUNK);
    s->b = malloc(CHUNK);
    bool ok = s->log && s->a && s->b ? run(s) : fl_fail("%s", _("Sin memoria"));
    if (s->u) {
        bool cancelled = fl_cancel;
        fl_cancel = false;
        if (s->prompt[0]) {
            back_to_base(s);
            fl_logf(L_INFO, _("Queda frenado en \"%s\": reset en la Terminal, o su corriente, lo arranca"), s->prompt);
        }
        fl_cancel = cancelled;
        aos_io_uart_close(s->u);
    }
    free(s->log);
    free(s->a);
    free(s->b);
    free(s);
    return ok;
}
