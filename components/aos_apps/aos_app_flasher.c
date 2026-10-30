/*
 * P4OS - Programador: flashes an ESP32-family board or an AVR Arduino through
 * the header or a USB port, or an Arm Cortex-M by SWD on the header's pins
 * (nRF52, RP2040, STM32F0/F1), whose RTT console it also opens.
 *
 * The work is the aos_flasher service's (components/aos_flasher): it owns the
 * port and the thread, so a flash started here goes on with the app closed,
 * and when it ends with the app out of sight a toast says so and the icon
 * gets a badge. This file is only the face of it:
 *
 *   the kind      ESP32, Arduino, SWD or STM32 (the service's family): what Detectar
 *                 looks for, which firmware the list shows, how to wire it.
 *   the target    the port and its EN/BOOT pins from modules.txt, the speed,
 *                 and "Detectar" (connect, read the chip, reset it back to
 *                 its program): chip, revision, flash size, MAC. If somebody
 *                 else holds the port - the Terminal, usually - it says who,
 *                 and for the Terminal offers to let it go.
 *   the firmware  what is on the card under /firmware: ESP-IDF build folders
 *                 (flasher_args.json, shown as a project with its files and
 *                 offsets) and loose .bin files; a chip that does not match
 *                 the detected one is marked before anyone taps Grabar.
 *   the tools     "Leer" copies the target's whole flash to the card (a dump,
 *                 listed afterwards with the firmware, to write back) and
 *                 "Borrar" erases it, after asking.
 *   the bridge    "Puente para la Mac": the port lent over TCP to esptool
 *                 (rfc2217, 4000) and avrdude (net:, 4001) on the computer,
 *                 until closed; the log says who connects.
 *   the console   SWD's RTT console: the target's lines in the log, and while
 *                 it is open the bar becomes a line to type (the screen's
 *                 keyboard) that goes down to the firmware with Enter.
 *   the probe     SWD's "Sonda para la computadora": the board as a CMSIS-DAP
 *                 probe on its OTG connector, for OpenOCD, pyOCD and
 *                 probe-rs, until closed; the commands and SWD transfers
 *                 counted, the log says when the debugger comes and goes.
 *   the job       a ring with the percentage, the phase, bytes, speed and
 *                 time left, each file with its state (done, going, waiting),
 *                 the service's log; at the end the time it took and whether
 *                 every file passed its MD5, and "Monitor", which hands the
 *                 port to the Terminal at 115200 to watch the new firmware
 *                 boot.
 *
 * Portrait is one scrolling column over a bar with the big button; landscape
 * puts the target (and, while flashing, the ring) on the left over the
 * button, and the list (or the files and the log) on the right. The state
 * that matters lives in S, so turning the screen rebuilds the same view.
 *
 * Not here yet (APPS.md): downloads of a release from a URL.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_io.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define APP_ID      "aos.flasher"
#define C_TINT      lv_color_hex(0x8B5CF6)      /* the app's colour, from its icon */
#define C_TINT_DEEP lv_color_hex(0x4C1D95)
#define C_LOG_BG    lv_color_hex(0x05070A)
#define BAR_H       148
#define SIDE_W      560

static const uint32_t BAUDS[] = { 115200, 230400, 460800, 921600 };
#define N_BAUDS (sizeof BAUDS / sizeof BAUDS[0])

static const char *const PHASE[] = {
    [AOS_FLASHER_IDLE] = N_("Listo para grabar"),
    [AOS_FLASHER_OPENING] = N_("Abriendo el puerto"),
    [AOS_FLASHER_CONNECTING] = N_("Conectando"),
    [AOS_FLASHER_STUB] = N_("Cargando el stub"),
    [AOS_FLASHER_BAUD] = N_("Subiendo la velocidad"),
    [AOS_FLASHER_INFO] = N_("Leyendo el chip"),
    [AOS_FLASHER_ERASING] = N_("Borrando"),
    [AOS_FLASHER_WRITING] = N_("Grabando"),
    [AOS_FLASHER_VERIFYING] = N_("Verificando"),
    [AOS_FLASHER_READING] = N_("Leyendo la flash"),
    [AOS_FLASHER_RESETTING] = N_("Reiniciando la placa"),
    [AOS_FLASHER_CONSOLE] = N_("Consola RTT abierta"),
    [AOS_FLASHER_BRIDGE] = N_("Puente abierto"),
    [AOS_FLASHER_PROBE] = N_("Sonda abierta"),
    [AOS_FLASHER_DONE] = N_("Grabación terminada"),
    [AOS_FLASHER_FAILED] = N_("No se pudo grabar"),
    [AOS_FLASHER_CANCELLED] = N_("Grabación cancelada"),
};

/* What survives closing the app and turning the screen */
static struct {
    char port[AOS_IO_PORT_NAME_MAX];
    int fam;                        /* aos_flasher_family_t */
    int baud_i;
    char sel[256];                  /* path of the chosen source */
    bool job_view;                  /* the job's screen instead of the setup */
    uint32_t log_from;              /* first log line of the job on screen */
    uint32_t seen_seq;              /* the last finished job the service tick told about */
    bool loaded;
    aos_flasher_source_t *src;      /* what the card has (malloc'd once) */
    int nsrc;
    bool scanned;
} S;

static struct {
    lv_obj_t *root;
    int32_t W, H;
    bool land;
    lv_timer_t *timer;
    /* target card */
    lv_obj_t *chip_name, *chip_info, *detect_btn, *detect_lbl, *busy_row, *busy_lbl, *busy_btn;
    lv_obj_t *port_lbl, *baud_lbl, *baud_chip;
    /* sources */
    lv_obj_t *list;
    /* job */
    lv_obj_t *arc, *pct, *mark, *ph_title, *ph_sub, *stat[3];
    lv_obj_t *file_state[AOS_FLASHER_FILES_MAX];
    lv_obj_t *log_col;
    uint32_t log_shown;
    /* actions */
    lv_obj_t *actions, *main_btn, *main_lbl;
    lv_obj_t *read_btn, *erase_btn;
    lv_obj_t *overlay;              /* the erase's confirmation */
    lv_obj_t *con_ta, *con_kb;      /* the console's line, and its keyboard */
    int32_t bar_y, bar_h;           /* where the actions bar sits, without the keyboard */
    uint32_t shown_seq;
    int shown_phase;
    bool shown_busy;
} U;

static void build(void);

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static void fmt_size(char *out, size_t n, uint32_t bytes)
{
    if (bytes < 1024 * 1024) snprintf(out, n, "%u KB", (unsigned)((bytes + 1023) / 1024));
    else snprintf(out, n, "%u,%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes % (1 << 20)) * 10 >> 20));
}

/* "193 / 945 KB", "0,4 / 1,3 MB": both in the total's unit */
static void fmt_pair(char *out, size_t n, uint32_t done, uint32_t total)
{
    if (total < 1024 * 1024)
        snprintf(out, n, "%u / %u KB", (unsigned)((done + 1023) / 1024), (unsigned)((total + 1023) / 1024));
    else
        snprintf(out, n, "%u,%u / %u,%u MB", (unsigned)(done >> 20), (unsigned)((done % (1 << 20)) * 10 >> 20),
                 (unsigned)(total >> 20), (unsigned)((total % (1 << 20)) * 10 >> 20));
}

static void fmt_time(char *out, size_t n, uint32_t ms)
{
    uint32_t s = (ms + 500) / 1000;
    snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* The phase's title, which at the end depends on what the job was */
static const char *phase_title(const aos_flasher_status_t *st)
{
    if (st->job == AOS_FLASHER_JOB_READ) {
        if (st->phase == AOS_FLASHER_DONE) return _("Volcado guardado");
        if (st->phase == AOS_FLASHER_FAILED) return _("No se pudo leer");
        if (st->phase == AOS_FLASHER_CANCELLED) return _("Lectura cancelada");
    } else if (st->job == AOS_FLASHER_JOB_BRIDGE) {
        if (st->phase == AOS_FLASHER_DONE || st->phase == AOS_FLASHER_CANCELLED) return _("Puente cerrado");
        if (st->phase == AOS_FLASHER_FAILED) return _("No se pudo abrir el puente");
    } else if (st->job == AOS_FLASHER_JOB_PROBE) {
        if (st->phase == AOS_FLASHER_DONE || st->phase == AOS_FLASHER_CANCELLED) return _("Sonda cerrada");
        if (st->phase == AOS_FLASHER_FAILED) return _("No se pudo abrir la sonda");
    } else if (st->job == AOS_FLASHER_JOB_CONSOLE) {
        if (st->phase == AOS_FLASHER_DONE || st->phase == AOS_FLASHER_CANCELLED) return _("Consola cerrada");
        if (st->phase == AOS_FLASHER_FAILED) return _("No se pudo abrir la consola");
    } else if (st->job == AOS_FLASHER_JOB_ERASE) {
        if (st->phase == AOS_FLASHER_DONE) return st->family == AOS_FLASHER_AVR ? _("Programa borrado") : _("Flash borrada");
        if (st->phase == AOS_FLASHER_FAILED) return _("No se pudo borrar");
        if (st->phase == AOS_FLASHER_CANCELLED) return _("Borrado cancelado");
    }
    return _(PHASE[st->phase]);
}

static aos_flasher_opts_t job_opts(void)
{
    aos_flasher_opts_t o = { .family = (aos_flasher_family_t)S.fam, .baud = BAUDS[S.baud_i] };
    return o;
}

static const aos_flasher_source_t *selected(void)
{
    for (int i = 0; i < S.nsrc; i++) if (!strcmp(S.src[i].path, S.sel)) return &S.src[i];
    return NULL;
}

static void scan(void)
{
    if (!S.src) S.src = calloc(AOS_FLASHER_SOURCES_MAX, sizeof *S.src);
    S.nsrc = S.src ? aos_flasher_scan(S.src, AOS_FLASHER_SOURCES_MAX) : 0;
    S.scanned = true;
    const aos_flasher_source_t *s = selected();
    if (!s || !aos_flasher_source_fits(s, S.fam)) S.sel[0] = 0;
}

/* Who holds the port's pins, if not the programmer; its friendly name. */
static const char *port_holder(char *friendly, size_t n)
{
    const aos_io_port_t *p = aos_io_port_find(S.port);
    if (!p) return NULL;
    const char *o = aos_io_owner(p->pins[0]);
    if (!o) o = aos_io_owner(p->pins[1]);
    if (!o || !strcmp(o, AOS_FLASHER_OWNER)) return NULL;
    if (!strncmp(o, "serial ", 7)) snprintf(friendly, n, "%s", _("Terminal"));
    else snprintf(friendly, n, "%s", o);
    return o;
}

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

static lv_obj_t *section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *t = aos_label(parent, title, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 24, 0);
    lv_obj_set_style_pad_top(t, 12, 0);
    return t;
}

static lv_obj_t *glyph_square(lv_obj_t *parent, const char *glyph, lv_color_t a, lv_color_t b, int32_t size)
{
    lv_obj_t *sq = box(parent, size, size);
    lv_obj_set_style_radius(sq, size * 26 / 100, 0);
    lv_obj_set_style_bg_color(sq, a, 0);
    lv_obj_set_style_bg_grad_color(sq, b, 0);
    lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_t *g = aos_label(sq, glyph, size >= 80 ? &aos_sym_44 : &aos_sym_28, lv_color_white());
    lv_obj_center(g);
    aos_make_decorative(sq);
    return sq;
}

/* A rounded pill button with an optional glyph. */
static lv_obj_t *pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_color_t fg,
                      lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 10);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, fg));
    lv_obj_t *l = aos_label(b, text, aos_font_small, fg);
    aos_make_decorative(l);
    if (label_out) *label_out = l;
    return b;
}

static lv_obj_t *hairline(lv_obj_t *parent)
{
    lv_obj_t *l = box(parent, lv_pct(100), 1);
    lv_obj_set_style_bg_color(l, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    return l;
}

/* -------------------------------------------------------------------------- */
/* The target                                                                  */
/* -------------------------------------------------------------------------- */

static void refresh_target(void)
{
    if (!U.chip_name) return;
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    bool detecting = st.busy && st.job == AOS_FLASHER_JOB_DETECT;
    bool avr = S.fam == AOS_FLASHER_AVR, swd = S.fam == AOS_FLASHER_SWD, stm = S.fam == AOS_FLASHER_STM32;
    char buf[128];
    if (st.chip_known && strcmp(st.port, S.port) == 0 && (int)st.family == S.fam && (avr || swd || stm)) {
        lv_label_set_text(U.chip_name, st.chip);
        char fl[16], pg[16];
        fmt_size(fl, sizeof fl, st.flash_size);
        if (st.page >= 1024) snprintf(pg, sizeof pg, "%u KB", st.page / 1024u);
        else snprintf(pg, sizeof pg, "%u B", st.page);
        if (st.secure) snprintf(buf, sizeof buf, "%s\n%s", _("Protegido contra lectura: sólo se puede borrar entero"), st.boot);
        else if (st.flash_size && !st.page) snprintf(buf, sizeof buf, "%s %s\n%s", fl, _("de flash"), st.boot);
        else if (st.flash_size) snprintf(buf, sizeof buf, "%s %s  ·  %s %s\n%s", fl, _("de flash"), swd ? _("sector") : _("página"), pg, st.boot);
        else if (swd && st.page) snprintf(buf, sizeof buf, "%s  ·  %s %s\n%s", _("flash externa"), _("sector"), pg, st.boot);
        else snprintf(buf, sizeof buf, "%s\n%s", swd ? _("Sin driver de flash todavía") : _("Firma desconocida"), st.boot);
        lv_label_set_text(U.chip_info, buf);
        lv_obj_set_style_text_color(U.chip_info, st.secure ? AOS_C_ORANGE : AOS_C_DIM, 0);
    } else if (st.chip_known && strcmp(st.port, S.port) == 0 && (int)st.family == S.fam) {
        lv_label_set_text(U.chip_name, st.chip);
        char rev[16] = "", fl[24] = "", mac[32] = "";
        if (st.revision != 0xFFFF) snprintf(rev, sizeof rev, "v%u.%u  ·  ", st.revision / 100, st.revision % 100);
        if (st.flash_size) snprintf(fl, sizeof fl, "%u MB %s", (unsigned)(st.flash_size >> 20), _("de flash"));
        else snprintf(fl, sizeof fl, "%s", _("flash ?"));
        if (st.mac_known)
            snprintf(mac, sizeof mac, "\nMAC %02X:%02X:%02X:%02X:%02X:%02X", st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
        snprintf(buf, sizeof buf, "%s%s%s", rev, fl, mac);
        lv_label_set_text(U.chip_info, buf);
        lv_obj_set_style_text_color(U.chip_info, AOS_C_DIM, 0);
    } else if (detecting) {
        lv_label_set_text(U.chip_name, _("Buscando la placa…"));
        lv_label_set_text(U.chip_info, phase_title(&st));
        lv_obj_set_style_text_color(U.chip_info, AOS_C_DIM, 0);
    } else if (!st.busy && st.job == AOS_FLASHER_JOB_DETECT && !st.ok && !strcmp(st.port, S.port) && (int)st.family == S.fam) {
        lv_label_set_text(U.chip_name, _("No respondió"));
        lv_label_set_text(U.chip_info, st.error);
        lv_obj_set_style_text_color(U.chip_info, AOS_C_ORANGE, 0);
    } else {
        lv_label_set_text(U.chip_name, _("Placa sin detectar"));
        lv_label_set_text(U.chip_info, avr ? _("Conectá el Arduino y tocá Detectar")
                                     : swd ? _("SWCLK, SWDIO y GND al header, y tocá Detectar")
                                     : stm ? _("Al header con BOOT0 y NRST, y tocá Detectar")
                                           : _("Conectala al header y tocá Detectar"));
        lv_obj_set_style_text_color(U.chip_info, AOS_C_DIM, 0);
    }
    if (!U.detect_btn) return;      /* the compact card of the job's screen */
    lv_label_set_text(U.detect_lbl, detecting ? _("Buscando") : _("Detectar"));
    lv_obj_set_state(U.detect_btn, LV_STATE_DISABLED, st.busy);
    lv_obj_set_style_opa(U.detect_btn, st.busy && !detecting ? LV_OPA_40 : LV_OPA_COVER, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = i ? U.erase_btn : U.read_btn;
        if (!b) continue;
        lv_obj_set_state(b, LV_STATE_DISABLED, st.busy);
        lv_obj_set_style_opa(b, st.busy ? LV_OPA_40 : LV_OPA_COVER, 0);
    }

    /* the port line */
    const aos_io_port_t *p = aos_io_port_find(S.port);
    int en = -1, boot = -1;
    aos_io_port_lines(S.port, &en, &boot);
    /* a USB port shows what is plugged into it; orange when it is gone */
    int ui = !strncmp(S.port, "usb", 3) && S.port[3] >= '0' && S.port[3] <= '9' ? S.port[3] - '0' : -1;
    char dev[48] = "";
    bool usb_ok = ui >= 0 && ui < aos_hal_usb_serial_count() && aos_hal_usb_serial_name(ui, dev, sizeof dev);
    if (usb_ok) {
        snprintf(buf, sizeof buf, "%s %.20s", S.port, dev);
        lv_label_set_text(U.port_lbl, buf);
    } else {
        lv_label_set_text(U.port_lbl, S.port);
    }
    lv_obj_set_style_text_color(U.port_lbl, p || usb_ok ? AOS_C_TEXT : AOS_C_ORANGE, 0);
    snprintf(buf, sizeof buf, "%u", (unsigned)BAUDS[S.baud_i]);
    lv_label_set_text(U.baud_lbl, buf);
    /* an Arduino's bootloader has its own speed */
    lv_obj_set_flag(U.baud_chip, LV_OBJ_FLAG_HIDDEN, avr || swd || stm);

    /* somebody else on the port, or no EN/BOOT */
    char who[32];
    const char *owner = st.busy ? NULL : port_holder(who, sizeof who);
    bool warn = owner || (p && en < 0 && boot < 0 && !swd);
    lv_obj_set_flag(U.busy_row, LV_OBJ_FLAG_HIDDEN, !warn);
    if (owner) {
        snprintf(buf, sizeof buf, _("%s tiene %s"), who, S.port);
        lv_label_set_text(U.busy_lbl, buf);
        lv_obj_set_flag(U.busy_btn, LV_OBJ_FLAG_HIDDEN, strncmp(owner, "serial ", 7) != 0);
    } else if (warn) {
        lv_label_set_text(U.busy_lbl, stm ? _("Sin EN/BOOT en modules.txt: poné BOOT0 en 1 y apretá reset al empezar")
                                  : avr ? _("Sin EN en modules.txt para el RESET: apretá el reset del Arduino al empezar")
                                          : _("Sin EN/BOOT en modules.txt: poné la placa en modo descarga a mano"));
        lv_obj_add_flag(U.busy_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void detect_cb(lv_event_t *e)
{
    if (aos_flasher_busy()) return;
    aos_flasher_opts_t o = job_opts();
    if (!aos_flasher_detect_ex(S.port, &o)) aos_ui_toast(_("No se pudo empezar"), 1800);
    refresh_target();
}

static void port_cb(lv_event_t *e)
{
    if (aos_flasher_busy()) return;
    /* the next UART port of modules.txt, then the USB serial ports of the
     * host plugged in now ("usb0"...), and round again */
    char names[24][16];
    int n = 0, cur = -1;
    for (int i = 0; i < aos_io_port_count() && n < 20; i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p && p->kind == AOS_PORT_UART) snprintf(names[n++], sizeof names[0], "%s", p->name);
    }
    for (int i = 0; S.fam != AOS_FLASHER_SWD && i < aos_hal_usb_serial_count() && i < 4; i++)
        snprintf(names[n++], sizeof names[0], "usb%d", i);
    for (int i = 0; i < n; i++) if (!strcmp(names[i], S.port)) cur = i;
    if (n) snprintf(S.port, sizeof S.port, "%s", names[(cur + 1) % n]);
    aos_hal_pref_set_str("fl_port", S.port);
    build();
}

static void fam_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (aos_flasher_busy() || f == S.fam) return;
    S.fam = f;
    aos_hal_pref_set_i32("fl_fam", f);
    if (f == AOS_FLASHER_SWD && !aos_io_port_find(S.port)) {
        /* SWD only goes through the header's pins */
        const aos_io_module_t *m = aos_io_module_find("target");
        snprintf(S.port, sizeof S.port, "%s", m ? m->port : "uart.b");
        aos_hal_pref_set_str("fl_port", S.port);
    }
    const aos_flasher_source_t *s = selected();
    if (s && !aos_flasher_source_fits(s, S.fam)) S.sel[0] = 0;
    build();
}

static void baud_cb(lv_event_t *e)
{
    S.baud_i = (S.baud_i + 1) % (int)N_BAUDS;
    aos_hal_pref_set_i32("fl_baud", (int32_t)BAUDS[S.baud_i]);
    refresh_target();
}

static void release_cb(lv_event_t *e)
{
    char who[32];
    const char *o = port_holder(who, sizeof who);
    if (o && !strncmp(o, "serial ", 7)) aos_serial_stop(atoi(o + 7));
    refresh_target();
    if (U.main_btn) build();
}

/* A job from the target card: the port must be free of anybody else. */
static bool port_free_or_say(void)
{
    char who[32];
    if (!port_holder(who, sizeof who)) return true;
    char msg[96];
    snprintf(msg, sizeof msg, _("%s tiene %s"), who, S.port);
    aos_ui_toast(msg, 2200);
    return false;
}

static void job_started(bool ok)
{
    if (!ok) { aos_ui_toast(_("No se pudo empezar"), 1800); return; }
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    S.seen_seq = st.seq;
    S.job_view = true;
    build();
}

static void read_cb(lv_event_t *e)
{
    if (aos_flasher_busy() || !port_free_or_say()) return;
    aos_flasher_opts_t o = job_opts();
    S.log_from = aos_flasher_log_count();
    /* SWD does not dump a chip: its tool there is the RTT console */
    job_started(S.fam == AOS_FLASHER_SWD ? aos_flasher_console(S.port, &o) : aos_flasher_read(S.port, &o));
}

static void bridge_cb(lv_event_t *e)
{
    if (aos_flasher_busy() || !port_free_or_say()) return;
    aos_flasher_opts_t o = job_opts();
    o.baud = BAUDS[S.baud_i] == 460800 ? 115200 : BAUDS[S.baud_i];     /* the tools set their own; a start */
    S.log_from = aos_flasher_log_count();
    job_started(aos_flasher_bridge(S.port, &o));
}

static void probe_cb(lv_event_t *e)
{
    if (aos_flasher_busy() || !port_free_or_say()) return;
    if (aos_hal_usb_mode() == AOS_HAL_USB_DISK) {
        aos_ui_toast(_("La tarjeta la tiene la computadora (modo Disco)"), 2500);
        return;
    }
    aos_flasher_opts_t o = job_opts();
    S.log_from = aos_flasher_log_count();
    job_started(aos_flasher_probe(S.port, &o));
}

static void overlay_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
}

static void erase_no_cb(lv_event_t *e) { overlay_close(); }

static void erase_yes_cb(lv_event_t *e)
{
    overlay_close();
    if (aos_flasher_busy() || !port_free_or_say()) return;
    aos_flasher_opts_t o = job_opts();
    S.log_from = aos_flasher_log_count();
    job_started(aos_flasher_erase(S.port, &o));
}

/* Erasing loses whatever program the board has: asked first, with the way
 * to keep it. */
static void erase_cb(lv_event_t *e)
{
    if (aos_flasher_busy() || U.overlay) return;
    U.overlay = box(lv_layer_top(), lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_60, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    int32_t w = U.land ? 620 : U.W - 80;
    lv_obj_t *c = card(U.overlay, w);
    lv_obj_set_style_pad_all(c, 30, 0);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_center(c);
    bool avr = S.fam == AOS_FLASHER_AVR;
    lv_obj_t *t = aos_label(c, avr ? _("¿Borrar el programa?") : _("¿Borrar toda la flash?"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, w - 60);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    char msg[200];
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    bool prot = S.fam == AOS_FLASHER_SWD && st.chip_known && st.secure && (int)st.family == S.fam;
    snprintf(msg, sizeof msg, avr ? _("El Arduino conectado en %s se queda sin programa; su bootloader queda. Para guardarlo antes, usá Leer la flash.")
                            : prot ? _("El chip en %s está protegido: borrarlo entero se lleva su firmware y la protección.")
                            : S.fam == AOS_FLASHER_SWD ? _("El chip conectado en %s se queda sin programa.")
                                  : _("La placa conectada en %s se queda sin programa. Para guardarlo antes, usá Leer la flash."), S.port);
    lv_obj_t *m = aos_label(c, msg, aos_font_body, AOS_C_DIM);
    lv_obj_set_width(m, w - 60);
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *row = box(c, w - 60, 88);
    lv_obj_set_style_pad_top(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    pill(row, NULL, _("Cancelar"), AOS_C_CARD2, AOS_C_TEXT, erase_no_cb, NULL);
    pill(row, NULL, _("Borrar todo"), AOS_C_RED, lv_color_white(), erase_yes_cb, NULL);
}

/* A tappable chip of the card's second row. */
static lv_obj_t *chip_btn(lv_obj_t *parent, const char *glyph, lv_color_t gc, lv_event_cb_t cb, lv_obj_t **lbl, const char *unit)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 8, 0);
    aos_make_decorative(aos_label(b, glyph, &aos_sym_28, gc));
    *lbl = aos_label(b, "", aos_font_small, AOS_C_TEXT);
    aos_make_decorative(*lbl);
    if (unit) aos_make_decorative(aos_label(b, unit, aos_font_caption, AOS_C_DIM));
    return b;
}

/* ESP32 | Arduino: two halves, the chosen one filled */
static void build_kind(lv_obj_t *parent)
{
    lv_obj_t *seg = box(parent, lv_pct(100), 60);
    lv_obj_set_style_bg_color(seg, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(seg, 30, 0);
    lv_obj_set_style_pad_all(seg, 4, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 4, 0);
    for (int f = 0; f < AOS_FLASHER_FAMILIES; f++) {
        bool on = f == S.fam;
        lv_obj_t *b = box(seg, 1, lv_pct(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 26, 0);
        lv_obj_set_style_bg_color(b, C_TINT_DEEP, 0);
        lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, fam_cb, LV_EVENT_CLICKED, (void *)(intptr_t)f);
        lv_obj_t *l = aos_label(b, aos_flasher_family_name(f), aos_font_small, on ? lv_color_white() : AOS_C_DIM);
        lv_obj_center(l);
    }
}

static void build_target(lv_obj_t *parent, int32_t w, bool compact)
{
    lv_obj_t *c = card(parent, w);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_set_style_pad_row(c, 20, 0);
    if (!compact) build_kind(c);

    /* the chip: what it is, or how to find out */
    /* tall enough for the name and two lines under it (revision and flash,
     * then the MAC) */
    lv_obj_t *top = box(c, lv_pct(100), 50 + 2 * lv_font_get_line_height(aos_font_caption));
    glyph_square(top, AOS_SYM_CHIP, C_TINT, C_TINT_DEEP, 96);
    int32_t tw = w - 48 - 116;
    U.chip_name = aos_label(top, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_size(U.chip_name, tw, lv_font_get_line_height(aos_font_title));
    lv_label_set_long_mode(U.chip_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.chip_name, LV_ALIGN_TOP_LEFT, 116, 4);
    U.chip_info = aos_label(top, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_size(U.chip_info, tw, 2 * lv_font_get_line_height(aos_font_caption));
    lv_label_set_long_mode(U.chip_info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.chip_info, LV_ALIGN_TOP_LEFT, 116, 50);
    if (compact) { refresh_target(); return; }

    hairline(c);

    /* the port, the speed (a tap cycles each) and Detectar; in landscape
     * the column is too narrow for the three, and Detectar gets a row */
    lv_obj_t *row = box(c, lv_pct(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *pc = chip_btn(row, AOS_SYM_SERIAL_PORT, C_TINT, port_cb, &U.port_lbl, NULL);
    lv_obj_t *bc = chip_btn(row, AOS_SYM_SPEEDOMETER, AOS_C_DIM, baud_cb, &U.baud_lbl, "bps");
    U.baud_chip = bc;
    if (U.land && S.fam != AOS_FLASHER_ESP) {
        /* no speed to choose: the port and Detectar fit in one row */
        lv_obj_set_flex_grow(pc, 1);
    } else if (U.land) {
        lv_obj_set_flex_grow(pc, 1);
        lv_obj_set_flex_grow(bc, 1);
        row = box(c, lv_pct(100), 64);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    } else {
        lv_obj_t *sp = box(row, 1, 1);
        lv_obj_set_flex_grow(sp, 1);
    }
    U.detect_btn = pill(row, AOS_SYM_MAGNIFY, _("Detectar"), lv_color_hex(0x2A2340), lv_color_hex(0xC4B5FD), detect_cb, &U.detect_lbl);
    lv_obj_set_height(U.detect_btn, 64);
    if (U.land) lv_obj_set_flex_grow(U.detect_btn, 1);

    /* the tools: the flash to the card, and erasing it */
    row = box(c, lv_pct(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    U.read_btn = S.fam == AOS_FLASHER_SWD ? pill(row, AOS_SYM_MESSAGE_TEXT_OUTLINE, _("Consola RTT"), AOS_C_CARD2, AOS_C_TEXT, read_cb, NULL)
                                          : pill(row, AOS_SYM_DOWNLOAD, _("Leer la flash"), AOS_C_CARD2, AOS_C_TEXT, read_cb, NULL);
    U.erase_btn = pill(row, AOS_SYM_ERASER, S.fam != AOS_FLASHER_AVR ? _("Borrar la flash") : U.land ? _("Borrar") : _("Borrar el programa"), AOS_C_CARD2,
                       lv_color_hex(0xFF8A80), erase_cb, NULL);
    lv_obj_set_height(U.read_btn, 64);
    lv_obj_set_height(U.erase_btn, 64);
    if (U.land) {
        lv_obj_set_flex_grow(U.read_btn, 1);
        lv_obj_set_flex_grow(U.erase_btn, 1);
    }
    /* the computer's own tools: the port over the network, or, for SWD,
     * the board as their debug probe on the OTG connector */
    row = box(c, lv_pct(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *br = S.fam == AOS_FLASHER_SWD
                 ? pill(row, AOS_SYM_USB, _("Sonda para la computadora"), AOS_C_CARD2, AOS_C_TEXT, probe_cb, NULL)
                 : pill(row, AOS_SYM_LAN_CONNECT, _("Puente para la Mac"), AOS_C_CARD2, AOS_C_TEXT, bridge_cb, NULL);
    lv_obj_set_height(br, 64);
    if (U.land) lv_obj_set_flex_grow(br, 1);

    /* the warning row, shown when it applies */
    U.busy_row = box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(U.busy_row, lv_color_hex(0x3A2A10), 0);
    lv_obj_set_style_bg_opa(U.busy_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.busy_row, 18, 0);
    lv_obj_set_style_pad_all(U.busy_row, 16, 0);
    lv_obj_set_flex_flow(U.busy_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.busy_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(U.busy_row, 14, 0);
    aos_label(U.busy_row, AOS_SYM_ALERT_OUTLINE, &aos_sym_28, AOS_C_ORANGE);
    U.busy_lbl = aos_label(U.busy_row, "", aos_font_caption, lv_color_hex(0xFFD6A0));
    lv_obj_set_flex_grow(U.busy_lbl, 1);
    lv_label_set_long_mode(U.busy_lbl, LV_LABEL_LONG_MODE_WRAP);
    U.busy_btn = pill(U.busy_row, NULL, _("Liberar"), AOS_C_ORANGE, lv_color_black(), release_cb, NULL);
    lv_obj_set_height(U.busy_btn, 60);
    refresh_target();
}

/* How to wire the target to the header: each line of the port with its
 * physical pin, and where it goes on the other board. */
static void build_wiring(lv_obj_t *parent, int32_t w)
{
    const aos_io_port_t *p = aos_io_port_find(S.port);
    if (!p) return;
    int en = -1, boot = -1;
    aos_io_port_lines(S.port, &en, &boot);
    section(parent, _("CONEXIÓN"));
    lv_obj_t *c = card(parent, w);
    lv_obj_set_style_pad_ver(c, 6, 0);
    bool avr = S.fam == AOS_FLASHER_AVR, swd = S.fam == AOS_FLASHER_SWD, stm = S.fam == AOS_FLASHER_STM32;
    struct { int gpio; const char *what, *to; } L[] = {
        { p->pins[0], swd ? "SWCLK" : "TX", swd ? N_("SWCLK del objetivo") : avr ? N_("RX del Arduino (D0)") : stm ? N_("RX del STM32 (PA10)") : N_("RX de la placa (U0RXD)") },
        { p->pins[1], swd ? "SWDIO" : "RX", swd ? N_("SWDIO del objetivo") : avr ? N_("TX del Arduino (D1), ver abajo") : stm ? N_("TX del STM32 (PA9)") : N_("TX de la placa (U0TXD)") },
        { en, swd ? "NRST" : "EN", swd ? N_("RESET, si lo tiene a mano") : avr ? N_("RESET") : stm ? N_("NRST") : N_("EN / RST") },
        { avr || swd ? -1 : boot, "BOOT", stm ? N_("BOOT0 (el puente, en una Blue Pill)") : N_("GPIO0 (GPIO9 en C3/C6/H2, GPIO35 en P4)") },
        { -2, "GND", N_("GND, común a las dos") },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) {
        if (L[i].gpio == -1) continue;
        if (n++) { lv_obj_t *hl = hairline(c); lv_obj_set_style_margin_left(hl, 96, 0); }
        lv_obj_t *r = box(c, lv_pct(100), 68);
        lv_obj_set_style_pad_hor(r, 24, 0);
        /* the header pin, as a numbered dot */
        const aos_io_pin_t *hp = L[i].gpio >= 0 ? aos_io_pin_of_gpio(L[i].gpio) : NULL;
        lv_obj_t *dot = box(r, 52, 52);
        lv_obj_set_style_radius(dot, 26, 0);
        lv_obj_set_style_bg_color(dot, L[i].gpio == -2 ? lv_color_hex(0x3A3A3C) : lv_color_hex(0x2A2340), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
        char num[8];
        snprintf(num, sizeof num, "%d", hp ? hp->pin : 10);
        lv_obj_t *nl = aos_label(dot, num, aos_font_caption, L[i].gpio == -2 ? AOS_C_DIM : lv_color_hex(0xC4B5FD));
        lv_obj_center(nl);
        char left[24];
        if (L[i].gpio >= 0) snprintf(left, sizeof left, "%s %s%d", L[i].what, swd ? "" : " GPIO", L[i].gpio);
        else snprintf(left, sizeof left, "%s", L[i].what);
        lv_obj_t *ll = aos_label(r, left, aos_font_small, AOS_C_TEXT);
        lv_obj_align(ll, LV_ALIGN_LEFT_MID, 72, 0);
        lv_obj_t *to = aos_label(r, _(L[i].to), aos_font_caption, AOS_C_DIM);
        lv_obj_align(to, LV_ALIGN_RIGHT_MID, 0, 0);
        aos_make_decorative(r);
    }
    if (swd) {
        lv_obj_t *n = aos_label(parent, _("Un objetivo de 3,3 V con su propia alimentación; el 3V3 de acá sólo si no la tiene. En una Pico: el conector DEBUG (SWCLK, GND, SWDIO)."),
                                aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(n, w - 48);
        lv_obj_set_style_pad_hor(n, 24, 0);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    }
    if (avr) {
        /* the P4's pins are 3.3 V and do not take 5 */
        lv_obj_t *n = aos_label(parent, _("Un Arduino de 5 V (Uno, Nano, Mega, Leonardo): su TX va al RX de acá por un divisor (1 kΩ en serie, 2 kΩ a GND) o un adaptador de niveles. Uno de 3,3 V va directo. Por USB no hace falta nada."),
                                aos_font_caption, AOS_C_ORANGE);
        lv_obj_set_width(n, w - 48);
        lv_obj_set_style_pad_hor(n, 24, 0);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    }
}

/* -------------------------------------------------------------------------- */
/* The firmware on the card                                                    */
/* -------------------------------------------------------------------------- */

static void source_cb(lv_event_t *e)
{
    if (aos_flasher_busy()) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= S.nsrc) return;
    if (!strcmp(S.sel, S.src[i].path)) S.sel[0] = 0;
    else snprintf(S.sel, sizeof S.sel, "%s", S.src[i].path);
    aos_hal_pref_set_str("fl_sel", S.sel);
    build();
}

static void rescan_cb(lv_event_t *e)
{
    scan();
    build();
    aos_ui_toast(S.nsrc == 1 ? _("1 firmware en la tarjeta") : _("Tarjeta leída"), 1400);
}

/* The chip a source is for, against the one detected: "" when fine. */
static const char *mismatch(const aos_flasher_source_t *s)
{
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    if (!st.chip_known || !s->chip[0] || (int)st.family != S.fam) return "";
    return strcasecmp(aos_flasher_chip_label(s->chip), st.chip) ? aos_flasher_chip_label(s->chip) : "";
}

static void file_lines(lv_obj_t *parent, const aos_flasher_source_t *s)
{
    lv_obj_t *blk = box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(blk, 108, 0);
    lv_obj_set_style_pad_right(blk, 24, 0);
    lv_obj_set_style_pad_bottom(blk, 20, 0);
    lv_obj_set_flex_flow(blk, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(blk, 8, 0);
    for (int f = 0; f < s->nfiles; f++) {
        lv_obj_t *r = box(blk, lv_pct(100), LV_SIZE_CONTENT);
        char off[16], sz[16];
        snprintf(off, sizeof off, "0x%05X", (unsigned)s->files[f].offset);
        fmt_size(sz, sizeof sz, s->files[f].size);
        lv_obj_t *o = aos_label(r, off, &aos_mono_18, lv_color_hex(0xC4B5FD));
        lv_obj_align(o, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *n = aos_label(r, s->files[f].name, &aos_mono_18, AOS_C_TEXT);
        lv_obj_set_size(n, lv_pct(62), lv_font_get_line_height(&aos_mono_18));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 104, 0);
        lv_obj_t *z = aos_label(r, sz, &aos_mono_18, AOS_C_DIM);
        lv_obj_align(z, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    char buf[200] = "";
    if (s->flash_mode[0])
        snprintf(buf, sizeof buf, "%s %s  ·  %s  ·  %s", _("Flash"), s->flash_mode, s->flash_size, s->flash_freq);
    const char *note = s->kind == AOS_FLASHER_BIN_APP ? _("Imagen de app: va a 0x10000, sobre el bootloader que la placa ya tenga")
                     : s->kind == AOS_FLASHER_BIN_MERGED ? _("Imagen completa: va entera desde 0x0")
                     : s->kind == AOS_FLASHER_BIN_AT ? _("La dirección sale del nombre del archivo")
                     : s->kind == AOS_FLASHER_IMAGE ? _("Las direcciones salen del archivo") : "";
    if (note[0]) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%s", buf[0] ? "\n" : "", note);
    if (buf[0]) {
        lv_obj_t *nl = aos_label(blk, buf, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(nl, lv_pct(100));
        lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_top(nl, 4, 0);
    }
    aos_make_decorative(blk);
}

/* "ESP32-S3-e53a10-20261027-174612@0x0.bin" -> "Volcado del 27/10 17:46";
 * the name as it is when it has no date in it */
static void source_title(const aos_flasher_source_t *s, char *out, size_t n)
{
    snprintf(out, n, "%s", s->name);
    if (!s->dump) return;
    for (const char *p = s->name; *p; p++) {
        int y, mo, d, h, mi;
        if (sscanf(p, "%4d%2d%2d-%2d%2d", &y, &mo, &d, &h, &mi) == 5 && p[8] == '-' && y > 2000 && mo >= 1 && mo <= 12) {
            snprintf(out, n, _("Volcado del %d/%d %02d:%02d"), d, mo, h, mi);
            return;
        }
    }
}

static void build_sources(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *head = box(parent, w, 56);
    lv_obj_t *t = aos_label(head, _("FIRMWARE EN LA TARJETA"), aos_font_caption, AOS_C_DIM);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 24, -6);
    lv_obj_t *rb = box(head, 72, 56);
    lv_obj_add_flag(rb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rb, rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rg = aos_label(rb, AOS_SYM_RESTART, &aos_sym_28, C_TINT);
    lv_obj_align(rg, LV_ALIGN_RIGHT_MID, -12, 4);
    lv_obj_align(rb, LV_ALIGN_RIGHT_MID, 0, 0);

    U.list = card(parent, w);
    int shown = 0;
    for (int i = 0; i < S.nsrc; i++) shown += aos_flasher_source_fits(&S.src[i], S.fam);
    if (!shown) {
        lv_obj_set_style_pad_all(U.list, 32, 0);
        lv_obj_set_style_pad_row(U.list, 14, 0);
        lv_obj_set_flex_align(U.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        aos_label(U.list, AOS_SYM_SD, &aos_sym_72, AOS_C_DIM);
        aos_label(U.list, aos_flasher_dir() ? _("No hay firmware en /firmware") : _("No hay tarjeta"), aos_font_body, AOS_C_TEXT);
        lv_obj_t *h = aos_label(U.list, S.fam == AOS_FLASHER_SWD || S.fam == AOS_FLASHER_STM32
                                    ? _("Copiá ahí el .hex, .elf, .uf2 o .bin de tu firmware (un .bin va al comienzo de la flash).")
                                    : S.fam == AOS_FLASHER_AVR
                                    ? _("Copiá ahí el .hex de tu sketch: en el IDE de Arduino, Sketch > Exportar binario compilado. El que no dice with_bootloader.")
                                    : _("Copiá ahí la carpeta build/ de un proyecto de ESP-IDF (la que tiene flasher_args.json) o un .bin. Un nombre como app@0x10000.bin dice dónde va."),
                                aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(h, lv_pct(100));
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    int row = 0;
    for (int i = 0; i < S.nsrc; i++) {
        const aos_flasher_source_t *s = &S.src[i];
        if (!aos_flasher_source_fits(s, S.fam)) continue;
        bool sel = !strcmp(S.sel, s->path);
        if (row++) {
            lv_obj_t *hl = hairline(U.list);
            lv_obj_set_style_margin_left(hl, 108, 0);
        }
        lv_obj_t *r = box(U.list, lv_pct(100), 112);
        lv_obj_set_style_pad_hor(r, 24, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, source_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (s->project) glyph_square(r, AOS_SYM_FOLDER, lv_color_hex(0x60A5FA), lv_color_hex(0x1D4ED8), 64);
        else if (s->dump) glyph_square(r, AOS_SYM_DOWNLOAD, lv_color_hex(0x34D399), lv_color_hex(0x047857), 64);
        else glyph_square(r, AOS_SYM_MEMORY, lv_color_hex(0x94A3B8), lv_color_hex(0x475569), 64);
        lv_obj_align(lv_obj_get_child(r, 0), LV_ALIGN_LEFT_MID, 0, 0);

        char title[64];
        source_title(s, title, sizeof title);
        lv_obj_t *n = aos_label(r, title, aos_font_body, AOS_C_TEXT);
        lv_obj_set_size(n, w - 48 - 84 - 60, lv_font_get_line_height(aos_font_body));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 84, -18);

        char sub[128], sz[16];
        fmt_size(sz, sizeof sz, s->total);
        const char *mm0 = mismatch(s);
        char chipbuf[32];
        if (mm0[0]) snprintf(chipbuf, sizeof chipbuf, "%s %s", _("Para"), mm0);
        else snprintf(chipbuf, sizeof chipbuf, "%s", s->chip[0] ? aos_flasher_chip_label(s->chip) : _("chip ?"));
        const char *chip = chipbuf;
        const char *ext = strrchr(s->name, '.');
        if (s->project)
            snprintf(sub, sizeof sub, "%s  ·  %d %s  ·  %s%s%s", chip, s->nfiles, _("archivos"), sz,
                     s->app_version[0] ? "  ·  " : "", s->app_version);
        else if (s->kind == AOS_FLASHER_IMAGE)
            snprintf(sub, sizeof sub, "%s  ·  %s  ·  %s", chip, ext ? ext + 1 : "", sz);
        else
            snprintf(sub, sizeof sub, "%s  ·  0x%X  ·  %s%s%s", chip, (unsigned)s->files[0].offset, sz,
                     s->app_version[0] ? "  ·  " : "", s->app_version);
        const char *mm = mismatch(s);
        lv_obj_t *sl = aos_label(r, sub, aos_font_caption, mm[0] ? AOS_C_ORANGE : AOS_C_DIM);
        lv_obj_set_size(sl, w - 48 - 84 - 60, lv_font_get_line_height(aos_font_caption));
        lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(sl, LV_ALIGN_LEFT_MID, 84, 22);

        lv_obj_t *mk = aos_label(r, sel ? AOS_SYM_CHECK : mm[0] ? AOS_SYM_ALERT_OUTLINE : AOS_SYM_CHEVRON_RIGHT, &aos_sym_28,
                                 sel ? C_TINT : mm[0] ? AOS_C_ORANGE : lv_color_hex(0x48484A));
        lv_obj_align(mk, LV_ALIGN_RIGHT_MID, 0, 0);
        aos_make_decorative(n);
        aos_make_decorative(sl);
        aos_make_decorative(mk);
        if (sel) file_lines(U.list, s);
    }
}

/* -------------------------------------------------------------------------- */
/* The job                                                                     */
/* -------------------------------------------------------------------------- */

static lv_color_t level_color(int level)
{
    switch (level) {
    case 1: return lv_color_hex(0x7BD88F);
    case 2: return lv_color_hex(0xFFC857);
    case 3: return lv_color_hex(0xFF6B6B);
    default: return lv_color_hex(0xB8BEC8);
    }
}

static void log_append(void)
{
    if (!U.log_col) return;
    uint32_t n = aos_flasher_log_count(), first = aos_flasher_log_first();
    if (U.log_shown < S.log_from) U.log_shown = S.log_from;
    if (U.log_shown < first) U.log_shown = first;
    bool added = false;
    while (U.log_shown < n) {
        char line[AOS_FLASHER_LOG_LINE];
        int lv = aos_flasher_log_line(U.log_shown++, line, sizeof line);
        if (lv < 0) continue;
        lv_obj_t *l = aos_label(U.log_col, line, &aos_mono_18, level_color(lv));
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        aos_make_decorative(l);
        added = true;
    }
    /* a long job: the oldest rows go, the ring keeps them anyway */
    while (lv_obj_get_child_count(U.log_col) > 80) lv_obj_delete(lv_obj_get_child(U.log_col, 0));
    if (added) {
        lv_obj_update_layout(U.log_col);
        lv_obj_scroll_to_y(U.log_col, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void refresh_job(void)
{
    if (!U.arc) return;
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    /* a job that moves data has a percentage; an erase does not */
    bool flash_job = st.job == AOS_FLASHER_JOB_FLASH || st.job == AOS_FLASHER_JOB_READ;
    bool done = !st.busy && st.job != AOS_FLASHER_JOB_NONE && st.job != AOS_FLASHER_JOB_DETECT;
    lv_color_t col = !done ? C_TINT : st.ok ? AOS_C_GREEN : st.phase == AOS_FLASHER_CANCELLED ? AOS_C_DIM : AOS_C_RED;
    lv_arc_set_value(U.arc, flash_job ? st.percent : 0);
    lv_obj_set_style_arc_color(U.arc, col, LV_PART_INDICATOR);
    char buf[128];
    if (done) {
        lv_obj_add_flag(U.pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.mark, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(U.mark, st.ok ? AOS_SYM_CHECK : st.phase == AOS_FLASHER_CANCELLED ? AOS_SYM_STOP : AOS_SYM_CLOSE);
        lv_obj_set_style_text_color(U.mark, col, 0);
    } else if (st.job == AOS_FLASHER_JOB_CONSOLE || st.job == AOS_FLASHER_JOB_BRIDGE || st.job == AOS_FLASHER_JOB_PROBE) {
        /* a console, a bridge or a probe has no end to count towards */
        lv_obj_add_flag(U.pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.mark, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(U.mark, st.job == AOS_FLASHER_JOB_BRIDGE ? AOS_SYM_LAN_CONNECT
                                : st.job == AOS_FLASHER_JOB_PROBE ? AOS_SYM_USB : AOS_SYM_MESSAGE_TEXT_OUTLINE);
        lv_obj_set_style_text_color(U.mark, C_TINT, 0);
    } else {
        lv_obj_remove_flag(U.pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.mark, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(U.pct, "%d%%", flash_job ? st.percent : 0);
    }
    lv_label_set_text(U.ph_title, phase_title(&st));

    /* the line under the phase */
    if (done && st.ok && st.job == AOS_FLASHER_JOB_READ) {
        snprintf(buf, sizeof buf, "%s  ·  SHA-256 %.12s…  ·  %s", _("En firmware/volcados"), st.out_sha256,
                 !st.verified ? _("sin verificar") : st.family == AOS_FLASHER_AVR ? _("leído dos veces") : _("MD5 verificado"));
    } else if (done && st.ok && st.job == AOS_FLASHER_JOB_ERASE) {
        snprintf(buf, sizeof buf, _("%s sin programa: lista para grabar"), st.chip);
    } else if (done && st.phase == AOS_FLASHER_CANCELLED && st.job == AOS_FLASHER_JOB_READ) {
        snprintf(buf, sizeof buf, "%s", _("No se guardó nada"));
    } else if (done && st.phase == AOS_FLASHER_CANCELLED && st.job == AOS_FLASHER_JOB_ERASE) {
        snprintf(buf, sizeof buf, "%s", _("Puede haber quedado a medio borrar"));
    } else if (st.phase == AOS_FLASHER_READING) {
        snprintf(buf, sizeof buf, "%s  ·  0x%X - 0x%X", st.chip, (unsigned)st.out_from, (unsigned)(st.out_from + st.total));
    } else if (done && st.ok) {
        snprintf(buf, sizeof buf, "%s %s %s  ·  %s", st.source, _("en"), st.chip,
                 !st.verified ? _("sin verificar") : st.family != AOS_FLASHER_ESP ? _("verificado") : _("MD5 verificado"));
    } else if (done && st.phase == AOS_FLASHER_CANCELLED && st.done) {
        snprintf(buf, sizeof buf, "%s", _("Quedó a medio grabar: grabala de nuevo para que arranque"));
    } else if (done) {
        snprintf(buf, sizeof buf, "%s", st.error);
    } else if (st.phase >= AOS_FLASHER_ERASING && st.phase <= AOS_FLASHER_VERIFYING) {
        snprintf(buf, sizeof buf, "%s  ·  %s %d %s %d  ·  0x%X", st.file_name, _("archivo"), st.file_index + 1, _("de"),
                 st.file_count, (unsigned)st.file_offset);
    } else if (st.phase == AOS_FLASHER_BRIDGE) {
        const char *n = aos_hal_device_name();
        snprintf(buf, sizeof buf, "esptool: rfc2217://%s.local:4000\navrdude: net:%s.local:4001", n && *n ? n : "p4os",
                 n && *n ? n : "p4os");
    } else if (st.phase == AOS_FLASHER_PROBE) {
        snprintf(buf, sizeof buf, "%s", aos_hal_usb_connected() ? _("La computadora la tiene: OpenOCD, pyOCD o probe-rs")
                                                                : _("Conectá el conector OTG a la computadora"));
    } else if (st.phase == AOS_FLASHER_CONSOLE) {
        snprintf(buf, sizeof buf, "%s  ·  %s", st.chip, _("lo que escribe su firmware, abajo"));
    } else if (st.chip_known && st.family != AOS_FLASHER_ESP) {
        snprintf(buf, sizeof buf, "%s  ·  %s", st.chip, st.boot);
    } else if (st.chip_known) {
        snprintf(buf, sizeof buf, "%s  ·  %s", st.chip, st.stub ? _("con stub") : _("con la ROM"));
    } else {
        snprintf(buf, sizeof buf, "%s  ·  %s", st.source, st.port);
    }
    lv_label_set_text(U.ph_sub, buf);
    lv_obj_set_style_text_color(U.ph_sub, done && !st.ok ? lv_color_hex(0xFF8A80) : AOS_C_DIM, 0);

    /* the three numbers; a probe's are the debugger's commands and the SWD
     * transfers they made */
    bool probe = st.job == AOS_FLASHER_JOB_PROBE;
    lv_label_set_text(lv_obj_get_child(lv_obj_get_parent(U.stat[0]), 0), probe ? _("COMANDOS") : _("TRANSFERIDO"));
    lv_label_set_text(lv_obj_get_child(lv_obj_get_parent(U.stat[1]), 0), probe ? _("PAQUETES SWD") : _("VELOCIDAD"));
    fmt_pair(buf, sizeof buf, st.done, st.total);
    if (probe) snprintf(buf, sizeof buf, "%u", (unsigned)st.done);
    lv_label_set_text(U.stat[0], flash_job || probe ? buf : "-");
    if (probe) snprintf(buf, sizeof buf, "%u", (unsigned)st.total);
    else if (st.bytes_per_s) snprintf(buf, sizeof buf, "%u,%u KB/s", (unsigned)(st.bytes_per_s / 1024), (unsigned)(st.bytes_per_s % 1024 * 10 / 1024));
    else snprintf(buf, sizeof buf, "-");
    lv_label_set_text(U.stat[1], buf);
    if (done || !st.eta_ms) fmt_time(buf, sizeof buf, st.elapsed_ms);
    else fmt_time(buf, sizeof buf, st.eta_ms);
    lv_label_set_text(U.stat[2], buf);
    lv_obj_t *cap = lv_obj_get_child(lv_obj_get_parent(U.stat[2]), 0);
    lv_label_set_text(cap, done || !st.eta_ms ? _("TIEMPO") : _("RESTANTE"));

    /* each file */
    const aos_flasher_source_t *s = st.job == AOS_FLASHER_JOB_FLASH ? selected() : NULL;
    uint32_t before = 0;
    for (int f = 0; s && f < s->nfiles && f < AOS_FLASHER_FILES_MAX; f++) {
        lv_obj_t *g = U.file_state[f];
        if (!g) continue;
        /* one file: the job's own total (an AVR's pages, written and read
         * back, are not the .hex's bytes) */
        uint32_t sz = s->nfiles == 1 && st.total ? st.total : s->files[f].size;
        bool is_done = st.ok || st.done >= before + sz;
        bool cur = !is_done && f == st.file_index && st.phase >= AOS_FLASHER_ERASING;
        bool failed = done && !st.ok && f == st.file_index && st.phase == AOS_FLASHER_FAILED && st.file_count;
        if (is_done && flash_job) {
            lv_obj_set_style_text_font(g, &aos_sym_28, 0);
            lv_label_set_text(g, AOS_SYM_CHECK);
            lv_obj_set_style_text_color(g, AOS_C_GREEN, 0);
        } else if (failed && cur) {
            lv_obj_set_style_text_font(g, &aos_sym_28, 0);
            lv_label_set_text(g, AOS_SYM_CLOSE);
            lv_obj_set_style_text_color(g, AOS_C_RED, 0);
        } else if (cur) {
            lv_obj_set_style_text_font(g, aos_font_caption, 0);
            lv_label_set_text_fmt(g, "%d%%", (int)((uint64_t)(st.done - before) * 100 / (sz ? sz : 1)));
            lv_obj_set_style_text_color(g, lv_color_hex(0xC4B5FD), 0);
        } else {
            lv_obj_set_style_text_font(g, aos_font_caption, 0);
            lv_label_set_text(g, "•");
            lv_obj_set_style_text_color(g, lv_color_hex(0x48484A), 0);
        }
        before += sz;
    }
    log_append();
}

static void build_progress(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = card(parent, w);
    lv_obj_set_style_pad_all(c, 26, 0);
    lv_obj_set_style_pad_row(c, 22, 0);

    int32_t ring = U.land ? 168 : 220;
    lv_obj_t *top = box(c, lv_pct(100), ring);
    U.arc = lv_arc_create(top);
    lv_obj_set_size(U.arc, ring, ring);
    lv_arc_set_rotation(U.arc, 270);
    lv_arc_set_bg_angles(U.arc, 0, 360);
    lv_arc_set_range(U.arc, 0, 100);
    lv_arc_set_value(U.arc, 0);
    lv_obj_remove_style(U.arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(U.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(U.arc, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(U.arc, 20, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(U.arc, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(U.arc, true, LV_PART_INDICATOR);
    lv_obj_align(U.arc, LV_ALIGN_LEFT_MID, 0, 0);
    U.pct = aos_label(U.arc, "0%", aos_font_large, AOS_C_TEXT);
    lv_obj_center(U.pct);
    U.mark = aos_label(U.arc, AOS_SYM_CHECK, &aos_sym_72, AOS_C_GREEN);
    lv_obj_center(U.mark);
    lv_obj_add_flag(U.mark, LV_OBJ_FLAG_HIDDEN);

    /* the phase and its line, as a column centred on the ring: either may
     * take two lines */
    int32_t tx = ring + (U.land ? 24 : 30), tw = w - 52 - tx;
    lv_obj_t *txt = box(top, tw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 10, 0);
    lv_obj_align(txt, LV_ALIGN_LEFT_MID, tx, 0);
    U.ph_title = aos_label(txt, "", U.land ? aos_font_body : aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(U.ph_title, tw);
    lv_label_set_long_mode(U.ph_title, LV_LABEL_LONG_MODE_WRAP);
    U.ph_sub = aos_label(txt, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.ph_sub, tw);
    lv_label_set_long_mode(U.ph_sub, LV_LABEL_LONG_MODE_WRAP);

    hairline(c);
    static const char *const CAPS[3] = { N_("TRANSFERIDO"), N_("VELOCIDAD"), N_("RESTANTE") };
    lv_obj_t *stats = box(c, lv_pct(100), 72);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *col = box(stats, 1, 72);
        lv_obj_set_flex_grow(col, i == 0 ? 4 : 3);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 4, 0);
        aos_label(col, _(CAPS[i]), aos_font_tiny, AOS_C_DIM);
        U.stat[i] = aos_label(col, "-", aos_font_body, AOS_C_TEXT);
    }
}

static void build_files_state(lv_obj_t *parent, int32_t w)
{
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    const aos_flasher_source_t *s = st.job == AOS_FLASHER_JOB_FLASH ? selected() : NULL;
    if (!s) return;
    char t[80];
    snprintf(t, sizeof t, "%s  ·  %s", _("ARCHIVOS"), s->name);
    for (char *p = t; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    section(parent, t);
    lv_obj_t *c = card(parent, w);
    lv_obj_set_style_pad_ver(c, 8, 0);
    memset(U.file_state, 0, sizeof U.file_state);
    for (int f = 0; f < s->nfiles && f < AOS_FLASHER_FILES_MAX; f++) {
        if (f) { lv_obj_t *hl = hairline(c); lv_obj_set_style_margin_left(hl, 88, 0); }
        lv_obj_t *r = box(c, lv_pct(100), 72);
        lv_obj_set_style_pad_hor(r, 24, 0);
        lv_obj_t *g = aos_label(r, "•", aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(g, 64);
        lv_obj_set_style_text_align(g, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(g, LV_ALIGN_LEFT_MID, -6, 0);
        U.file_state[f] = g;
        char off[16], sz[16];
        snprintf(off, sizeof off, "0x%05X", (unsigned)s->files[f].offset);
        fmt_size(sz, sizeof sz, s->files[f].size);
        lv_obj_t *o = aos_label(r, off, &aos_mono_18, lv_color_hex(0xC4B5FD));
        lv_obj_align(o, LV_ALIGN_LEFT_MID, 64, 0);
        lv_obj_t *n = aos_label(r, s->files[f].name, aos_font_small, AOS_C_TEXT);
        lv_obj_set_size(n, w - 48 - 170 - 110, lv_font_get_line_height(aos_font_small));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 170, 0);
        lv_obj_t *z = aos_label(r, sz, aos_font_caption, AOS_C_DIM);
        lv_obj_align(z, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void build_log(lv_obj_t *parent, int32_t w, int32_t h)
{
    section(parent, _("REGISTRO"));
    U.log_col = box(parent, w, h);
    lv_obj_set_style_bg_color(U.log_col, C_LOG_BG, 0);
    lv_obj_set_style_bg_opa(U.log_col, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.log_col, 22, 0);
    lv_obj_set_style_pad_all(U.log_col, 18, 0);
    lv_obj_set_style_pad_row(U.log_col, 6, 0);
    lv_obj_set_flex_flow(U.log_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(U.log_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.log_col, LV_DIR_VER);
    U.log_shown = S.log_from;
    log_append();
}

/* -------------------------------------------------------------------------- */
/* The buttons                                                                 */
/* -------------------------------------------------------------------------- */

static void flash_cb(lv_event_t *e)
{
    const aos_flasher_source_t *s = selected();
    if (!s || aos_flasher_busy() || !port_free_or_say()) return;
    aos_flasher_opts_t o = job_opts();
    S.log_from = aos_flasher_log_count();
    job_started(aos_flasher_flash(S.port, s, &o));
}

static void cancel_cb(lv_event_t *e) { aos_flasher_cancel(); }

/* A line down the RTT console, with its newline */
static void con_send_cb(lv_event_t *e)
{
    if (!U.con_ta) return;
    const char *t = lv_textarea_get_text(U.con_ta);
    char buf[200];
    int n = snprintf(buf, sizeof buf, "%s\n", t);
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    if (!aos_flasher_console_send(buf, n)) aos_ui_toast(_("No entró: esperá que el firmware lea lo anterior"), 1800);
    else lv_textarea_set_text(U.con_ta, "");
}

/* The keyboard takes the bottom - in landscape only the left column's, so
 * the log on the right stays in sight - and the bar goes up over it */
static void con_ta_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (!U.con_kb || !U.actions) return;
    if (c == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(U.con_kb, U.con_ta);
        lv_obj_remove_flag(U.con_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(U.con_kb);
        lv_obj_update_layout(U.con_kb);
        lv_obj_set_y(U.actions, U.H - lv_obj_get_height(U.con_kb) - U.bar_h);
    } else if (c == LV_EVENT_DEFOCUSED || c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
        if (c == LV_EVENT_READY) { con_send_cb(e); return; }    /* Enter: sent, the keyboard stays */
        lv_obj_add_flag(U.con_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(U.actions, U.bar_y);
    }
}

static void done_cb(lv_event_t *e)
{
    if (aos_flasher_busy()) return;
    S.job_view = false;
    scan();                         /* a dump is on the list now */
    build();
}

/* The Terminal on the same port: its capture channel 0 at 115200 (9600 for
 * an Arduino, the speed of its examples), and the app itself, which shows
 * that channel and remembers the port. */
static void monitor_cb(lv_event_t *e)
{
    if (aos_flasher_busy()) return;
    uint32_t baud = S.fam == AOS_FLASHER_AVR ? 9600 : 115200;
    aos_hal_pref_set_str("ser_port", S.port);
    aos_hal_pref_set_i32("ser_baud", (int32_t)baud);
    if (!aos_serial_start(0, S.port, baud)) {
        aos_ui_toast(_("No se pudo abrir el puerto"), 1800);
        return;
    }
    aos_ui_open("aos.serial");
}

static lv_obj_t *big_button(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_color_t fg,
                            lv_event_cb_t cb, lv_obj_t **lbl)
{
    lv_obj_t *b = box(parent, 10, 104);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 52, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 14, 0);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_44, fg));
    lv_obj_t *l = aos_label(b, text, aos_font_body, fg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    aos_make_decorative(l);
    if (lbl) *lbl = l;
    return b;
}

static void build_actions(void)
{
    lv_obj_clean(U.actions);
    U.main_btn = U.main_lbl = U.con_ta = NULL;
    if (U.con_kb) { lv_obj_delete(U.con_kb); U.con_kb = NULL; }
    lv_obj_set_y(U.actions, U.bar_y);
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    U.shown_busy = st.busy;
    if (S.job_view && st.busy && st.job == AOS_FLASHER_JOB_CONSOLE) {
        /* a line to type, send, and the square that closes the console */
        U.con_ta = lv_textarea_create(U.actions);
        lv_textarea_set_one_line(U.con_ta, true);
        lv_textarea_set_placeholder_text(U.con_ta, _("Escribile al firmware…"));
        lv_obj_set_height(U.con_ta, 104);
        lv_obj_set_flex_grow(U.con_ta, 1);
        lv_obj_set_style_text_font(U.con_ta, &aos_mono_22, 0);
        lv_obj_set_style_bg_color(U.con_ta, AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(U.con_ta, AOS_C_TEXT, 0);
        lv_obj_set_style_border_width(U.con_ta, 0, 0);
        lv_obj_set_style_radius(U.con_ta, 52, 0);
        lv_obj_set_style_pad_hor(U.con_ta, 32, 0);
        lv_obj_set_style_pad_top(U.con_ta, 34, 0);
        lv_obj_add_event_cb(U.con_ta, con_ta_cb, LV_EVENT_ALL, NULL);
        lv_obj_t *send = big_button(U.actions, AOS_SYM_SEND, "", C_TINT, lv_color_white(), con_send_cb, NULL);
        lv_obj_set_flex_grow(send, 0);
        lv_obj_set_width(send, 104);
        lv_obj_t *stop = big_button(U.actions, AOS_SYM_STOP, "", AOS_C_CARD2, AOS_C_TEXT, cancel_cb, NULL);
        lv_obj_set_flex_grow(stop, 0);
        lv_obj_set_width(stop, 104);
        U.con_kb = lv_keyboard_create(U.root);
        lv_obj_set_size(U.con_kb, U.land ? SIDE_W : U.W, U.land ? U.H / 2 : U.H / 3);
        lv_obj_align(U.con_kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        aos_keyboard_style(U.con_kb, aos_font_small);
        lv_obj_add_flag(U.con_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (S.job_view && st.busy && st.job == AOS_FLASHER_JOB_BRIDGE) {
        big_button(U.actions, AOS_SYM_STOP, _("Cerrar el puente"), AOS_C_CARD2, AOS_C_TEXT, cancel_cb, NULL);
    } else if (S.job_view && st.busy && st.job == AOS_FLASHER_JOB_PROBE) {
        big_button(U.actions, AOS_SYM_STOP, _("Cerrar la sonda"), AOS_C_CARD2, AOS_C_TEXT, cancel_cb, NULL);
    } else if (S.job_view && st.busy) {
        big_button(U.actions, AOS_SYM_STOP, _("Cancelar"), AOS_C_CARD2, AOS_C_RED, cancel_cb, NULL);
    } else if (S.job_view) {
        /* SWD's console is the monitor; a serial one on its pins would fight it */
        if (st.family != AOS_FLASHER_SWD)
            big_button(U.actions, AOS_SYM_SERIAL_PORT, _("Monitor"), AOS_C_CARD2, AOS_C_TEXT, monitor_cb, NULL);
        big_button(U.actions, NULL, _("Listo"), C_TINT, lv_color_white(), done_cb, NULL);
    } else {
        const aos_flasher_source_t *s = selected();
        char who[32];
        bool can = s && !st.busy && !port_holder(who, sizeof who);
        char t[80];
        char title[64];
        if (s) source_title(s, title, sizeof title);
        if (s && s->dump) snprintf(t, sizeof t, "%s", _("Grabar el volcado"));
        else if (s) snprintf(t, sizeof t, "%s %s", _("Grabar"), title);
        else snprintf(t, sizeof t, "%s", _("Elegí un firmware"));
        U.main_btn = big_button(U.actions, AOS_SYM_FLASH, t, can ? C_TINT : AOS_C_CARD2, can ? lv_color_white() : AOS_C_DIM,
                                flash_cb, &U.main_lbl);
        if (!can) lv_obj_remove_flag(U.main_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static lv_obj_t *column(lv_obj_t *parent, int32_t x, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, 0);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_pad_hor(c, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(c, 28, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

static void header(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *h = box(parent, w, U.land ? 92 : 100);
    lv_obj_t *t = aos_label(h, _("Programador"), aos_font_large, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 4, 0);
}

static void actions_bar(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    U.actions = box(parent, w, h);
    lv_obj_set_pos(U.actions, x, y);
    U.bar_y = y;
    U.bar_h = h;
    lv_obj_set_style_pad_hor(U.actions, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.actions, 14, 0);
    lv_obj_set_flex_flow(U.actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(U.actions, 16, 0);
    lv_obj_set_style_bg_color(U.actions, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(U.actions, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(U.actions, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(U.actions, 1, 0);
    lv_obj_set_style_border_color(U.actions, lv_color_hex(0x1C1C1E), 0);
    build_actions();
}

static void build(void)
{
    lv_obj_clean(U.root);
    U.con_kb = U.con_ta = NULL;
    U.chip_name = U.chip_info = U.detect_btn = U.detect_lbl = U.busy_row = U.busy_lbl = U.busy_btn = NULL;
    U.port_lbl = U.baud_lbl = U.baud_chip = U.list = U.arc = U.log_col = U.actions = NULL;
    U.read_btn = U.erase_btn = NULL;
    memset(U.file_state, 0, sizeof U.file_state);
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    U.shown_seq = st.seq;
    U.shown_phase = st.phase;
    int32_t bar = U.land ? 128 : BAR_H;

    if (!U.land) {
        int32_t cw = U.W - 2 * AOS_UI_PAD;
        lv_obj_t *col = column(U.root, 0, U.W, U.H - bar);
        header(col, cw);
        build_target(col, cw, S.job_view);
        if (S.job_view) {
            build_progress(col, cw);
            build_files_state(col, cw);
            build_log(col, cw, 360);
        } else {
            build_sources(col, cw);
            build_wiring(col, cw);
        }
        actions_bar(U.root, 0, U.H - bar, U.W, bar);
    } else {
        /* the target (and the ring) over the button on the left; the list,
         * or the files and the log, on the right */
        int32_t lw = SIDE_W - AOS_UI_PAD - AOS_UI_PAD / 2, rw = U.W - SIDE_W, rcw = rw - AOS_UI_PAD - AOS_UI_PAD / 2;
        lv_obj_t *left = column(U.root, 0, SIDE_W, U.H - bar);
        lv_obj_set_style_pad_right(left, AOS_UI_PAD / 2, 0);
        lv_obj_set_style_pad_bottom(left, 8, 0);
        if (S.job_view) {
            lv_obj_set_style_pad_top(left, 16, 0);
            build_target(left, lw, true);
            build_progress(left, lw);
        } else {
            header(left, lw);
            build_target(left, lw, false);
        }
        actions_bar(U.root, 0, U.H - bar, SIDE_W, bar);
        lv_obj_set_style_pad_right(U.actions, AOS_UI_PAD / 2, 0);
        lv_obj_set_style_border_width(U.actions, 0, 0);

        lv_obj_t *right = column(U.root, SIDE_W, rw, U.H);
        lv_obj_set_style_pad_left(right, AOS_UI_PAD / 2, 0);
        lv_obj_set_style_pad_top(right, S.job_view ? 0 : 20, 0);
        if (S.job_view) {
            build_files_state(right, rcw);
            lv_obj_update_layout(right);
            int32_t used = 0;
            for (uint32_t i = 0; i < lv_obj_get_child_count(right); i++) used += lv_obj_get_height(lv_obj_get_child(right, i)) + 16;
            int32_t lh = U.H - used - 28 - 52;
            build_log(right, rcw, lh < 220 ? 220 : lh);
        } else {
            build_sources(right, rcw);
            build_wiring(right, rcw);
        }
    }
    refresh_job();
}

static void timer_cb(lv_timer_t *t)
{
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    if (st.busy != U.shown_busy) {
        /* a job ended or started: its buttons, and the list's chip marks */
        if (!S.job_view && !st.busy && st.job == AOS_FLASHER_JOB_DETECT) build();
        else if (U.actions) build_actions();
        U.shown_busy = st.busy;
    }
    if (st.busy || st.seq != U.shown_seq || (int)st.phase != U.shown_phase) {
        U.shown_seq = st.seq;
        U.shown_phase = st.phase;
        refresh_target();
        refresh_job();
    }
    const char *cur = aos_ui_current_app();
    if (!st.busy && S.job_view && cur && !strcmp(cur, APP_ID)) S.seen_seq = st.seq;    /* seen here: no toast */
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static void load_prefs(void)
{
    if (S.loaded) return;
    S.loaded = true;
    snprintf(S.port, sizeof S.port, "uart.b");
    /* the port of the "target" module, when modules.txt has one */
    const aos_io_module_t *m = aos_io_module_find("target");
    if (m) snprintf(S.port, sizeof S.port, "%s", m->port);
    aos_hal_pref_get_str("fl_port", S.port, sizeof S.port);
    int32_t b = 460800;
    aos_hal_pref_get_i32("fl_baud", &b);
    S.baud_i = 2;
    for (size_t i = 0; i < N_BAUDS; i++) if (BAUDS[i] == (uint32_t)b) S.baud_i = (int)i;
    aos_hal_pref_get_str("fl_sel", S.sel, sizeof S.sel);
    int32_t f = AOS_FLASHER_ESP;
    aos_hal_pref_get_i32("fl_fam", &f);
    S.fam = f >= 0 && f < AOS_FLASHER_FAMILIES ? (int)f : AOS_FLASHER_ESP;
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    S.seen_seq = st.seq;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    load_prefs();
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    if (!S.scanned) scan();
    /* a job running (started before a turn of the screen, or before the app
     * was closed) comes back to its own screen */
    if (aos_flasher_busy()) S.job_view = true;
    build();
    U.timer = lv_timer_create(timer_cb, 150, NULL);
    aos_ui_set_badge(APP_ID, 0);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    overlay_close();
    if (U.timer) lv_timer_delete(U.timer);
    memset(&U, 0, sizeof U);
}

/* Archivos opens a firmware here (aos_ui_open_app_with): a loose .bin, a
 * project folder, or any file inside one - the source that holds it gets
 * chosen. Not while a job runs: that screen stays. */
static void open_arg(const char *path)
{
    if (aos_flasher_busy()) return;
    scan();
    const aos_flasher_source_t *best = NULL;
    size_t pl = strlen(path);
    for (int i = 0; i < S.nsrc; i++) {
        const char *sp = S.src[i].path;
        size_t n = strlen(sp);
        bool holds = !strncmp(path, sp, n) && (path[n] == 0 || path[n] == '/');     /* a file of the source */
        bool inside = n > pl && !strncmp(sp, path, pl) && sp[pl] == '/';           /* its folder, above build/ */
        if (holds || inside) { best = &S.src[i]; break; }
    }
    if (!best) {
        aos_ui_toast(_("Ese archivo no es un firmware que se pueda grabar"), 2000);
        return;
    }
    if (!aos_flasher_source_fits(best, S.fam)) {
        S.fam = best->family >= 0 ? best->family : AOS_FLASHER_AVR;
        aos_hal_pref_set_i32("fl_fam", S.fam);
    }
    snprintf(S.sel, sizeof S.sel, "%s", best->path);
    aos_hal_pref_set_str("fl_sel", S.sel);
    S.job_view = false;
}

static void show(aos_app_t *self, void *inst)
{
    const char *arg = aos_ui_take_open_arg(APP_ID);
    if (arg) open_arg(arg);
    aos_ui_set_badge(APP_ID, 0);
    if (U.timer) lv_timer_resume(U.timer);
    if (!aos_flasher_busy() && !S.job_view) { scan(); build(); }
    else { refresh_target(); refresh_job(); if (U.actions) build_actions(); }
}

/* Out of sight the service tick keeps watch; the timer rests. */
static void hide(aos_app_t *self, void *inst)
{
    overlay_close();
    if (U.timer) lv_timer_pause(U.timer);
}

static bool back(aos_app_t *self, void *inst)
{
    if (S.job_view && !aos_flasher_busy()) { S.job_view = false; build(); return true; }
    return false;
}

/* From the main loop, 5 Hz, with the app closed or in the background: a job
 * that ended out of sight is told with a toast and a badge on the icon. */
void aos_app_flasher_service_tick(void)
{
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    if (st.busy || st.seq == S.seen_seq || st.job == AOS_FLASHER_JOB_DETECT || st.job == AOS_FLASHER_JOB_NONE) {
        if (!st.busy && st.job == AOS_FLASHER_JOB_DETECT) S.seen_seq = st.seq;
        return;
    }
    S.seen_seq = st.seq;
    const char *cur = aos_ui_current_app();
    if (cur && !strcmp(cur, APP_ID)) return;
    char msg[160];
    if (st.job == AOS_FLASHER_JOB_FLASH && st.ok) snprintf(msg, sizeof msg, _("Grabación terminada: %s en %s"), st.source, st.chip);
    else if (st.job == AOS_FLASHER_JOB_READ && st.ok) snprintf(msg, sizeof msg, _("Volcado de %s guardado en la tarjeta"), st.chip);
    else if (st.job == AOS_FLASHER_JOB_ERASE && st.ok) snprintf(msg, sizeof msg, _("Flash de %s borrada"), st.chip);
    else if (st.phase == AOS_FLASHER_CANCELLED) snprintf(msg, sizeof msg, "%s", phase_title(&st));
    else snprintf(msg, sizeof msg, "%s: %s", phase_title(&st), st.error);
    aos_ui_toast(msg, 4000);
    aos_ui_set_badge(APP_ID, 1);
}

void aos_app_flasher_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = APP_ID, .name = "Programador", .icon = AOS_SYM_CHIP,
            .color_a = 0x8B5CF6, .color_b = 0x4C1D95,
            .flags = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_KEEP_AWAKE,
            .order = 120,
        },
        .create = create, .destroy = destroy, .show = show, .hide = hide, .back = back,
    };
}
