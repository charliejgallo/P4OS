/*
 * P4OS - Bus: the bench's I2C, SPI and GPIO tools, over aos_io.
 *
 *   I2C    an i2cdetect of any I2C port (the header's i2c.ext, the board's
 *          own bus, whatever modules.txt adds), what usually lives at each
 *          address, and a register view of one device: 256 bytes read in
 *          16-byte blocks, the ones that changed since the last read marked,
 *          optionally re-read twice a second, and any byte writable from a
 *          hex keypad.
 *   SPI    one transfer at a time on an SPI port: clock, mode, chip select
 *          and bit order, the bytes to send typed on a hex keypad, and what
 *          came back in hex and ASCII. Presets fill and send the transfers
 *          that answer the first question about a chip - a flash's JEDEC id,
 *          a MAX31855's temperatures, an MCP3008 channel - and the loopback
 *          that tests the port itself with a jumper from pin 34 to pin 36,
 *          and say what the bytes mean. A reply of all 0xFF or all 0x00 is
 *          explained as what it usually is: nobody there.
 *   GPIO   the 40-pin header in its physical order. The pins nobody holds
 *          can be made inputs (plain, pull-up, pull-down) or outputs, with
 *          their level shown live and an output flipped by a tap. They stay
 *          as they were left when the app closes - a relay driven from here
 *          keeps its state - until "Soltar" gives them back.
 *
 * The bus work runs in a thread of its own, one job at a time: a probe of 112
 * addresses or a stuck bus with its timeouts would otherwise freeze the
 * screen. A timer picks the results up.
 *
 * Still to come (APPS.md): PWM and the ADC as a voltmeter, which need their
 * own calls in aos_io first.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_io.h"
#include "aos_sys_glyphs.h"
#include "aos_mono.h"

#include <stdio.h>
#include <string.h>

#define OWNER       "Bus"
/* The SPI side claims its pins under a name of its own, so that a pin the
 * GPIO panel holds as "Bus" is refused with its owner named instead of being
 * quietly taken over by the transfer. */
#define OWNER_SPI   "Bus SPI"
#define C_AMBER     lv_color_hex(0xF59E0B)
#define C_FOUND     lv_color_hex(0x15803D)
#define C_BOARD     lv_color_hex(0x1D4ED8)
#define C_CHANGED   lv_color_hex(0xB45309)

enum { TAB_I2C, TAB_SPI, TAB_GPIO, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { "I2C", "SPI", "GPIO" };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_CONNECTION, AOS_SYM_SWAP_HORIZONTAL, AOS_SYM_CHIP };

/* SPI clocks the stepper offers */
static const uint32_t CLOCKS[] = { 100000, 250000, 500000, 1000000, 2000000, 4000000, 5000000,
                                   8000000, 10000000, 20000000, 40000000 };
#define N_CLOCKS  (int)(sizeof CLOCKS / sizeof CLOCKS[0])
#define CLOCK_1M  3
#define SPI_MAX   64                        /* bytes in one transfer from here */

/* What a transfer was for, so its reply can be read */
enum { P_NONE = -1, P_FLASH, P_TC, P_ADC, P_LOOP, P_COUNT };

/* Why a transfer did not happen */
enum { E_NONE, E_PORT, E_PIN, E_CS_MOD, E_OPEN, E_XFER };

/* GPIO modes as the app cycles them; M_NONE gives the pin back */
enum { M_NONE, M_IN, M_UP, M_DOWN, M_OUT, M_COUNT };
static const char *const MODE_NAME[M_COUNT] = { N_("libre"), "IN", "IN ↑", "IN ↓", "OUT" };

/* What survives closing the app and turning the screen */
static struct {
    int tab;
    char port[AOS_IO_PORT_NAME_MAX];        /* the I2C port being looked at */
    bool scanned, found[128];
    int dev;                                /* -1: the address list */
    bool regs_ok, have_prev, live;
    uint8_t regs[256], prev[256];
    uint8_t mode[64], level[64];
    /* SPI: the settings, the bytes to send, the last reply */
    char spi_port[AOS_IO_PORT_NAME_MAX];
    char spi_cs[24];                        /* "": the port's CS, "-": none, else a module's */
    int spi_clk, spi_mode, adc_ch;
    bool spi_lsb;
    uint8_t tx[SPI_MAX];
    int txn;
    bool have;                              /* a reply to show */
    int res_preset, res_ch, res_err, res_err_gpio, res_n;
    char res_err_who[24], res_desc[48];
    uint8_t res_tx[SPI_MAX], res_rx[SPI_MAX];
} S;
/* Not initialised in place, so that it is .bss and goes to PSRAM (psram.lf). */
__attribute__((constructor)) static void S_defaults(void)
{
    S.dev = -1;
    S.spi_clk = CLOCK_1M;
    S.res_preset = P_NONE;
    static const uint8_t JEDEC[4] = { 0x9F, 0, 0, 0 };
    memcpy(S.tx, JEDEC, sizeof JEDEC);
    S.txn = sizeof JEDEC;
}

static struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT], *overlay;
    lv_timer_t *timer;
    int32_t W, H, PW;
    bool land;
    lv_obj_t *status, *scan_btn;
    lv_obj_t *addr[128];
    lv_obj_t *reg[256];
    lv_obj_t *pin_mode[64], *pin_led[64];
    uint32_t seen_seq, live_ms;
    /* keypad */
    uint8_t wr_reg;
    char wr_text[3];
    bool wr_fresh;                  /* the shown value is the old one: a key replaces it */
    lv_obj_t *wr_value;
    /* SPI */
    lv_obj_t *clk_val, *mode_seg[4], *cs_val, *bit_seg[2], *tx_hex, *tx_count, *adc_chip;
    lv_obj_t *res_desc, *res_tx, *res_rx, *res_ascii, *res_note;
    uint8_t kp_bytes[SPI_MAX];      /* the hex keypad's copy while it is open */
    int kp_n;
    int kp_nib;                     /* -1: no half byte typed */
} U;

static void build_page(void);

/* -------------------------------------------------------------------------- */
/* The worker                                                                  */
/* -------------------------------------------------------------------------- */

enum { JOB_SCAN = 1, JOB_DUMP, JOB_WRITE, JOB_SPI };

static struct {
    volatile bool busy;
    volatile uint32_t seq;          /* bumped when a job ends */
    int job;
    char port[AOS_IO_PORT_NAME_MAX];
    uint8_t addr, reg, val;
    /* results */
    bool ok, found[128];
    uint8_t regs[256];
    /* SPI: the transfer and what came of it */
    aos_io_spi_cfg_t cfg;
    char cs_mod[24];
    uint8_t tx[SPI_MAX], rx[SPI_MAX];
    int n, err, err_gpio, preset, ch;
    char err_who[24], desc[48];
} J;

/* Why aos_io_spi_open() said no, from what the pins and modules.txt say now:
 * the port missing, one of its pins (or the CS) held by someone else, a
 * module without a usable cs=, or something lower down (the log has it). */
static void spi_why(void)
{
    const aos_io_port_t *p = aos_io_port_find(J.port);
    if (!p || p->kind != AOS_PORT_SPI) { J.err = E_PORT; return; }
    int cs = J.cfg.cs == AOS_IO_SPI_CS_NONE ? -1 : J.cs_mod[0] ? aos_io_module_gpio(J.cs_mod, "cs") : p->pins[3];
    if (J.cs_mod[0] && cs < 0) { J.err = E_CS_MOD; return; }
    int pins[4] = { p->pins[0], p->pins[1], p->pins[2], cs };
    for (int k = 0; k < 4; k++) {
        const char *o = pins[k] >= 0 ? aos_io_owner(pins[k]) : NULL;
        if (o && strcmp(o, OWNER_SPI)) {
            J.err = E_PIN;
            J.err_gpio = pins[k];
            snprintf(J.err_who, sizeof J.err_who, "%s", o);
            return;
        }
    }
    J.err = E_OPEN;
}

static void spi_job(void)
{
    J.err = E_NONE;
    J.desc[0] = 0;
    J.cfg.module = J.cs_mod[0] ? J.cs_mod : NULL;
    aos_io_spi_t *sp = aos_io_spi_open(J.port, OWNER_SPI, &J.cfg);
    if (!sp) { spi_why(); J.ok = false; return; }
    snprintf(J.desc, sizeof J.desc, "%s", aos_io_spi_desc(sp));
    memset(J.rx, 0, sizeof J.rx);
    J.ok = aos_io_spi_xfer(sp, J.tx, J.rx, (size_t)J.n, 200);
    if (!J.ok) J.err = E_XFER;
    aos_io_spi_close(sp);
}

static void worker(void *arg)
{
    (void)arg;
    if (J.job == JOB_SPI) {
        spi_job();
        J.busy = false;
        J.seq++;
        return;
    }
    aos_io_i2c_t *b = aos_io_i2c_open(J.port, OWNER);
    J.ok = b != NULL;
    if (b) {
        switch (J.job) {
        case JOB_SCAN:
            for (int a = 0; a < 128; a++) J.found[a] = a >= 0x08 && a <= 0x77 && aos_io_i2c_probe(b, (uint8_t)a);
            break;
        case JOB_WRITE: {
            uint8_t w[2] = { J.reg, J.val };
            J.ok = aos_io_i2c_xfer(b, J.addr, w, 2, NULL, 0, 50);
            if (!J.ok) break;
        }   /* fall through: read it all back */
        /* fallthrough */
        case JOB_DUMP:
            J.ok = true;
            for (int r = 0; r < 256 && J.ok; r += 16) {
                uint8_t w = (uint8_t)r;
                J.ok = aos_io_i2c_xfer(b, J.addr, &w, 1, &J.regs[r], 16, 50);
            }
            break;
        }
        aos_io_i2c_close(b);
    }
    J.busy = false;
    J.seq++;
}

static bool job_start(int job, uint8_t addr, uint8_t reg, uint8_t val)
{
    if (J.busy) return false;
    J.busy = true;
    J.job = job;
    snprintf(J.port, sizeof J.port, "%s", S.port);
    J.addr = addr;
    J.reg = reg;
    J.val = val;
    if (!aos_hal_thread_start("bus", worker, NULL, 4096, 4)) {
        J.busy = false;
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Small pieces                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) {
        lv_obj_t *g = aos_label(b, glyph, &aos_sym_28, lv_color_white());
        aos_make_decorative(g);
    }
    aos_make_decorative(aos_label(b, text, aos_font_body, lv_color_white()));
    return b;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* A flex column that scrolls, for one side of a page. */
static lv_obj_t *column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

static int i2c_ports(const aos_io_port_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < aos_io_port_count() && n < max; i++)
        if (aos_io_port_at(i)->kind == AOS_PORT_I2C) out[n++] = aos_io_port_at(i);
    return n;
}

static bool board_bus(void) { return !strcmp(S.port, "i2c.board"); }

/* -------------------------------------------------------------------------- */
/* I2C: the address grid                                                       */
/* -------------------------------------------------------------------------- */

static void status_refresh(void)
{
    if (!U.status) return;
    const aos_io_port_t *pt = aos_io_port_find(S.port);
    char pins[48] = "";
    if (pt) snprintf(pins, sizeof pins, "SDA %d · SCL %d · %u kHz", pt->pins[0], pt->pins[1], (unsigned)(pt->freq / 1000));
    if (J.busy && J.job == JOB_SCAN) {
        lv_label_set_text_fmt(U.status, "%s   %s", _("Buscando…"), pins);
    } else if (!S.scanned) {
        lv_label_set_text_fmt(U.status, "%s   %s", _("Sin escanear"), pins);
    } else {
        int n = 0;
        for (int a = 0; a < 128; a++) n += S.found[a];
        if (n == 1) lv_label_set_text_fmt(U.status, "%s   %s", _("Respondió 1 dirección"), pins);
        else lv_label_set_text_fmt(U.status, _("Respondieron %d direcciones   %s"), n, pins);
    }
}

static void grid_refresh(void)
{
    for (int a = 0; a < 128; a++) {
        lv_obj_t *c = U.addr[a];
        if (!c) continue;
        bool valid = a >= 0x08 && a <= 0x77;
        bool f = S.scanned && S.found[a];
        bool known = f && board_bus() && aos_io_i2c_guess((uint8_t)a, true)[0];
        lv_obj_set_style_bg_color(c, f ? (known ? C_BOARD : C_FOUND) : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(c, f ? LV_OPA_COVER : valid ? LV_OPA_50 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(c, 0), f ? lv_color_white() : valid ? AOS_C_DIM : lv_color_hex(0x3A3A3C), 0);
    }
}

static void open_dev(int addr)
{
    S.dev = addr;
    S.regs_ok = S.have_prev = false;
    S.live = false;
    build_page();
    job_start(JOB_DUMP, (uint8_t)addr, 0, 0);
}

static void addr_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (S.scanned && S.found[a]) open_dev(a);
}

static void dev_row_cb(lv_event_t *e) { open_dev((int)(intptr_t)lv_event_get_user_data(e)); }

static void scan_cb(lv_event_t *e)
{
    if (job_start(JOB_SCAN, 0, 0, 0)) status_refresh();
}

static void port_cb(lv_event_t *e)
{
    const aos_io_port_t *ports[AOS_IO_PORT_MAX];
    int n = i2c_ports(ports, AOS_IO_PORT_MAX);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= n || J.busy) return;
    snprintf(S.port, sizeof S.port, "%s", ports[i]->name);
    S.scanned = false;
    build_page();
    job_start(JOB_SCAN, 0, 0, 0);
    status_refresh();
}

static void build_grid(lv_obj_t *parent, int32_t w)
{
    const int32_t lab = 58, cw = (w - 2 * 14 - lab) / 16, ch = cw + 4;
    lv_obj_t *g = card(parent, w, 14 * 2 + 9 * ch);
    lv_obj_set_style_pad_all(g, 14, 0);
    for (int c = 0; c < 16; c++) {
        lv_obj_t *h = aos_label(g, "", aos_font_tiny, AOS_C_DIM);
        lv_label_set_text_fmt(h, "%X", c);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(h, cw);
        lv_obj_set_pos(h, lab + c * cw, (ch - 18) / 2);
    }
    for (int r = 0; r < 8; r++) {
        lv_obj_t *l = aos_label(g, "", aos_font_tiny, AOS_C_DIM);
        lv_label_set_text_fmt(l, "%X0", r);
        lv_obj_set_pos(l, 6, (r + 1) * ch + (ch - 18) / 2);
        for (int c = 0; c < 16; c++) {
            int a = r * 16 + c;
            lv_obj_t *cell = box(g, cw - 4, ch - 4);
            lv_obj_set_pos(cell, lab + c * cw + 2, (r + 1) * ch + 2);
            lv_obj_set_style_radius(cell, 8, 0);
            lv_obj_t *t = lv_label_create(cell);
            lv_obj_set_style_text_font(t, aos_font_tiny, 0);
            lv_label_set_text_fmt(t, "%02X", a);
            lv_obj_center(t);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, addr_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
            U.addr[a] = cell;
        }
    }
    grid_refresh();
}

static void build_found(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *t = aos_label(parent, _("DISPOSITIVOS"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 12, 0);
    lv_obj_t *g = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    int n = 0;
    for (int a = 0; a < 128; a++) {
        if (!S.scanned || !S.found[a]) continue;
        lv_obj_t *r = box(g, lv_pct(100), AOS_UI_ROW_H);
        lv_obj_set_style_pad_hor(r, 22, 0);
        if (n++) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        }
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, dev_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
        lv_obj_t *ad = lv_label_create(r);
        lv_obj_set_style_text_font(ad, &aos_mono_22, 0);
        lv_obj_set_style_text_color(ad, C_AMBER, 0);
        lv_label_set_text_fmt(ad, "0x%02X", a);
        lv_obj_align(ad, LV_ALIGN_LEFT_MID, 0, 0);
        const char *guess = aos_io_i2c_guess((uint8_t)a, board_bus());
        lv_obj_t *gl = aos_label(r, guess[0] ? guess : _("desconocido"), aos_font_body, guess[0] ? AOS_C_TEXT : AOS_C_DIM);
        lv_obj_set_width(gl, w - 44 - 110 - 40);
        lv_label_set_long_mode(gl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(gl, LV_ALIGN_LEFT_MID, 110, 0);
        lv_obj_t *c = aos_label(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (!n) {
        lv_obj_t *r = box(g, lv_pct(100), AOS_UI_ROW_H);
        lv_obj_set_style_pad_hor(r, 22, 0);
        lv_obj_t *l = aos_label(r, S.scanned ? _("Nadie respondió. ¿Pull-ups en SDA y SCL? ¿El módulo tiene 3V3?")
                                             : _("Tocá Escanear."), aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, w - 44);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

static void build_i2c_top(lv_obj_t *parent, int32_t w)
{
    const aos_io_port_t *ports[AOS_IO_PORT_MAX];
    int n = i2c_ports(ports, AOS_IO_PORT_MAX);
    lv_obj_t *row = box(parent, w, 80);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    /* segmented control of the ports */
    lv_obj_t *seg = box(row, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_bg_color(seg, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(seg, 36, 0);
    lv_obj_set_style_pad_all(seg, 6, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(seg, 1);
    for (int i = 0; i < n; i++) {
        bool on = !strcmp(ports[i]->name, S.port);
        lv_obj_t *s = box(seg, LV_SIZE_CONTENT, 60);
        lv_obj_set_flex_grow(s, 1);
        lv_obj_set_style_radius(s, 30, 0);
        lv_obj_set_style_bg_color(s, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(s, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s, port_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = aos_label(s, ports[i]->name, aos_font_small, on ? AOS_C_TEXT : AOS_C_DIM);
        lv_obj_center(l);
    }
    U.scan_btn = pill(row, AOS_SYM_MAGNIFY, _("Escanear"), C_AMBER, scan_cb, NULL);
    U.status = caption(parent, "", w);
    lv_obj_set_style_pad_left(U.status, 12, 0);
    status_refresh();
}

/* -------------------------------------------------------------------------- */
/* I2C: one device's registers                                                 */
/* -------------------------------------------------------------------------- */

static void regs_refresh(void)
{
    for (int r = 0; r < 256; r++) {
        lv_obj_t *c = U.reg[r];
        if (!c) continue;
        lv_obj_t *t = lv_obj_get_child(c, 0);
        if (!S.regs_ok) { lv_label_set_text(t, "··"); lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0); continue; }
        lv_label_set_text_fmt(t, "%02X", S.regs[r]);
        bool ch = S.have_prev && S.prev[r] != S.regs[r];
        lv_obj_set_style_bg_color(c, C_CHANGED, 0);
        lv_obj_set_style_bg_opa(c, ch ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(t, ch ? lv_color_white() : S.regs[r] ? AOS_C_TEXT : AOS_C_DIM, 0);
    }
    if (U.status) {
        if (J.busy) lv_label_set_text(U.status, _("Leyendo…"));
        else if (!S.regs_ok) lv_label_set_text(U.status, _("No contestó a la lectura de registros. Hay chips que no usan registros (los SHT3x, por ejemplo, reciben comandos)."));
        else lv_label_set_text(U.status, _("Tocá un byte para escribirlo. En naranja, lo que cambió desde la lectura anterior."));
    }
}

static void read_cb(lv_event_t *e) { job_start(JOB_DUMP, (uint8_t)S.dev, 0, 0); }
static void live_cb(lv_event_t *e) { S.live = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); }
static void dev_back_cb(lv_event_t *e) { S.dev = -1; S.live = false; build_page(); }

/* The hex keypad that writes one register */
static void kp_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
    U.wr_value = NULL;
}

static void kp_show(void)
{
    if (U.wr_value) lv_label_set_text_fmt(U.wr_value, "0x%s%s", U.wr_text, strlen(U.wr_text) < 2 ? "_" : "");
}

static void kp_key_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    size_t n = strlen(U.wr_text);
    if (k < 16 && U.wr_fresh) { U.wr_text[0] = 0; n = 0; }
    if (k <= 16) U.wr_fresh = false;
    if (k < 16) {
        if (n < 2) { U.wr_text[n] = "0123456789ABCDEF"[k]; U.wr_text[n + 1] = 0; }
    } else if (k == 16) {
        if (n) U.wr_text[n - 1] = 0;
    } else if (k == 17) {
        kp_close();
        return;
    } else if (n) {
        unsigned v = 0;
        sscanf(U.wr_text, "%x", &v);
        kp_close();
        if (job_start(JOB_WRITE, (uint8_t)S.dev, U.wr_reg, (uint8_t)v)) {
            char t[48];
            snprintf(t, sizeof t, "0x%02X ← 0x%02X", U.wr_reg, v);
            aos_ui_toast(t, 1200);
        }
        return;
    }
    kp_show();
}

static void reg_cb(lv_event_t *e)
{
    if (!S.regs_ok) return;
    U.wr_reg = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    snprintf(U.wr_text, sizeof U.wr_text, "%02X", S.regs[U.wr_reg]);
    U.wr_fresh = true;
    S.live = false;
    kp_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    const int32_t kw = U.land ? 132 : 150, kh = U.land ? 84 : 104, gap = 12;
    lv_obj_t *sh = card(U.overlay, 4 * kw + 5 * gap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sh, gap, 0);
    lv_obj_set_style_pad_top(sh, 24, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(sh, gap, 0);
    lv_obj_center(sh);
    lv_obj_t *t = aos_label(sh, "", aos_font_small, AOS_C_DIM);
    lv_label_set_text_fmt(t, _("Escribir en 0x%02X, registro 0x%02X"), S.dev, U.wr_reg);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    U.wr_value = aos_label(sh, "", aos_font_huge, C_AMBER);
    lv_obj_set_width(U.wr_value, lv_pct(100));
    lv_obj_set_style_text_align(U.wr_value, LV_TEXT_ALIGN_CENTER, 0);
    kp_show();
    static const char *const EXTRA[3] = { LV_SYMBOL_BACKSPACE, N_("Cancelar"), N_("Escribir") };
    for (int k = 0; k < 19; k++) {
        bool wide = k >= 17;
        lv_obj_t *b = box(sh, wide ? 2 * kw + gap : kw, kh);
        if (k == 16) lv_obj_set_width(b, 4 * kw + 3 * gap);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, k == 18 ? C_AMBER : k >= 16 ? AOS_C_CARD2 : lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, kp_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_obj_t *l;
        if (k < 16) {
            l = lv_label_create(b);
            lv_obj_set_style_text_font(l, aos_font_title, 0);
            lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
            lv_label_set_text_fmt(l, "%X", k);
        } else if (k == 16) {
            l = aos_label(b, EXTRA[0], aos_font_title, AOS_C_TEXT);
        } else {
            l = aos_label(b, aos_tr(EXTRA[k - 16]), aos_font_body, AOS_C_TEXT);
        }
        lv_obj_center(l);
    }
}

static void build_regs(lv_obj_t *parent, int32_t w)
{
    const int32_t lab = 60, cw = (w - 2 * 12 - lab) / 16, ch = U.land ? 36 : 40;
    lv_obj_t *g = card(parent, w, 12 * 2 + 17 * ch);
    lv_obj_set_style_pad_all(g, 12, 0);
    for (int c = 0; c < 16; c++) {
        lv_obj_t *h = lv_label_create(g);
        lv_obj_set_style_text_font(h, &aos_mono_18, 0);
        lv_obj_set_style_text_color(h, AOS_C_DIM, 0);
        lv_label_set_text_fmt(h, "%X", c);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(h, cw);
        lv_obj_set_pos(h, lab + c * cw, (ch - 22) / 2);
    }
    for (int r = 0; r < 16; r++) {
        lv_obj_t *l = lv_label_create(g);
        lv_obj_set_style_text_font(l, &aos_mono_18, 0);
        lv_obj_set_style_text_color(l, C_AMBER, 0);
        lv_label_set_text_fmt(l, "%X0", r);
        lv_obj_set_pos(l, 4, (r + 1) * ch + (ch - 22) / 2);
        for (int c = 0; c < 16; c++) {
            int a = r * 16 + c;
            lv_obj_t *cell = box(g, cw - 2, ch - 4);
            lv_obj_set_pos(cell, lab + c * cw + 1, (r + 1) * ch + 2);
            lv_obj_set_style_radius(cell, 6, 0);
            lv_obj_t *t = lv_label_create(cell);
            lv_obj_set_style_text_font(t, &aos_mono_18, 0);
            lv_obj_center(t);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, reg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
            U.reg[a] = cell;
        }
    }
}

static void build_dev_head(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 60);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, dev_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = aos_label(b, "", aos_font_body, AOS_C_ACCENT);
    lv_label_set_text_fmt(bl, LV_SYMBOL_LEFT "  %s", S.port);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *t = lv_label_create(parent);
    lv_obj_set_style_text_font(t, aos_font_large, 0);
    lv_obj_set_style_text_color(t, AOS_C_TEXT, 0);
    lv_label_set_text_fmt(t, "0x%02X", S.dev);
    const char *guess = aos_io_i2c_guess((uint8_t)S.dev, board_bus());
    lv_obj_t *gl = aos_label(parent, guess[0] ? guess : _("Dispositivo desconocido"), aos_font_body, AOS_C_DIM);
    lv_obj_set_width(gl, w);
    lv_label_set_long_mode(gl, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *row = box(parent, w, 80);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    pill(row, AOS_SYM_RESTART, _("Leer"), C_AMBER, read_cb, NULL);
    lv_obj_t *lv = card(row, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_radius(lv, 38, 0);
    lv_obj_set_style_pad_hor(lv, 24, 0);
    lv_obj_set_flex_flow(lv, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lv, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(lv, 16, 0);
    aos_label(lv, _("En vivo"), aos_font_body, AOS_C_TEXT);
    lv_obj_t *sw = lv_switch_create(lv);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_set_style_bg_color(sw, C_AMBER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, S.live);
    lv_obj_add_event_cb(sw, live_cb, LV_EVENT_VALUE_CHANGED, NULL);

    U.status = caption(parent, "", w);
}

/* -------------------------------------------------------------------------- */
/* SPI                                                                         */
/* -------------------------------------------------------------------------- */

static int spi_ports(const aos_io_port_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < aos_io_port_count() && n < max; i++)
        if (aos_io_port_at(i)->kind == AOS_PORT_SPI) out[n++] = aos_io_port_at(i);
    return n;
}

/* The chip selects on offer: the port's own cs=, the cs= of each module on
 * the port (a second chip on the same wires), and none at all. */
typedef struct { char mod[24]; int gpio; } cs_opt_t;

static int cs_options(cs_opt_t *o, int max)
{
    int n = 0;
    const aos_io_port_t *p = aos_io_port_find(S.spi_port);
    if (!p) return 0;
    if (p->pins[3] >= 0 && n < max) { o[n].mod[0] = 0; o[n++].gpio = p->pins[3]; }
    for (int i = 0; i < aos_io_module_count() && n < max - 1; i++) {
        const aos_io_module_t *m = aos_io_module_at(i);
        int g = strcmp(m->port, p->name) ? -1 : aos_io_module_gpio(m->name, "cs");
        if (g < 0) continue;
        snprintf(o[n].mod, sizeof o[n].mod, "%s", m->name);
        o[n++].gpio = g;
    }
    snprintf(o[n].mod, sizeof o[n].mod, "-");
    o[n++].gpio = -1;
    return n;
}

static int cs_current(const cs_opt_t *o, int n)
{
    for (int i = 0; i < n; i++) if (!strcmp(o[i].mod, S.spi_cs)) return i;
    return 0;
}

static void fmt_clock(char *out, size_t n, uint32_t hz)
{
    if (hz >= 1000000) snprintf(out, n, "%u MHz", (unsigned)(hz / 1000000));
    else snprintf(out, n, "%u kHz", (unsigned)(hz / 1000));
}

static void hex_line(char *out, size_t cap, const uint8_t *b, int n)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n && o + 4 < cap; i++) o += (size_t)snprintf(out + o, cap - o, i ? " %02X" : "%02X", b[i]);
}

static void ascii_line(char *out, size_t cap, const uint8_t *b, int n)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 3 < cap; i++) {
        if (b[i] >= 0x20 && b[i] < 0x7F) out[o++] = (char)b[i];
        else { out[o++] = (char)0xC2; out[o++] = (char)0xB7; }      /* a middle dot, UTF-8 */
    }
    out[o] = 0;
}

static bool spi_send(int preset)
{
    if (J.busy || !S.txn) return false;
    cs_opt_t o[AOS_IO_MODULE_MAX + 2];
    int n = cs_options(o, AOS_IO_MODULE_MAX + 2);
    if (!n) return false;
    const cs_opt_t *c = &o[cs_current(o, n)];
    J.busy = true;
    J.job = JOB_SPI;
    snprintf(J.port, sizeof J.port, "%s", S.spi_port);
    J.cfg = (aos_io_spi_cfg_t){
        .clock_hz = CLOCKS[S.spi_clk], .mode = (uint8_t)S.spi_mode, .lsb_first = S.spi_lsb,
        .cs = c->gpio < 0 ? AOS_IO_SPI_CS_NONE : AOS_IO_SPI_CS_PORT,
    };
    snprintf(J.cs_mod, sizeof J.cs_mod, "%s", c->gpio < 0 ? "" : c->mod);
    memcpy(J.tx, S.tx, (size_t)S.txn);
    J.n = S.txn;
    J.preset = preset;
    J.ch = S.adc_ch;
    if (!aos_hal_thread_start("bus", worker, NULL, 4096, 4)) {
        J.busy = false;
        return false;
    }
    if (U.res_note) {
        lv_label_set_text(U.res_note, _("Enviando…"));
        lv_obj_set_style_text_color(U.res_note, AOS_C_DIM, 0);
    }
    return true;
}

/* What the reply says. First the two answers that mean "nobody there",
 * whatever was asked; then, after a preset, what the chip said. */
static void spi_decode(char *t, size_t n, lv_color_t *col)
{
    const uint8_t *rx = S.res_rx, *tx = S.res_tx;
    int len = S.res_n;
    *col = AOS_C_DIM;
    switch (S.res_err) {
    case E_PORT: snprintf(t, n, _("%s no está en modules.txt"), S.spi_port); *col = AOS_C_RED; return;
    case E_PIN: snprintf(t, n, _("GPIO%d lo tiene %s"), S.res_err_gpio, S.res_err_who); *col = AOS_C_RED; return;
    case E_CS_MOD: snprintf(t, n, _("El módulo %s no tiene un cs= que sirva"), S.spi_cs); *col = AOS_C_RED; return;
    case E_OPEN: snprintf(t, n, "%s", _("No se pudo abrir el puerto")); *col = AOS_C_RED; return;
    case E_XFER: snprintf(t, n, "%s", _("La transferencia falló (el registro dice por qué)")); *col = AOS_C_RED; return;
    default: break;
    }
    /* the loopback counts bytes; MISO stuck at one level is no loop at all */
    if (S.res_preset == P_LOOP) {
        int same = 0, ff = 0, zero = 0;
        for (int i = 0; i < len; i++) { same += rx[i] == tx[i]; ff += rx[i] == 0xFF; zero += rx[i] == 0x00; }
        if (same == len) { snprintf(t, n, _("Lazo bien: volvieron los %d bytes"), len); *col = AOS_C_GREEN; }
        else if (same <= 1 || ff == len || zero == len) { snprintf(t, n, "%s", _("No volvió nada. Puenteá el pin 34 (MOSI) con el 36 (MISO).")); *col = AOS_C_ORANGE; }
        else { snprintf(t, n, _("Volvieron bien %d de %d bytes: ¿cable flojo, o el reloj alto para el cable?"), same, len); *col = AOS_C_ORANGE; }
        return;
    }
    /* a 9F typed by hand is a JEDEC id read all the same */
    int preset = S.res_preset;
    if (preset == P_NONE && len >= 4 && tx[0] == 0x9F) preset = P_FLASH;
    bool ff = true, zero = true;
    int from = preset == P_FLASH ? 1 : preset == P_ADC ? 1 : 0;   /* what counts as the chip's reply */
    for (int i = from; i < len; i++) { ff &= rx[i] == 0xFF; zero &= rx[i] == 0x00; }
    if (len > from && ff) { snprintf(t, n, "%s", _("Todo 0xFF: no contestó nadie (MISO quedó en 1). ¿Alimentación, CS, cables?")); *col = AOS_C_ORANGE; return; }
    if (len > from && zero) { snprintf(t, n, "%s", _("Todo 0x00: MISO quedó en 0. ¿Está a masa, o el chip sin alimentar?")); *col = AOS_C_ORANGE; return; }
    switch (preset) {
    case P_FLASH: {
        if (len < 4) break;
        static const struct { uint8_t id; const char *name; } MAKER[] = {
            { 0xEF, "Winbond" }, { 0xC2, "Macronix" }, { 0xC8, "GigaDevice" }, { 0x20, "Micron / XMC" },
            { 0x1F, "Adesto" }, { 0xBF, "SST / Microchip" }, { 0x9D, "ISSI" }, { 0x68, "Boya" }, { 0x0B, "XTX" },
            { 0x85, "Puya" }, { 0x5E, "Zbit" }, { 0x01, "Spansion / Cypress" },
        };
        const char *mk = NULL;
        for (size_t i = 0; i < sizeof MAKER / sizeof MAKER[0]; i++) if (MAKER[i].id == rx[1]) mk = MAKER[i].name;
        char size[24] = "";
        if (rx[3] >= 0x10 && rx[3] <= 0x1F) {
            uint32_t bytes = 1u << rx[3];
            if (bytes >= 1048576) snprintf(size, sizeof size, "%u MB", (unsigned)(bytes >> 20));
            else snprintf(size, sizeof size, "%u KB", (unsigned)(bytes >> 10));
        }
        /* Winbond's W25Qxx: the number is the size in megabits */
        if (rx[1] == 0xEF && (rx[2] == 0x40 || rx[2] == 0x70) && rx[3] >= 0x14 && rx[3] <= 0x19)
            snprintf(t, n, "Winbond W25Q%u · %s", (unsigned)((1u << rx[3]) >> 17), size);
        else
            snprintf(t, n, _("Fabricante %02X%s%s%s, tipo %02X%s%s"), rx[1], mk ? " (" : "", mk ? mk : "", mk ? ")" : "",
                     rx[2], size[0] ? " · " : "", size);
        *col = AOS_C_GREEN;
        return;
    }
    case P_TC: {
        if (len < 4) break;
        uint32_t w = (uint32_t)rx[0] << 24 | (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3];
        if (w & 0x10000) {
            if (w & 1) snprintf(t, n, "%s", _("La termocupla está abierta (o no está conectada)"));
            else if (w & 2) snprintf(t, n, "%s", _("La termocupla está en corto a masa"));
            else if (w & 4) snprintf(t, n, "%s", _("La termocupla está en corto a VCC"));
            else snprintf(t, n, "%s", _("El MAX31855 marca una falla"));
            *col = AOS_C_ORANGE;
            return;
        }
        int32_t tc = (int32_t)w >> 18, cj = (int32_t)(w << 16) >> 20;
        snprintf(t, n, _("Termocupla %.2f °C · chip %.2f °C"), (double)(tc * 0.25f), (double)(cj * 0.0625f));
        *col = AOS_C_GREEN;
        return;
    }
    case P_ADC: {
        if (len < 3) break;
        int code = (rx[1] & 3) << 8 | rx[2];
        snprintf(t, n, _("Canal %d: %d de 1023\n%.3f V con VREF de 3.3 V"), S.res_ch, code, (double)(code * 3.3f / 1024.0f));
        *col = AOS_C_GREEN;
        return;
    }
    default: break;
    }
    t[0] = 0;
}

static void res_show(bool on)
{
    lv_obj_t *const parts[4] = { lv_obj_get_parent(U.res_tx), lv_obj_get_parent(U.res_rx),
                                 lv_obj_get_parent(U.res_ascii), U.res_desc };
    for (int i = 0; i < 4; i++) {
        if (on) lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void res_refresh(void)
{
    if (!U.res_note) return;
    res_show(S.have);
    if (S.have && S.res_err != E_NONE && S.res_err != E_XFER) {
        /* nothing went out: only what was meant to, and why not */
        lv_obj_add_flag(lv_obj_get_parent(U.res_rx), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lv_obj_get_parent(U.res_ascii), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.res_desc, LV_OBJ_FLAG_HIDDEN);
    }
    if (!S.have) {
        lv_obj_remove_flag(U.res_note, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(U.res_desc, "");
        lv_label_set_text(U.res_tx, "");
        lv_label_set_text(U.res_rx, "");
        lv_label_set_text(U.res_ascii, "");
        lv_label_set_text(U.res_note, _("Tocá Enviar o un ejemplo."));
        lv_obj_set_style_text_color(U.res_note, AOS_C_DIM, 0);
        return;
    }
    char line[SPI_MAX * 3 + 8];
    lv_label_set_text(U.res_desc, S.res_desc);
    bool sent = S.res_err == E_NONE || S.res_err == E_XFER;
    hex_line(line, sizeof line, S.res_tx, S.res_n);
    lv_label_set_text(U.res_tx, line);
    hex_line(line, sizeof line, S.res_rx, sent ? S.res_n : 0);
    lv_label_set_text(U.res_rx, line);
    ascii_line(line, sizeof line, S.res_rx, sent ? S.res_n : 0);
    lv_label_set_text(U.res_ascii, line);
    char t[160];
    lv_color_t c;
    spi_decode(t, sizeof t, &c);
    lv_label_set_text(U.res_note, t);
    lv_obj_set_style_text_color(U.res_note, c, 0);
    if (t[0]) lv_obj_remove_flag(U.res_note, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.res_note, LV_OBJ_FLAG_HIDDEN);
}

static void tx_refresh(void)
{
    if (!U.tx_hex) return;
    char line[SPI_MAX * 3 + 8];
    hex_line(line, sizeof line, S.tx, S.txn);
    lv_label_set_text(U.tx_hex, S.txn ? line : "··");
    if (S.txn == 1) lv_label_set_text(U.tx_count, _("1 byte · tocá para editar"));
    else lv_label_set_text_fmt(U.tx_count, _("%d bytes · tocá para editar"), S.txn);
}

static void seg_style(lv_obj_t *s, bool on)
{
    lv_obj_set_style_bg_opa(s, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s, 0), on ? AOS_C_TEXT : AOS_C_DIM, 0);
}

static void settings_refresh(void)
{
    if (!U.clk_val) return;
    char hz[16];
    fmt_clock(hz, sizeof hz, CLOCKS[S.spi_clk]);
    lv_label_set_text(U.clk_val, hz);
    for (int i = 0; i < 4; i++) seg_style(U.mode_seg[i], i == S.spi_mode);
    for (int i = 0; i < 2; i++) seg_style(U.bit_seg[i], i == (int)S.spi_lsb);
    cs_opt_t o[AOS_IO_MODULE_MAX + 2];
    int n = cs_options(o, AOS_IO_MODULE_MAX + 2), c = cs_current(o, n);
    if (!n) lv_label_set_text(U.cs_val, "");
    else if (o[c].gpio < 0) lv_label_set_text(U.cs_val, _("sin CS"));
    else if (!o[c].mod[0]) lv_label_set_text_fmt(U.cs_val, _("GPIO%d · puerto"), o[c].gpio);
    else lv_label_set_text_fmt(U.cs_val, "GPIO%d · %s", o[c].gpio, o[c].mod);
    if (U.adc_chip) lv_label_set_text_fmt(U.adc_chip, "CH%d", S.adc_ch);
}

static void clk_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    S.spi_clk += d;
    if (S.spi_clk < 0) S.spi_clk = 0;
    if (S.spi_clk >= N_CLOCKS) S.spi_clk = N_CLOCKS - 1;
    settings_refresh();
}

static void mode_seg_cb(lv_event_t *e) { S.spi_mode = (int)(intptr_t)lv_event_get_user_data(e); settings_refresh(); }
static void bit_seg_cb(lv_event_t *e) { S.spi_lsb = (int)(intptr_t)lv_event_get_user_data(e) != 0; settings_refresh(); }

static void cs_cb(lv_event_t *e)
{
    cs_opt_t o[AOS_IO_MODULE_MAX + 2];
    int n = cs_options(o, AOS_IO_MODULE_MAX + 2);
    if (!n) return;
    int c = (cs_current(o, n) + 1) % n;
    snprintf(S.spi_cs, sizeof S.spi_cs, "%s", o[c].mod);
    settings_refresh();
}

static void spi_port_cb(lv_event_t *e)
{
    const aos_io_port_t *ports[AOS_IO_PORT_MAX];
    int n = spi_ports(ports, AOS_IO_PORT_MAX);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= n || J.busy) return;
    snprintf(S.spi_port, sizeof S.spi_port, "%s", ports[i]->name);
    S.spi_cs[0] = 0;
    build_page();
}

static void send_cb(lv_event_t *e) { spi_send(P_NONE); }

/* The presets: the bytes, the mode and a clock every chip of the kind
 * takes, then a send. The CS stays as chosen: it is where the chip is. */
static void preset(int which)
{
    static const uint8_t LOOP[16] = { 0x55, 0xAA, 0x00, 0xFF, 0x01, 0x02, 0x04, 0x08,
                                      0x10, 0x20, 0x40, 0x80, 'P', '4', 'O', 'S' };
    switch (which) {
    case P_FLASH: S.tx[0] = 0x9F; S.tx[1] = S.tx[2] = S.tx[3] = 0; S.txn = 4; break;
    case P_TC:    memset(S.tx, 0, 4); S.txn = 4; break;
    case P_ADC:   S.tx[0] = 0x01; S.tx[1] = (uint8_t)(0x80 | S.adc_ch << 4); S.tx[2] = 0; S.txn = 3; break;
    case P_LOOP:  memcpy(S.tx, LOOP, sizeof LOOP); S.txn = sizeof LOOP; break;
    default: return;
    }
    if (which != P_LOOP) { S.spi_mode = 0; S.spi_lsb = false; S.spi_clk = CLOCK_1M; }
    settings_refresh();
    tx_refresh();
    spi_send(which);
}

static void preset_cb(lv_event_t *e) { preset((int)(intptr_t)lv_event_get_user_data(e)); }

static void adc_ch_cb(lv_event_t *e)
{
    S.adc_ch = (S.adc_ch + 1) % 8;
    settings_refresh();
    lv_event_stop_bubbling(e);
}

/* The hex keypad for the bytes to send: two keys a byte, the half byte
 * shown with an underscore until its second key comes. */
static void hk_show(void)
{
    if (!U.wr_value) return;
    char line[SPI_MAX * 3 + 8];
    hex_line(line, sizeof line, U.kp_bytes, U.kp_n);
    if (U.kp_nib >= 0) {
        size_t l = strlen(line);
        snprintf(line + l, sizeof line - l, "%s%X_", U.kp_n ? " " : "", U.kp_nib);
    } else if (!U.kp_n) {
        snprintf(line, sizeof line, "_");
    }
    lv_label_set_text(U.wr_value, line);
}

static void hk_key_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 16) {
        if (U.kp_nib < 0) { if (U.kp_n < SPI_MAX) U.kp_nib = k; }
        else { U.kp_bytes[U.kp_n++] = (uint8_t)(U.kp_nib << 4 | k); U.kp_nib = -1; }
    } else if (k == 16) {
        if (U.kp_nib >= 0) U.kp_nib = -1;
        else if (U.kp_n) { U.kp_n--; U.kp_nib = U.kp_bytes[U.kp_n] >> 4; }
    } else if (k == 17) {
        U.kp_n = 0;
        U.kp_nib = -1;
    } else {
        if (U.kp_nib >= 0 && U.kp_n < SPI_MAX) U.kp_bytes[U.kp_n++] = (uint8_t)(U.kp_nib << 4);   /* "9" alone is 0x90 */
        memcpy(S.tx, U.kp_bytes, (size_t)U.kp_n);
        S.txn = U.kp_n;
        kp_close();
        tx_refresh();
        return;
    }
    hk_show();
}

static void tx_edit_cb(lv_event_t *e)
{
    memcpy(U.kp_bytes, S.tx, (size_t)S.txn);
    U.kp_n = S.txn;
    U.kp_nib = -1;
    kp_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    const int32_t kw = U.land ? 132 : 150, kh = U.land ? 72 : 104, gap = 12;
    lv_obj_t *sh = card(U.overlay, 4 * kw + 5 * gap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sh, gap, 0);
    lv_obj_set_style_pad_top(sh, 20, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(sh, gap, 0);
    lv_obj_center(sh);
    lv_obj_t *t = aos_label(sh, "", aos_font_small, AOS_C_DIM);
    lv_label_set_text_fmt(t, _("Bytes a enviar, hasta %d"), SPI_MAX);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    U.wr_value = lv_label_create(sh);
    lv_obj_set_style_text_font(U.wr_value, &aos_mono_22, 0);
    lv_obj_set_style_text_color(U.wr_value, C_AMBER, 0);
    lv_obj_set_width(U.wr_value, lv_pct(100));
    lv_label_set_long_mode(U.wr_value, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(U.wr_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_min_height(U.wr_value, U.land ? 60 : 92, 0);
    hk_show();
    static const char *const EXTRA[2] = { N_("Borrar todo"), N_("Listo") };
    for (int k = 0; k < 19; k++) {
        lv_obj_t *b = box(sh, k >= 17 ? 2 * kw + gap : kw, kh);
        if (k == 16) lv_obj_set_width(b, 4 * kw + 3 * gap);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, k == 18 ? C_AMBER : k >= 16 ? AOS_C_CARD2 : lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, hk_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_obj_t *l;
        if (k < 16) {
            l = lv_label_create(b);
            lv_obj_set_style_text_font(l, aos_font_title, 0);
            lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
            lv_label_set_text_fmt(l, "%X", k);
        } else if (k == 16) {
            l = aos_label(b, LV_SYMBOL_BACKSPACE, aos_font_title, AOS_C_TEXT);
        } else {
            l = aos_label(b, aos_tr(EXTRA[k - 17]), aos_font_body, AOS_C_TEXT);
        }
        lv_obj_center(l);
    }
}

/* A small setting: its name on top, its control below. */
static lv_obj_t *setting(lv_obj_t *parent, int32_t w, const char *name)
{
    lv_obj_t *c = card(parent, w, 104);
    lv_obj_set_style_pad_hor(c, 16, 0);
    lv_obj_t *l = aos_label(c, name, aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(l, 2, 10);
    return c;
}

static lv_obj_t *seg_row(lv_obj_t *parent, int n, const char *const *names, lv_obj_t **out, lv_event_cb_t cb)
{
    lv_obj_t *seg = box(parent, lv_pct(100), 52);
    lv_obj_align(seg, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(seg, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_50, 0);
    lv_obj_set_style_radius(seg, 26, 0);
    lv_obj_set_style_pad_all(seg, 4, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < n; i++) {
        lv_obj_t *s = box(seg, LV_SIZE_CONTENT, lv_pct(100));
        lv_obj_set_flex_grow(s, 1);
        lv_obj_set_style_radius(s, 22, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(0x48484A), 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(aos_label(s, names[i], aos_font_small, AOS_C_DIM));
        out[i] = s;
    }
    return seg;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_center(aos_label(b, glyph, &aos_sym_28, AOS_C_TEXT));
    return b;
}

static void build_spi_top(lv_obj_t *parent, int32_t w)
{
    const aos_io_port_t *ports[AOS_IO_PORT_MAX];
    int n = spi_ports(ports, AOS_IO_PORT_MAX);
    lv_obj_t *row = box(parent, w, 80);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *seg = box(row, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_bg_color(seg, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(seg, 36, 0);
    lv_obj_set_style_pad_all(seg, 6, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(seg, 1);
    for (int i = 0; i < n; i++) {
        bool on = !strcmp(ports[i]->name, S.spi_port);
        lv_obj_t *s = box(seg, LV_SIZE_CONTENT, 60);
        lv_obj_set_flex_grow(s, 1);
        lv_obj_set_style_radius(s, 30, 0);
        lv_obj_set_style_bg_color(s, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(s, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s, spi_port_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(aos_label(s, ports[i]->name, aos_font_small, on ? AOS_C_TEXT : AOS_C_DIM));
    }
    pill(row, AOS_SYM_SEND, _("Enviar"), C_AMBER, send_cb, NULL);
    const aos_io_port_t *pt = aos_io_port_find(S.spi_port);
    lv_obj_t *st = caption(parent, "", w);
    lv_obj_set_style_pad_left(st, 12, 0);
    if (pt) {
        char miso[16] = "", cs[16] = "";
        if (pt->pins[2] >= 0) snprintf(miso, sizeof miso, " · MISO %d", pt->pins[2]);
        if (pt->pins[3] >= 0) snprintf(cs, sizeof cs, " · CS %d", pt->pins[3]);
        lv_label_set_text_fmt(st, "SCK %d · MOSI %d%s%s", pt->pins[0], pt->pins[1], miso, cs);
    }
}

static void build_spi_settings(lv_obj_t *parent, int32_t w)
{
    const int32_t gap = 12, cw = (w - gap) / 2;
    lv_obj_t *g = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, gap, 0);

    lv_obj_t *c = setting(g, cw, _("Reloj"));
    lv_obj_t *mi = round_btn(c, AOS_SYM_MINUS, clk_cb, (void *)(intptr_t)-1);
    lv_obj_align(mi, LV_ALIGN_BOTTOM_LEFT, 0, -10);
    lv_obj_t *pl = round_btn(c, AOS_SYM_PLUS, clk_cb, (void *)(intptr_t)1);
    lv_obj_align(pl, LV_ALIGN_BOTTOM_RIGHT, 0, -10);
    U.clk_val = aos_label(c, "", aos_font_body, AOS_C_TEXT);
    lv_obj_align(U.clk_val, LV_ALIGN_BOTTOM_MID, 0, -20);

    static const char *const MODES[4] = { "0", "1", "2", "3" };
    c = setting(g, cw, _("Modo (CPOL, CPHA)"));
    seg_row(c, 4, MODES, U.mode_seg, mode_seg_cb);

    c = setting(g, cw, _("Chip select"));
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, cs_cb, LV_EVENT_CLICKED, NULL);
    U.cs_val = aos_label(c, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(U.cs_val, cw - 32 - 40);
    lv_label_set_long_mode(U.cs_val, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.cs_val, LV_ALIGN_BOTTOM_LEFT, 2, -20);
    lv_obj_t *ch = aos_label(c, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
    lv_obj_align(ch, LV_ALIGN_BOTTOM_RIGHT, 0, -20);

    static const char *const BITS[2] = { "MSB", "LSB" };
    c = setting(g, cw, _("Primero va el bit"));
    seg_row(c, 2, BITS, U.bit_seg, bit_seg_cb);
    settings_refresh();
}

static void build_spi_tx(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *t = aos_label(parent, _("BYTES A ENVIAR"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 12, 0);
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, tx_edit_cb, LV_EVENT_CLICKED, NULL);
    U.tx_hex = lv_label_create(c);
    lv_obj_set_style_text_font(U.tx_hex, &aos_mono_22, 0);
    lv_obj_set_style_text_color(U.tx_hex, C_AMBER, 0);
    lv_obj_set_width(U.tx_hex, w - 40);
    lv_label_set_long_mode(U.tx_hex, LV_LABEL_LONG_MODE_WRAP);
    U.tx_count = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    tx_refresh();
}

static lv_obj_t *mono_line(lv_obj_t *parent, int32_t w, const char *tag, lv_color_t col)
{
    lv_obj_t *r = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_t *k = aos_label(r, tag, aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(k, 0, 0);
    lv_obj_t *v = lv_label_create(r);
    lv_obj_set_style_text_font(v, &aos_mono_22, 0);
    lv_obj_set_style_text_color(v, col, 0);
    lv_obj_set_width(v, w - 84);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(v, 84, 0);
    return v;
}

static void build_spi_result(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *t = aos_label(parent, _("RESULTADO"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 12, 0);
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    U.res_note = aos_label(c, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_width(U.res_note, w - 40);
    lv_label_set_long_mode(U.res_note, LV_LABEL_LONG_MODE_WRAP);
    U.res_tx = mono_line(c, w - 40, "TX", AOS_C_TEXT);
    U.res_rx = mono_line(c, w - 40, "RX", C_AMBER);
    U.res_ascii = mono_line(c, w - 40, "ASCII", AOS_C_DIM);
    U.res_desc = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    res_refresh();
}

static void build_spi_presets(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *t = aos_label(parent, _("EJEMPLOS"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 12, 0);
    lv_obj_t *g = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    static const char *const NAME[P_COUNT] = {
        N_("Memoria flash: leer ID (9F)"), N_("MAX31855: temperatura"),
        N_("MCP3008: canal"), N_("Lazo: puentear los pines 34 y 36"),
    };
    static const char *const GLYPH[P_COUNT] = { AOS_SYM_MEMORY, AOS_SYM_THERMOMETER, AOS_SYM_GAUGE, AOS_SYM_SWAP_HORIZONTAL };
    const int32_t rh = U.land ? 80 : AOS_UI_ROW_H;
    for (int i = 0; i < P_COUNT; i++) {
        lv_obj_t *r = box(g, lv_pct(100), rh);
        lv_obj_set_style_pad_hor(r, 22, 0);
        if (i) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        }
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *gl = aos_label(r, GLYPH[i], &aos_sym_28, C_AMBER);
        lv_obj_align(gl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *l = aos_label(r, aos_tr(NAME[i]), aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(l, w - 44 - 48 - (i == P_ADC ? 120 : 40));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 48, 0);
        if (i == P_ADC) {
            /* the channel, a chip of its own that changes it without sending */
            lv_obj_t *chp = box(r, 96, 52);
            lv_obj_set_style_radius(chp, 26, 0);
            lv_obj_set_style_bg_color(chp, AOS_C_CARD2, 0);
            lv_obj_set_style_bg_opa(chp, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_opa(chp, LV_OPA_60, LV_STATE_PRESSED);
            lv_obj_set_ext_click_area(chp, 12);
            lv_obj_add_flag(chp, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(chp, adc_ch_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_align(chp, LV_ALIGN_RIGHT_MID, -36, 0);
            U.adc_chip = aos_label(chp, "", aos_font_small, AOS_C_TEXT);
            lv_obj_center(U.adc_chip);
        }
        lv_obj_t *c = aos_label(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    settings_refresh();
}

static void build_spi(lv_obj_t *parent)
{
    const aos_io_port_t *ports[AOS_IO_PORT_MAX];
    int n = spi_ports(ports, AOS_IO_PORT_MAX);
    if (n && !aos_io_port_find(S.spi_port)) snprintf(S.spi_port, sizeof S.spi_port, "%s", ports[0]->name);
    int32_t w = U.W - 2 * AOS_UI_PAD, ch = lv_obj_get_height(parent);
    if (!n) {
        lv_obj_t *col = column(parent, w, ch);
        lv_obj_t *t = aos_label(col, _("No hay puertos SPI"), aos_font_large, AOS_C_TEXT);
        (void)t;
        caption(col, _("Agregá uno a modules.txt (Módulos > Puertos), por ejemplo:"), w);
        lv_obj_t *ex = aos_label(col, "port spi.a sck=52 mosi=50 miso=51 cs=49", aos_font_small, C_AMBER);
        (void)ex;
        return;
    }
    if (!U.land) {
        lv_obj_t *col = column(parent, w, ch);
        build_spi_top(col, w);
        build_spi_settings(col, w);
        build_spi_tx(col, w);
        build_spi_result(col, w);
        build_spi_presets(col, w);
    } else {
        int32_t lw = 620, rw = w - lw - AOS_UI_PAD;
        lv_obj_t *l = column(parent, lw, ch);
        build_spi_top(l, lw);
        build_spi_settings(l, lw);
        build_spi_tx(l, lw);
        lv_obj_t *r = column(parent, rw, ch);
        lv_obj_set_x(r, lw + AOS_UI_PAD);
        build_spi_result(r, rw);
        build_spi_presets(r, rw);
    }
}

/* -------------------------------------------------------------------------- */
/* GPIO                                                                        */
/* -------------------------------------------------------------------------- */

static bool pin_usable(const aos_io_pin_t *p)
{
    if (p->gpio < 0 || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) return false;
    const char *o = aos_io_owner(p->gpio);
    return !o || !strcmp(o, OWNER);
}

static void pin_refresh(int gpio)
{
    lv_obj_t *m = U.pin_mode[gpio], *led = U.pin_led[gpio];
    if (!m) return;
    int mode = S.mode[gpio];
    lv_label_set_text(lv_obj_get_child(m, 0), mode ? MODE_NAME[mode] : aos_tr(MODE_NAME[0]));
    lv_obj_set_style_bg_color(m, mode == M_OUT ? C_AMBER : mode ? C_BOARD : AOS_C_CARD2, 0);
    int lvl = mode == M_NONE ? -1 : mode == M_OUT ? S.level[gpio] : aos_io_gpio_get(gpio);
    lv_obj_set_style_bg_color(led, lvl == 1 ? lv_color_hex(0x30D158) : lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_color(led, lvl < 0 ? lv_color_hex(0x2C2C2E) : lv_color_hex(0x48484A), 0);
    lv_label_set_text(lv_obj_get_child(led, 0), lvl < 0 ? "" : lvl ? "1" : "0");
}

static void mode_cb(lv_event_t *e)
{
    int gpio = (int)(intptr_t)lv_event_get_user_data(e);
    int m = (S.mode[gpio] + 1) % M_COUNT;
    static const aos_gpio_mode_t HW[M_COUNT] = { 0, AOS_GPIO_INPUT, AOS_GPIO_INPUT_PULLUP, AOS_GPIO_INPUT_PULLDOWN, AOS_GPIO_OUTPUT };
    if (m == M_NONE) {
        aos_io_gpio_mode(gpio, AOS_GPIO_INPUT, OWNER);      /* a released pin is left floating, not driven */
        aos_io_release(gpio, OWNER);
    } else if (!aos_io_gpio_mode(gpio, HW[m], OWNER)) {
        const char *o = aos_io_owner(gpio);
        char t[64];
        snprintf(t, sizeof t, _("GPIO%d lo tiene %s"), gpio, o ? o : "?");
        aos_ui_toast(t, 2000);
        return;
    }
    if (m == M_OUT) { S.level[gpio] = 0; aos_io_gpio_set(gpio, 0); }
    S.mode[gpio] = (uint8_t)m;
    pin_refresh(gpio);
}

static void led_cb(lv_event_t *e)
{
    int gpio = (int)(intptr_t)lv_event_get_user_data(e);
    if (S.mode[gpio] != M_OUT) return;
    S.level[gpio] ^= 1;
    aos_io_gpio_set(gpio, S.level[gpio]);
    pin_refresh(gpio);
}

static void release_cb(lv_event_t *e)
{
    for (int g = 0; g < 64; g++)
        if (S.mode[g]) aos_io_gpio_mode(g, AOS_GPIO_INPUT, OWNER);
    aos_io_release_owner(OWNER);
    memset(S.mode, 0, sizeof S.mode);
    build_page();
}

static void build_gpio(lv_obj_t *parent)
{
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = column(parent, w, lv_obj_get_height(parent));
    lv_obj_t *top = box(col, w, 80);
    lv_obj_t *t = aos_label(top, _("Header"), aos_font_large, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *rel = pill(top, NULL, _("Soltar"), AOS_C_CARD2, release_cb, NULL);
    lv_obj_align(rel, LV_ALIGN_RIGHT_MID, 0, 0);
    caption(col, _("Tocá el modo para cambiarlo. En OUT, tocá el nivel para invertirlo. Los pines quedan así al cerrar la app, hasta que los sueltes."), w);

    const aos_io_pin_t *hdr = aos_io_header();
    int cols = U.land ? 4 : 2;
    const int32_t gap = 8, cw = (w - (cols - 1) * gap) / cols, rh = 64;
    lv_obj_t *grid = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, gap, 0);
    /* physical order: as on the board, the even pin on the left of each
     * pair and the odd one (5 V's column) on the right; in landscape the
     * header folds in two, pins 1-20 then 21-40 */
    int rows = 40 / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = U.land ? (c / 2) * 20 + r * 2 + (1 - c % 2) : r * 2 + (1 - c);
            const aos_io_pin_t *p = &hdr[idx];
            bool use = pin_usable(p);
            lv_obj_t *cell = card(grid, cw, rh);
            lv_obj_set_style_pad_hor(cell, 12, 0);
            if (!use) lv_obj_set_style_bg_opa(cell, LV_OPA_50, 0);
            lv_obj_t *n = aos_label(cell, "", aos_font_caption, AOS_C_DIM);
            lv_label_set_text_fmt(n, "%d", p->pin);
            lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
            uint32_t pc = p->label[0] == '5' ? 0xFF6B60 : p->label[0] == '3' ? 0xFFA14A : p->label[0] == 'G' && p->gpio < 0 ? 0x8E8E93 : 0xFFFFFF;
            lv_obj_t *l = aos_label(cell, p->label, aos_font_small, use ? lv_color_hex(pc) : p->gpio < 0 ? lv_color_hex(pc) : AOS_C_DIM);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 40, 0);
            if (p->gpio < 0) continue;
            if (!use) {
                const char *o = aos_io_owner(p->gpio);
                lv_obj_t *d = aos_label(cell, o ? o : (p->flags & AOS_PIN_RESERVED) ? _("reservado") : _("de la placa"),
                                        aos_font_tiny, AOS_C_DIM);
                lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
                continue;
            }
            lv_obj_t *led = box(cell, 40, 40);
            lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(led, 2, 0);
            lv_obj_set_ext_click_area(led, 12);
            lv_obj_align(led, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_t *lt = lv_label_create(led);
            lv_obj_set_style_text_font(lt, aos_font_tiny, 0);
            lv_obj_set_style_text_color(lt, lv_color_white(), 0);
            lv_obj_center(lt);
            lv_obj_add_flag(led, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(led, led_cb, LV_EVENT_CLICKED, (void *)(intptr_t)p->gpio);
            lv_obj_t *m = box(cell, 92, 44);
            lv_obj_set_style_radius(m, 22, 0);
            lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
            lv_obj_set_ext_click_area(m, 10);
            lv_obj_align(m, LV_ALIGN_RIGHT_MID, -52, 0);
            lv_obj_t *mt = aos_label(m, "", aos_font_tiny, lv_color_white());
            lv_obj_center(mt);
            lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(m, mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)p->gpio);
            U.pin_mode[p->gpio] = m;
            U.pin_led[p->gpio] = led;
            pin_refresh(p->gpio);
        }
    }
    lv_obj_t *leg = caption(col, _("IN ↑ / IN ↓: con pull-up o pull-down interno. Los pines 35 a 39 van por el LDO VO4: medí antes de colgarles algo."), w);
    (void)leg;
}

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    kp_close();
    lv_obj_clean(U.content);
    U.status = U.scan_btn = NULL;
    memset(U.addr, 0, sizeof U.addr);
    memset(U.reg, 0, sizeof U.reg);
    memset(U.pin_mode, 0, sizeof U.pin_mode);
    memset(U.pin_led, 0, sizeof U.pin_led);
    U.clk_val = U.cs_val = U.tx_hex = U.tx_count = U.adc_chip = NULL;
    U.res_desc = U.res_tx = U.res_rx = U.res_ascii = U.res_note = NULL;
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == S.tab ? C_AMBER : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 1), c, 0);
    }
    int32_t ch = lv_obj_get_height(U.content);
    if (S.tab == TAB_GPIO) { build_gpio(U.content); return; }
    if (S.tab == TAB_SPI) { build_spi(U.content); return; }

    int32_t w = U.W - 2 * AOS_UI_PAD;
    if (S.dev < 0) {
        if (!U.land) {
            lv_obj_t *col = column(U.content, w, ch);
            build_i2c_top(col, w);
            build_grid(col, w);
            build_found(col, w);
        } else {
            int32_t lw = 680, rw = w - lw - AOS_UI_PAD;
            lv_obj_t *l = column(U.content, lw, ch);
            build_i2c_top(l, lw);
            build_grid(l, lw);
            lv_obj_t *r = column(U.content, rw, ch);
            lv_obj_set_x(r, lw + AOS_UI_PAD);
            lv_obj_set_style_pad_top(r, 96, 0);
            build_found(r, rw);
        }
        return;
    }
    if (!U.land) {
        lv_obj_t *col = column(U.content, w, ch);
        build_dev_head(col, w);
        build_regs(col, w);
    } else {
        int32_t lw = 700, rw = w - lw - AOS_UI_PAD;
        lv_obj_t *l = column(U.content, lw, ch);
        build_regs(l, lw);
        lv_obj_t *r = column(U.content, rw, ch);
        lv_obj_set_x(r, lw + AOS_UI_PAD);
        build_dev_head(r, rw);
    }
    regs_refresh();
}

static void tab_cb(lv_event_t *e)
{
    S.tab = (int)(intptr_t)lv_event_get_user_data(e);
    build_page();
}

static void timer_cb(lv_timer_t *t)
{
    if (J.seq != U.seen_seq && !J.busy) {
        U.seen_seq = J.seq;
        if (J.job == JOB_SPI) {
            S.have = true;
            S.res_preset = J.preset;
            S.res_ch = J.ch;
            S.res_err = J.err;
            S.res_err_gpio = J.err_gpio;
            snprintf(S.res_err_who, sizeof S.res_err_who, "%s", J.err_who);
            snprintf(S.res_desc, sizeof S.res_desc, "%s", J.desc);
            S.res_n = J.n;
            memcpy(S.res_tx, J.tx, sizeof S.res_tx);
            memcpy(S.res_rx, J.rx, sizeof S.res_rx);
            res_refresh();
        } else if (J.job == JOB_SCAN) {
            memcpy(S.found, J.found, sizeof S.found);
            S.scanned = J.ok;
            if (!J.ok) aos_ui_toast(_("No se pudo abrir el puerto"), 2000);
            if (S.tab == TAB_I2C && S.dev < 0) build_page();
        } else {
            if (S.regs_ok && J.ok) { memcpy(S.prev, S.regs, 256); S.have_prev = true; }
            S.regs_ok = J.ok;
            if (J.ok) memcpy(S.regs, J.regs, 256);
            if (S.tab == TAB_I2C && S.dev >= 0) regs_refresh();
        }
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (S.tab == TAB_I2C && S.dev >= 0 && S.live && !U.overlay && now - U.live_ms >= 500) {
        U.live_ms = now;
        job_start(JOB_DUMP, (uint8_t)S.dev, 0, 0);
    }
    if (S.tab == TAB_GPIO)
        for (int g = 0; g < 64; g++)
            if (U.pin_mode[g] && S.mode[g] != M_OUT && S.mode[g] != M_NONE) pin_refresh(g);
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.seen_seq = J.seq;
    if (!S.port[0]) snprintf(S.port, sizeof S.port, "i2c.ext");
    const int32_t tab_h = U.land ? 96 : 116;

    U.content = box(root, U.W, U.H - tab_h);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.content, 8, 0);
    lv_obj_update_layout(U.content);

    lv_obj_t *bar = box(root, U.W, tab_h);
    lv_obj_set_pos(bar, 0, U.H - tab_h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = box(bar, U.W / TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *g = aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, U.land ? -14 : -16);
        lv_obj_t *n = aos_label(t, TAB_NAME[i], aos_font_tiny, AOS_C_DIM);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, U.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }
    build_page();
    /* the first time in, look at the header's bus straight away */
    if (S.tab == TAB_I2C && S.dev < 0 && !S.scanned) { job_start(JOB_SCAN, 0, 0, 0); status_refresh(); }
    U.timer = lv_timer_create(timer_cb, 120, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    S.live = false;
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { kp_close(); return true; }
    if (S.tab == TAB_I2C && S.dev >= 0) { S.dev = -1; S.live = false; build_page(); return true; }
    return false;
}

void aos_app_bus_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.bus", .name = "Bus", .icon = AOS_SYM_CONNECTION,
            .color_a = 0xF59E0B, .color_b = 0xB45309,
            .order = 510,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
