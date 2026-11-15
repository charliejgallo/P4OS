/*
 * P4OS - Logic: analysers built on a Cypress FX2 / FX2LP, on the USB host.
 *
 * The "Logic Analyzer 24MHz 8CH" boxes sold everywhere are a CY7C68013A
 * with a buffer in front: a copy of the first Saleae Logic, with its USB
 * ids (0925:3881). The chip has no program of its own; whoever drives it
 * loads one into its RAM at every plug-in. This app loads fx2lafw, sigrok's
 * open firmware (GPL), the same that PulseView loads, so the box behaves
 * as with PulseView. The firmware is not in the app (it is someone else's
 * program, under its own licence): it goes on the card, in
 * <card>/logica/fx2lafw/, from sigrok's "sigrok-firmware-fx2lafw" files
 * (README.md says where; the portal's page takes the file).
 *
 * How the chip is driven (what sigrok's fx2lafw driver does, written anew
 * for the board's raw USB):
 *
 *   loading     vendor request 0xA0 (the chip's own, in silicon) writes its
 *               RAM: CPUCS (0xE600) = 1 holds the 8051 in reset, the image
 *               goes in at address 0, CPUCS = 0 runs it. fx2lafw then drops
 *               off the bus and comes back as a new device, with the same
 *               ids, "sigrok" and "fx2lafw" as its names.
 *   version     vendor request 0xB0 IN, two bytes (major, minor): major 1.
 *   start       vendor request 0xB1 OUT, three bytes: flags (bit 6: the
 *               48 MHz clock, else 30 MHz; bit 5: 16 bits wide), and the
 *               delay between samples in clock ticks less one, big end
 *               first. 24 MHz is 48 MHz / 2, delay 1.
 *   samples     bulk IN on endpoint 0x82, a byte per sample (D0 = bit 0),
 *               until the transfers stop being collected. There is no stop
 *               command: the next start resets the chip's FIFOs.
 *
 * The box only streams: the depth of a capture is the board's PSRAM. At
 * 24 MHz that is 24 MB/s through the host, the ring and into the capture,
 * all in PSRAM; what does not keep up is dropped by the USB stream a whole
 * transfer at a time, counted, and said ("con huecos"). docs: README.md.
 */
#include "la.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FX2_EP_IN     0x82
#define FX2_CPUCS     0xE600
#define FX2_REQ_RAM   0xA0
#define CMD_FW_VER    0xB0
#define CMD_START     0xB1
#define CMD_REVID     0xB2
#define FLAG_48MHZ    0x40
#define MAX_DELAY     (6 * 256)
#define OWNER         "Lógica"

typedef struct {
    uint16_t vid, pid;
    const char *name;
    const char *fw;
} fx2_prof_t;

/* sigrok's list (libsigrok's fx2lafw driver), with the file each one takes;
 * the names as the boxes say them, the same in every language */
static const fx2_prof_t PROF[] = {
    { 0x0925, 0x3881, "Logic 24 MHz 8CH (Saleae)",       "fx2lafw-saleae-logic.fw" },
    { 0x04B4, 0x8613, "Cypress FX2LP",                   "fx2lafw-cypress-fx2.fw" },
    { 0x1D50, 0x608C, "sigrok FX2 LA 8CH",               "fx2lafw-sigrok-fx2-8ch.fw" },
    { 0x1D50, 0x608D, "sigrok FX2 LA 16CH",              "fx2lafw-sigrok-fx2-16ch.fw" },
    { 0x1D50, 0x608F, "usb-c-grok",                      "fx2lafw-usb-c-grok.fw" },
    { 0x08A9, 0x0014, "CWAV USBee AX",                   "fx2lafw-cwav-usbeeax.fw" },
    { 0x08A9, 0x0015, "CWAV USBee DX",                   "fx2lafw-cwav-usbeedx.fw" },
    { 0x08A9, 0x0009, "CWAV USBee SX",                   "fx2lafw-cwav-usbeesx.fw" },
    { 0x08A9, 0x0005, "CWAV USBee ZX",                   "fx2lafw-cwav-usbeezx.fw" },
    { 0x16D0, 0x0498, "Braintechnology USB-LPS",         "fx2lafw-braintechnology-usb-lps.fw" },
};
#define NPROF ((int)(sizeof PROF / sizeof PROF[0]))

static int prof_of(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < NPROF; i++)
        if (PROF[i].vid == vid && PROF[i].pid == pid) return i;
    return -1;
}

static bool has(const char *s, const char *w)
{
    for (; *s; s++) {
        size_t i = 0;
        while (w[i] && s[i] && (s[i] | 0x20) == (w[i] | 0x20)) i++;
        if (!w[i]) return true;
    }
    return false;
}

static bool runs_fx2lafw(const aos_usb_dev_t *d)
{
    return has(d->vendor, "sigrok") && has(d->product, "fx2lafw");
}

const char *la_fx2_fw_dir(void)
{
    static char p[96];
    const char *d = la_dir();
    if (!d) return NULL;
    snprintf(p, sizeof p, "%s/fx2lafw", d);
    return p;
}

/* the firmware file for a profile, in fx2lafw/ or in logica/ itself; also
 * an Intel HEX of it (.ihx, .hex), as sigrok's sources build it */
static bool fw_path(int prof, char *out, size_t n)
{
    const char *base = la_dir();
    if (!base) return false;
    const char *dirs[2] = { la_fx2_fw_dir(), base };
    char stem[64];
    snprintf(stem, sizeof stem, "%s", PROF[prof].fw);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    static const char *const ext[] = { ".fw", ".ihx", ".hex" };
    for (int i = 0; i < 2; i++)
        for (int e = 0; e < 3; e++) {
            snprintf(out, n, "%s/%s%s", dirs[i], stem, ext[e]);
            FILE *f = fopen(out, "rb");
            if (f) {
                fclose(f);
                return true;
            }
        }
    return false;
}

int la_fx2_find(la_dev_t *out, int max)
{
    aos_usb_dev_t *d = malloc(16 * sizeof *d);
    if (!d) return 0;
    int n = aos_hal_usb_devices(d, 16), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        int p = prof_of(d[i].vid, d[i].pid);
        if (p < 0) continue;
        la_dev_t *o = &out[k++];
        memset(o, 0, sizeof *o);
        o->drv = LA_DRV_FX2;
        o->prof = (int8_t)p;
        o->usb_addr = d[i].addr;
        o->vid = d[i].vid;
        o->pid = d[i].pid;
        o->ready = runs_fx2lafw(&d[i]);
        o->channels = 8;
        o->max_samples = 0;
        /* at Full Speed (pins 21/23) bulk carries about 1 MB/s */
        o->max_rate = d[i].speed == 2 ? 24000000 : 1000000;
        snprintf(o->name, sizeof o->name, "%s", PROF[p].name);
        char fw[128];
        const char *speed = d[i].speed == 2 ? "USB 2.0 HS" : _("USB de 12 Mbit/s, hasta 1 MHz");
        if (o->ready) snprintf(o->detail, sizeof o->detail, _("fx2lafw cargado · %s"), speed);
        else if (fw_path(p, fw, sizeof fw)) snprintf(o->detail, sizeof o->detail, _("se le carga fx2lafw · %s"), speed);
        else snprintf(o->detail, sizeof o->detail, _("falta %.40s en la tarjeta"), PROF[p].fw);
    }
    free(d);
    return k;
}

/* ---- loading the firmware ---- */

static bool ram_write(aos_usb_raw_t *u, uint16_t addr, const uint8_t *p, int len)
{
    while (len > 0) {
        int c = len > 1024 ? 1024 : len;
        if (aos_hal_usb_raw_control(u, 0x40, FX2_REQ_RAM, addr, 0, (void *)p, (uint16_t)c, 1000) != c) return false;
        addr += c;
        p += c;
        len -= c;
    }
    return true;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* an Intel HEX file into the chip's 16 KB of code and data RAM */
static int ihex_parse(const char *txt, size_t len, uint8_t *img, int cap)
{
    int top = 0;
    const char *p = txt, *end = txt + len;
    while (p < end) {
        while (p < end && *p != ':') p++;
        if (p >= end) break;
        p++;
        uint8_t rec[260];
        int n = 0;
        while (p + 1 < end && hexv(p[0]) >= 0 && hexv(p[1]) >= 0 && n < (int)sizeof rec) {
            rec[n++] = (uint8_t)(hexv(p[0]) << 4 | hexv(p[1]));
            p += 2;
        }
        if (n < 5 || n != rec[0] + 5) return -1;
        uint8_t sum = 0;
        for (int i = 0; i < n; i++) sum += rec[i];
        if (sum) return -1;
        int addr = rec[1] << 8 | rec[2];
        if (rec[3] == 1) break;
        if (rec[3] != 0) continue;
        if (addr + rec[0] > cap) return -1;
        memcpy(img + addr, rec + 4, rec[0]);
        if (addr + rec[0] > top) top = addr + rec[0];
    }
    return top;
}

static const char *upload(const la_dev_t *d)
{
    char path[128];
    if (!fw_path(d->prof, path, sizeof path)) {
        static char why[96];
        snprintf(why, sizeof why, _("Falta %s en la carpeta logica/fx2lafw de la tarjeta"), PROF[d->prof].fw);
        return why;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return _("No se pudo leer el firmware");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) {
        fclose(f);
        return _("El archivo del firmware no parece fx2lafw");
    }
    uint8_t *raw = la_big_alloc((size_t)sz + 1);
    uint8_t *img = la_big_alloc(0x4000);
    if (!raw || !img) {
        fclose(f);
        la_big_free(raw);
        la_big_free(img);
        return _("No hay memoria");
    }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    int len;
    if (got && raw[0] == ':') {
        memset(img, 0, 0x4000);
        len = ihex_parse((const char *)raw, got, img, 0x4000);
    } else {
        len = got > 0x4000 ? -1 : (int)got;
        if (len > 0) memcpy(img, raw, (size_t)len);
    }
    la_big_free(raw);
    if (len <= 0) {
        la_big_free(img);
        return _("El archivo del firmware no parece fx2lafw");
    }
    aos_usb_raw_t *u = aos_hal_usb_raw_open(d->usb_addr, OWNER);
    if (!u) {
        la_big_free(img);
        return _("El analizador está ocupado o se desenchufó");
    }
    uint8_t one = 1, zero = 0;
    bool ok = aos_hal_usb_raw_control(u, 0x40, FX2_REQ_RAM, FX2_CPUCS, 0, &one, 1, 1000) == 1 &&
              ram_write(u, 0, img, len) &&
              aos_hal_usb_raw_control(u, 0x40, FX2_REQ_RAM, FX2_CPUCS, 0, &zero, 1, 1000) >= 0;
    aos_hal_usb_raw_close(u);
    la_big_free(img);
    aos_hal_log("logica", "fx2: %s, %d bytes to %02x: %s", path, len, d->usb_addr, ok ? "ok" : "failed");
    return ok ? NULL : _("El analizador no aceptó el firmware");
}

/* does fx2lafw run at this address? The names say so ("sigrok",
 * "fx2lafw"), and on the board that is all that is asked: the bare chip
 * does not answer fx2lafw's version request (0xB0) at all, and the board's
 * host keeps such a transfer out, after which the device cannot be opened
 * again until it is plugged anew (seen on the board, 2026-11-14). The
 * simulator does not read names from the Mac, so there it asks. */
static bool fx2_answers(uint8_t addr)
{
#ifndef AOS_SIM
    bool named = false;
    aos_usb_dev_t *l = malloc(16 * sizeof *l);
    if (l) {
        int n = aos_hal_usb_devices(l, 16);
        for (int i = 0; i < n; i++)
            if (l[i].addr == addr) named = runs_fx2lafw(&l[i]);
        free(l);
    }
    if (!named) return false;
#endif
    aos_usb_raw_t *u = aos_hal_usb_raw_open(addr, OWNER);
    if (!u) return false;
    uint8_t ver[2] = { 0 };
    bool ok = aos_hal_usb_raw_control(u, 0xC0, CMD_FW_VER, 0, 0, ver, 2, 300) == 2 && ver[0] == 1;
    aos_hal_usb_raw_close(u);
    return ok;
}

/* after the upload the chip drops off the bus and comes back as a new
 * device with the same ids: wait for one that answers */
static bool wait_renum(la_dev_t *d)
{
    aos_usb_dev_t *l = malloc(16 * sizeof *l);
    if (!l) return false;
    bool found = false;
    aos_hal_sleep_ms(800);
    /* up to 15 s: a computer may ask its user to allow the "new" device
     * (macOS does, which is how the simulator meets it) */
    for (int t = 0; t < 100 && !found && !LA.stop; t++) {
        aos_hal_sleep_ms(150);
        int n = aos_hal_usb_devices(l, 16);
        for (int i = 0; i < n && !found; i++)
            if (l[i].vid == d->vid && l[i].pid == d->pid && fx2_answers(l[i].addr)) {
                aos_hal_log("logica", "fx2: back at %u after %d ms", l[i].addr, 800 + 150 * (t + 1));
                d->usb_addr = l[i].addr;
                d->ready = true;
                found = true;
            }
    }
    free(l);
    return found;
}

/* ---- capturing ---- */

/* the stream in flight first, then fx2lafw's start (which also empties its
 * FIFOs): a restart after a failed transfer is the same, with the endpoint's
 * halt cleared in between */
static bool fx2_go(aos_usb_raw_t *u, int xfer, int nx, int ring, const uint8_t *cmd)
{
    if (!aos_hal_usb_raw_stream_start(u, FX2_EP_IN, xfer, nx, ring)) {
        aos_hal_log("logica", "fx2: start: the stream did not start");
        return false;
    }
    int r = aos_hal_usb_raw_control(u, 0x40, CMD_START, 0, 0, (void *)cmd, 3, 500);
    if (r == 3) return true;
    aos_hal_log("logica", "fx2: start: the start command answered %d", r);
    aos_hal_usb_raw_stream_stop(u);
    return false;
}

static void set_state(int st, uint32_t prog)
{
    la_lock();
    LA.state = st;
    LA.prog = prog;
    la_unlock();
}

/* finds the analyser again after it dropped off the bus (same ids, any
 * address): up to five seconds */
static bool fx2_find_again(la_dev_t *d)
{
    aos_usb_dev_t *l = malloc(16 * sizeof *l);
    if (!l) return false;
    bool found = false;
    for (int t = 0; t < 25 && !found && !LA.stop; t++) {
        aos_hal_sleep_ms(200);
        int n = aos_hal_usb_devices(l, 16);
        for (int i = 0; i < n; i++)
            if (l[i].vid == d->vid && l[i].pid == d->pid) {
                d->usb_addr = l[i].addr;
                d->ready = runs_fx2lafw(&l[i]);
                found = true;
                break;
            }
    }
    free(l);
    return found;
}

/* fx2lafw in the chip (loading it if it is not), the device open, its
 * version checked and its interface claimed */
static const char *fx2_connect(la_dev_t *d, aos_usb_raw_t **out)
{
    *out = NULL;
    if (!d->ready && fx2_answers(d->usb_addr)) d->ready = true;   /* loaded earlier, named or not */
    if (!d->ready) {
        set_state(LA_ST_CONNECT, 0);
        const char *why = upload(d);
        if (why) return why;
        if (!wait_renum(d)) return _("El analizador no volvió después de cargarle fx2lafw");
        aos_hal_sleep_ms(300);
    }
    aos_usb_raw_t *u = aos_hal_usb_raw_open(d->usb_addr, OWNER);
    if (!u) return _("El analizador está ocupado o se desenchufó");
    uint8_t ver[2] = { 0 };
    if (aos_hal_usb_raw_control(u, 0xC0, CMD_FW_VER, 0, 0, ver, 2, 500) != 2) {
        aos_hal_usb_raw_close(u);
        return _("El analizador no contesta: desenchufalo y enchufalo otra vez");
    }
    if (ver[0] != 1) {
        aos_hal_usb_raw_close(u);
        return _("Esa versión de fx2lafw no es compatible (hace falta la 1.x)");
    }
    if (!aos_hal_usb_raw_claim(u, 0, 0)) {
        aos_hal_usb_raw_close(u);
        return _("No se pudo tomar el analizador");
    }
    /* The endpoint's data toggle: the claim gives the host a new pipe, at
     * DATA0, but the chip keeps its own across a new configuration (it
     * stays powered, with fx2lafw, when the board restarts), and the first
     * packet then fails and halts the pipe (seen on the board, 2026-11-14).
     * CLEAR_FEATURE(ENDPOINT_HALT) puts the chip's back to DATA0. */
    int h = aos_hal_usb_raw_control(u, 0x02, 0x01 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */, FX2_EP_IN, NULL, 0, 300);
    aos_hal_log("logica", "fx2: fx2lafw %u.%u at %u (endpoint cleared: %d)", ver[0], ver[1], d->usb_addr, h);
    *out = u;
    return NULL;
}

/* The board's host, after the box dropped off the bus and came back with
 * the host running (and after fx2lafw is loaded, which is the same),
 * failed every bulk transfer at once, on new pipes too, until the host
 * itself was started again (2026-11-14; asked of the firmware). The app
 * does that, once a capture, only when the box is all there is on the
 * host: a pendrive would be unmounted under its user. */
static bool host_alone(const la_dev_t *d)
{
    aos_usb_dev_t *l = malloc(16 * sizeof *l);
    if (!l) return false;
    int n = aos_hal_usb_devices(l, 16), mine = 0;
    for (int i = 0; i < n; i++)
        if (l[i].vid == d->vid && l[i].pid == d->pid) mine++;
    free(l);
    return n == mine;
}

static bool host_restart(void)
{
    aos_hal_log("logica", "fx2: the host refuses every transfer: starting it again");
    if (!aos_hal_usb_host_set(false)) return false;
    aos_hal_sleep_ms(1500);
    return aos_hal_usb_host_set(true);
}

const char *la_fx2_capture(const la_dev_t *dev, const la_cfg_t *cfg, la_cap_t *c)
{
    la_dev_t d = *dev;
    uint32_t rate = cfg->rate;
    if (rate > d.max_rate) rate = d.max_rate;
    uint32_t delay;
    uint8_t flags;
    if (48000000 % rate == 0 && 48000000 / rate - 1 <= MAX_DELAY) {
        flags = FLAG_48MHZ;
        delay = 48000000 / rate - 1;
    } else {
        flags = 0;
        delay = 30000000 / rate - 1;
        if (30000000 / rate < 1 || delay > MAX_DELAY) return _("El analizador no puede muestrear a esa velocidad");
    }
    aos_usb_raw_t *u;
    const char *cwhy = fx2_connect(&d, &u);
    if (cwhy) return cwhy;

    const uint32_t total = cfg->samples;
    if (!la_cap_alloc(c, total)) {
        aos_hal_usb_raw_close(u);
        return _("No hay memoria para tantas muestras");
    }
    c->n = 0;
    c->trig = -1;
    c->rate = rate;
    c->nch = 8;
    c->lost = 0;

    /* fx2lafw stops for good when its FIFO overflows: the host has to keep
     * transfers waiting for as long as it may look away. sigrok keeps
     * about half a second of them; the board's are internal RAM (the
     * host's DMA buffers), so it keeps fewer, of about 10 ms each, up to
     * 16 KB. Measured on a Mac (2026-11-14, a clone straight on a port):
     * 4 x 10 KB lost the stream within half a second at 1 MHz; 16 x 64 KB
     * took 8M samples at 24 MHz with no gap. That Mac also saw transfers
     * fail now and then and the box drop off the bus and come back: the
     * loop below starts it again. */
    int xfer = (int)(rate / 100);
    xfer = (xfer + 511) & ~511;
    if (xfer < 512) xfer = 512;
#ifdef AOS_SIM
    if (xfer > 65536) xfer = 65536;
    int nx = 16;
#else
    if (xfer > 16384) xfer = 16384;
    /* 6 x 16 KB of the board's internal RAM: 4 ms at 24 MHz, 24 ms at 4.
     * Waiting a long time for a trigger at 4 MHz with 4 of them, the host
     * looked away for longer every few minutes and fx2lafw stopped (the
     * loop starts it again, but every time the samples before the trigger
     * start over) */
    int nx = 6;
#endif
    /* the host's ring, PSRAM: 80 ms at 24 MB/s (la_max_samples leaves room
     * for it) */
    int ring = rate >= 8000000 ? 2 * 1024 * 1024 : 1024 * 1024;
    uint8_t cmd[3] = { flags, (uint8_t)(delay >> 8), (uint8_t)delay };
    if (!fx2_go(u, xfer, nx, ring, cmd)) {
        aos_hal_usb_raw_close(u);
        return _("El analizador no arrancó");
    }
    aos_hal_log("logica", "fx2: %u Hz (flags %02x delay %u), %u samples, %d x %d",
                (unsigned)rate, flags, (unsigned)delay, (unsigned)total, nx, xfer);

    const bool trig = la_trig_any(cfg->trig);
    const uint32_t pre = trig ? (uint32_t)((uint64_t)total * cfg->pre / 100) : 0;
    uint64_t w = 0;                 /* samples received */
    uint32_t idx = 0;               /* where the next one goes */
    int64_t t_at = -1;              /* the trigger, in samples received */
    uint64_t end = total;           /* stop once w reaches it */
    uint8_t prev = 0;
    uint32_t quiet_ms = 0, errs_seen = 0;
    int restarts = 0, empty = 0;
    bool stalled = false, host_done = false;
    uint64_t w_at_start = 0;
    const char *why = NULL;
    /* nothing for this long and the stream is dead: four transfers' time,
     * at least 600 ms */
    uint32_t quiet_max = (uint32_t)((uint64_t)xfer * 4000 / rate);
    if (quiet_max < 600) quiet_max = 600;
    set_state(trig ? LA_ST_WAIT : LA_ST_CAPTURE, 0);

    /* until the trigger, for as long as it takes; after it, to the end */
    while ((trig && t_at < 0) || w < end) {
        if (LA.stop) break;
        uint32_t room = total - idx;
        /* once the end is known, not a sample past it: the buffer is a
         * ring, and those would land on the oldest ones kept, the ones
         * before the trigger (a whole transfer of them, measured) */
        if ((t_at >= 0 || !trig) && end - w < room) room = (uint32_t)(end - w);
        /* before it, no more than fits after a trigger found in this read */
        if (trig && t_at < 0 && total - pre < room) room = total - pre > 0 ? total - pre : 1;
        int got = aos_hal_usb_raw_stream_read(u, c->buf + idx, (int)(room > 65536 ? 65536 : room), 100);
        uint32_t errs = 0;
        bool gone = got == AOS_USB_RAW_GONE;
        if (got <= 0 && !gone) {
            quiet_ms += 100;
            aos_hal_usb_raw_stream_stats(u, NULL, NULL, &errs);
        }
        if (gone || (got <= 0 && (errs != errs_seen || quiet_ms >= quiet_max))) {
            /* A transfer failed (the host aborts the others with it), the
             * analyser dropped off the bus and came back, or nothing comes:
             * fx2lafw's FIFO overflowed meanwhile and it stopped. Before
             * the trigger a gap does not matter: start again from nothing.
             * After it, keep what came. */
            errs_seen = errs;
            empty = w == w_at_start ? empty + 1 : 0;
            /* before the trigger it goes on for as long as data keeps
             * coming between the restarts; 20 in a row with none is the end */
            if ((trig && t_at >= 0) || empty >= 20 || (!trig && restarts >= 20)) {
                if (gone) why = _("Se desenchufó el analizador");
                else stalled = true;
                break;
            }
            restarts++;
            /* a pipe the host halted stays halted once nothing is in
             * flight: the device is opened anew, which gives new pipes */
            aos_hal_usb_raw_stream_stop(u);
            bool was_gone = gone || aos_hal_usb_raw_gone(u);
            aos_hal_usb_raw_close(u);
            u = NULL;
            set_state(LA_ST_CONNECT, 0);
            if (empty >= 3 && !host_done) {
                host_done = true;
                if (!host_alone(&d) || !host_restart()) {
                    why = _("El host USB no le acepta transferencias al analizador: apagá y prendé el host (Ajustes, USB)");
                    break;
                }
                was_gone = true;
            }
            const char *rw = !was_gone || fx2_find_again(&d) ? fx2_connect(&d, &u) : _("Se desenchufó el analizador");
            if (rw) {
                why = rw;
                break;
            }
            if (was_gone) aos_hal_log("logica", "fx2: it dropped off the bus and came back at %u", d.usb_addr);
            if (!fx2_go(u, xfer, nx, ring, cmd)) {
                why = _("El analizador no arrancó");
                break;
            }
            set_state(trig ? LA_ST_WAIT : LA_ST_CAPTURE, 0);
            aos_hal_usb_raw_stream_stats(u, NULL, NULL, &errs_seen);
            w = 0;
            w_at_start = 0;
            idx = 0;
            t_at = -1;
            end = total;
            quiet_ms = 0;
            continue;
        }
        if (got <= 0) continue;
        quiet_ms = 0;
        /* the trigger counts once the samples to keep before it are in */
        if (trig && t_at < 0 && w + (uint32_t)got > pre) {
            uint32_t from = w >= pre ? 0 : (uint32_t)(pre - w);
            int32_t f = la_trig_find(cfg->trig, c->buf + idx, (uint32_t)got, from, w ? prev : c->buf[idx]);
            if (f >= 0) {
                t_at = (int64_t)w + f;
                end = (uint64_t)t_at + (total - pre);
                if (end < total) end = total;
                set_state(LA_ST_CAPTURE, 0);
            }
        }
        prev = c->buf[idx + got - 1];
        w += (uint32_t)got;
        idx += (uint32_t)got;
        if (idx >= total) idx = 0;
        la_lock();
        if (t_at >= 0 || !trig) LA.prog = (uint32_t)((w > end ? end : w) - (end - total));
        else LA.prog = (uint32_t)(w > total ? total : w);
        la_unlock();
    }
    if (restarts) aos_hal_log("logica", "fx2: the stream was started again %d times", restarts);
    uint64_t bytes = 0;
    uint32_t dropped = 0, errors = 0;
    if (u) {
        aos_hal_usb_raw_stream_stats(u, &bytes, &dropped, &errors);
        aos_hal_usb_raw_stream_stop(u);
        aos_hal_usb_raw_close(u);
    }
    aos_hal_log("logica", "fx2: %u samples, trigger %d, dropped %u, errors %u%s%s", (unsigned)(w > total ? total : w),
                (int)t_at, (unsigned)dropped, (unsigned)errors, LA.stop ? ", stopped" : "", stalled ? ", it stalled" : "");
    if (why) {
        c->n = 0;
        return why;
    }
    if (stalled && (!w || (trig && t_at < 0))) {
        c->n = 0;
        return w ? _("El analizador se frenó antes del disparo: el USB no dio abasto. Probá una velocidad menor")
                 : _("El analizador no manda muestras");
    }
    /* the window kept: the last `total` samples received (fewer if it was
     * stopped before filling) */
    uint64_t have = w > end ? end : w;
    if (have >= total) {
        c->n = total;
        la_rotate(c->buf, total, (uint32_t)(have % total));
        uint64_t first = have - total;
        c->trig = t_at >= 0 && (uint64_t)t_at >= first ? (int32_t)((uint64_t)t_at - first) : -1;
    } else {
        c->n = (uint32_t)have;
        c->trig = t_at >= 0 ? (int32_t)t_at : -1;
    }
    c->lost = dropped;
    c->stalled = stalled;
    snprintf(c->src, sizeof c->src, "%s", PROF[d.prof].name);
    return NULL;
}
