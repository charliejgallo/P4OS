/*
 * P4OS - EEPROM: the Chip page.
 *
 * Which part (the family, then the chip from the table), where it hangs,
 * detecting it, and its write protection. Every change is kept in the
 * preferences at once and told to the portal's page (chip.json).
 */
#include "ee.h"

static const uint32_t CLOCKS[] = { 100000, 250000, 500000, 1000000, 2000000, 4000000, 5000000,
                                   8000000, 10000000, 20000000 };
#define N_CLOCKS (int)(sizeof CLOCKS / sizeof CLOCKS[0])

static const char *const DEF_CHIP[EE_FAM_COUNT] = { "24LC256", "25LC640", "25Q128", "93C66" };

static void changed(void)
{
    ee_conf_save(&S.conf);
    S.vers_stale = true;
    ee_portal_chip();
    ee_rebuild();
}

static void hz_text(uint32_t hz, char *out, size_t len)
{
    if (hz >= 1000000 && !(hz % 1000000)) snprintf(out, len, "%u MHz", (unsigned)(hz / 1000000));
    else snprintf(out, len, "%u kHz", (unsigned)(hz / 1000));
}

/* -------------------------------------------------------------------------- */
/* Family and chip                                                             */
/* -------------------------------------------------------------------------- */

static void fam_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (J.busy || f == ee_cur()->fam) return;
    S.conf.chip = ee_chip_find(DEF_CHIP[f]);
    S.det[0] = 0;
    S.sugg_chip = -1;
    S.have_sr = false;
    changed();
}

static int s_pick_idx[48];

static void chip_pick(int i, void *ud)
{
    (void)ud;
    if (J.busy) return;
    S.conf.chip = s_pick_idx[i];
    const ee_chip_t *c = ee_cur();
    if (c->fam == EE_I2C) {
        /* the address may not be one this chip can take */
        uint8_t b[8];
        int n = ee_i2c_bases(c, b, 8);
        bool ok = false;
        for (int k = 0; k < n; k++) ok |= b[k] == S.conf.i2c_addr;
        if (!ok) S.conf.i2c_addr = b[0];
    }
    changed();
}

static void chip_cb(lv_event_t *e)
{
    (void)e;
    static ee_item_t it[48];
    static char txt[48][40];
    int fam = ee_cur()->fam, n = 0;
    for (int i = 0; i < ee_chip_count() && n < 48; i++) {
        const ee_chip_t *c = ee_chip_at(i);
        if (c->fam != fam) continue;
        char st[16];
        ee_size_text(c->size, st, sizeof st);
        snprintf(txt[n], sizeof txt[n], "%s  ·  %s", c->name, st);
        it[n] = (ee_item_t){ txt[n], c->also, false, i == S.conf.chip };
        s_pick_idx[n++] = i;
    }
    ee_sheet(_("Chip"), it, n, chip_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* Ports, addresses, pins                                                      */
/* -------------------------------------------------------------------------- */

static char s_ports[AOS_IO_PORT_MAX][AOS_IO_PORT_NAME_MAX];

static void port_pick(int i, void *ud)
{
    char *dst = ud;
    snprintf(dst, AOS_IO_PORT_NAME_MAX, "%s", s_ports[i]);
    changed();
}

static void port_cb(lv_event_t *e)
{
    if (J.busy) return;
    bool spi = ee_cur()->fam == EE_SPI || ee_cur()->fam == EE_FLASH;
    char *dst = spi ? S.conf.spi_port : S.conf.i2c_port;
    static ee_item_t it[AOS_IO_PORT_MAX];
    static char sub[AOS_IO_PORT_MAX][48];
    int n = 0;
    for (int i = 0; i < aos_io_port_count() && n < AOS_IO_PORT_MAX; i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind != (spi ? AOS_PORT_SPI : AOS_PORT_I2C)) continue;
        snprintf(s_ports[n], sizeof s_ports[n], "%s", p->name);
        if (spi) snprintf(sub[n], sizeof sub[n], "SCK %d · MOSI %d · MISO %d · CS %d", p->pins[0], p->pins[1], p->pins[2], p->pins[3]);
        else snprintf(sub[n], sizeof sub[n], "SDA %d · SCL %d · %u kHz", p->pins[0], p->pins[1], (unsigned)(p->freq / 1000));
        it[n] = (ee_item_t){ s_ports[n], sub[n], false, !strcmp(p->name, dst) };
        n++;
    }
    (void)e;
    if (!n) { aos_ui_toast(_("modules.txt no tiene puertos de esa clase"), 2000); return; }
    ee_sheet(_("Puerto"), it, n, port_pick, dst);
}

static uint8_t s_bases[8];

static void addr_pick(int i, void *ud)
{
    (void)ud;
    S.conf.i2c_addr = s_bases[i];
    changed();
}

static void apins_text(const ee_chip_t *c, uint8_t base, char *out, size_t len)
{
    if (!c->apins) { snprintf(out, len, "%s", _("sin pines de dirección")); return; }
    size_t k = 0;
    out[0] = 0;
    for (int b = 2; b >= 0; b--) {
        if (!(c->apins & (1 << b))) continue;
        k += (size_t)snprintf(out + k, len - k, "%sA%d=%d", k ? " " : "", b, (base >> b) & 1);
        if (k >= len) break;
    }
}

static void addr_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) return;
    const ee_chip_t *c = ee_cur();
    int n = ee_i2c_bases(c, s_bases, 8);
    static ee_item_t it[8];
    static char txt[8][12], sub[8][32];
    for (int i = 0; i < n; i++) {
        snprintf(txt[i], sizeof txt[i], "0x%02X", s_bases[i]);
        apins_text(c, s_bases[i], sub[i], sizeof sub[i]);
        it[i] = (ee_item_t){ txt[i], sub[i], false, s_bases[i] == S.conf.i2c_addr };
    }
    ee_sheet(_("Dirección"), it, n, addr_pick, NULL);
}

static int s_gpio[40];

static void cs_pick(int i, void *ud)
{
    (void)ud;
    S.conf.spi_cs = s_gpio[i];
    changed();
}

static void mw_pick(int i, void *ud)
{
    S.conf.mw[(intptr_t)ud] = s_gpio[i];
    changed();
}

/* A sheet of the header's GPIOs; with 'port_cs', first the port's own CS. */
static void gpio_sheet(const char *title, int current, bool port_cs, ee_pick_cb_t cb, void *ud)
{
    static ee_item_t it[41];
    static char txt[41][32], sub[41][64];
    int n = 0;
    if (port_cs) {
        const aos_io_port_t *p = aos_io_port_find(S.conf.spi_port);
        snprintf(txt[n], sizeof txt[n], "%s", _("El del puerto"));
        snprintf(sub[n], sizeof sub[n], "cs=%d %s", p ? p->pins[3] : -1, _("en modules.txt"));
        it[n] = (ee_item_t){ txt[n], sub[n], false, current < 0 };
        s_gpio[n++] = -1;
    }
    const aos_io_pin_t *h = aos_io_header();
    for (int i = 0; i < 40 && n < 41; i++) {
        if (h[i].gpio < 0 || (h[i].flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) continue;
        const char *o = aos_io_owner(h[i].gpio);
        snprintf(txt[n], sizeof txt[n], "GPIO%d", h[i].gpio);
        snprintf(sub[n], sizeof sub[n], "%s %d%s%s%s%s", _("pata"), h[i].pin, h[i].note[0] ? " · " : "", h[i].note,
                 o && strcmp(o, EE_OWNER) ? " · " : "", o && strcmp(o, EE_OWNER) ? o : "");
        it[n] = (ee_item_t){ txt[n], sub[n], false, h[i].gpio == current };
        s_gpio[n++] = h[i].gpio;
    }
    ee_sheet(title, it, n, cb, ud);
}

static void cs_cb(lv_event_t *e)
{
    (void)e;
    if (!J.busy) gpio_sheet(_("Chip select"), S.conf.spi_cs, true, cs_pick, NULL);
}

static void mw_cb(lv_event_t *e)
{
    static const char *const NAME[4] = { "CS", "SK", "DI", "DO" };
    intptr_t k = (intptr_t)lv_event_get_user_data(e);
    if (!J.busy) gpio_sheet(NAME[k], S.conf.mw[k], false, mw_pick, (void *)k);
}

static void clk_pick(int i, void *ud)
{
    (void)ud;
    S.conf.spi_hz = CLOCKS[i];
    changed();
}

static void clk_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) return;
    static ee_item_t it[N_CLOCKS];
    static char txt[N_CLOCKS][16];
    for (int i = 0; i < N_CLOCKS; i++) {
        hz_text(CLOCKS[i], txt[i], sizeof txt[i]);
        it[i] = (ee_item_t){ txt[i], NULL, false, CLOCKS[i] == S.conf.spi_hz };
    }
    ee_sheet(_("Reloj"), it, N_CLOCKS, clk_pick, NULL);
}

static void org_cb(lv_event_t *e)
{
    if (J.busy) return;
    S.conf.mw_org = (int)(intptr_t)lv_event_get_user_data(e) ? 16 : 8;
    changed();
}

/* -------------------------------------------------------------------------- */
/* Detecting                                                                   */
/* -------------------------------------------------------------------------- */

static void detect_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) { aos_ui_toast(_("Hay un trabajo en curso"), 1600); return; }
    if (ee_job_start(JOB_DETECT, 0)) { snprintf(S.det, sizeof S.det, "%s", _("Buscando…")); ee_rebuild(); }
}

static void size_go(void *ud)
{
    (void)ud;
    if (ee_job_start(JOB_SIZE, 0)) { snprintf(S.det, sizeof S.det, "%s", _("Probando…")); ee_rebuild(); }
}

static void size_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) { aos_ui_toast(_("Hay un trabajo en curso"), 1600); return; }
    ee_confirm(_("Prueba de tamaño"),
               _("Escribe dos marcas en el byte 0 y mira dónde da la vuelta la memoria; al final vuelve a poner el byte que estaba. Con WP a 3V3 no puede escribir."),
               _("Probar"), true, size_go, NULL);
}

static void use_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy || S.sugg_chip < 0) return;
    S.conf.chip = S.sugg_chip;
    if (S.sugg_addr >= 0x50) S.conf.i2c_addr = (uint8_t)S.sugg_addr;
    if (S.sugg_org) S.conf.mw_org = S.sugg_org;
    S.sugg_chip = -1;
    changed();
}

static bool sugg_differs(void)
{
    if (S.sugg_chip < 0) return false;
    if (S.sugg_chip != S.conf.chip) return true;
    if (ee_cur()->fam == EE_I2C && S.sugg_addr >= 0x50 && S.sugg_addr != S.conf.i2c_addr) return true;
    if (ee_cur()->fam == EE_MW && S.sugg_org && S.sugg_org != S.conf.mw_org) return true;
    return false;
}

/* -------------------------------------------------------------------------- */
/* Protection                                                                  */
/* -------------------------------------------------------------------------- */

static uint8_t s_new_sr;

static void status_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) { aos_ui_toast(_("Hay un trabajo en curso"), 1600); return; }
    ee_job_start(JOB_STATUS, 0);
}

static void protect_go(void *ud)
{
    (void)ud;
    if (J.busy) return;
    J.new_sr = s_new_sr;                /* before the thread starts: it reads it at once */
    ee_job_start(JOB_PROTECT, 0);
}

static const char *const BP_NAME[4] = { N_("Nada"), N_("¼ de arriba"), N_("½ de arriba"), N_("Toda") };

static void bp_pick(int i, void *ud)
{
    (void)ud;
    uint8_t wpen = S.have_sr ? (S.sr & 0x80) : 0;
    s_new_sr = (uint8_t)(wpen | (i << 2));
    char txt[160];
    snprintf(txt, sizeof txt, _("Proteger: %s. Lo protegido no se puede escribir hasta sacarlo."), _(BP_NAME[i]));
    ee_confirm(_("Protección"), txt, _("Cambiar"), true, protect_go, NULL);
}

static void bp_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) return;
    static ee_item_t it[4];
    int cur = S.have_sr ? (S.sr >> 2) & 3 : -1;
    for (int i = 0; i < 4; i++) it[i] = (ee_item_t){ _(BP_NAME[i]), NULL, false, i == cur };
    ee_sheet(_("Bloques protegidos (BP1 BP0)"), it, 4, bp_pick, NULL);
}

static void wpen_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) return;
    if (!S.have_sr) { aos_ui_toast(_("Leé el estado primero"), 1600); return; }
    s_new_sr = (uint8_t)(S.sr ^ 0x80) & 0x8C;
    ee_confirm(_("WPEN"),
               (s_new_sr & 0x80) ? _("Con WPEN puesto, la pata /WP en bajo traba el registro de estado.")
                                 : _("Sin WPEN, la pata /WP no traba el registro de estado."),
               _("Cambiar"), true, protect_go, NULL);
}

static void unprotect_cb(lv_event_t *e)
{
    (void)e;
    if (J.busy) return;
    s_new_sr = 0;
    ee_confirm(_("Quitar protección"), _("Pone en 0 el registro de estado de la flash: se puede escribir toda."),
               _("Quitar"), true, protect_go, NULL);
}

static void sr_text(char *out, size_t len)
{
    if (!S.have_sr) { snprintf(out, len, "%s", _("sin leer")); return; }
    if (ee_cur()->fam == EE_FLASH)
        snprintf(out, len, "0x%02X · BP %d%s", S.sr, (S.sr >> 2) & 7, (S.sr & 0x1C) ? _(" (protegida)") : "");
    else
        snprintf(out, len, "0x%02X · %s%s", S.sr, _(BP_NAME[(S.sr >> 2) & 3]), (S.sr & 0x80) ? " · WPEN" : "");
}

/* -------------------------------------------------------------------------- */
/* The cards                                                                   */
/* -------------------------------------------------------------------------- */

static void card_chip(lv_obj_t *col, int32_t w)
{
    const ee_chip_t *c = ee_cur();
    ee_title(col, _("MEMORIA"));
    lv_obj_t *k = ee_card(col, w);
    static const char *const FAM[EE_FAM_COUNT] = { "I2C", "SPI", "Flash", "93xx" };
    ee_seg(k, w - 40, FAM, EE_FAM_COUNT, c->fam, fam_cb);
    char st[16], val[40];
    ee_size_text(c->size, st, sizeof st);
    snprintf(val, sizeof val, "%s · %s", c->name, st);
    ee_row(k, w - 40, _("Chip"), val, chip_cb, NULL);
    char info[160];
    if (c->fam == EE_FLASH)
        snprintf(info, sizeof info, _("%s. Páginas de 256 B, se borra por sectores de 4 KB."), c->also);
    else if (c->fam == EE_MW)
        snprintf(info, sizeof info, _("%s. Palabras de 8 o 16 bits según ORG."), c->also);
    else
        snprintf(info, sizeof info, _("%s. Página de %u B, escritura de %u ms."), c->also, c->page, c->twc_ms);
    ee_caption(k, info, w - 40);
}

static void card_conn(lv_obj_t *col, int32_t w)
{
    const ee_chip_t *c = ee_cur();
    ee_title(col, _("CONEXIÓN"));
    lv_obj_t *k = ee_card(col, w);
    int32_t iw = w - 40;
    char v[48];
    if (c->fam == EE_I2C) {
        ee_row(k, iw, _("Puerto"), S.conf.i2c_port, port_cb, NULL);
        char ap[32];
        apins_text(c, S.conf.i2c_addr, ap, sizeof ap);
        snprintf(v, sizeof v, "0x%02X · %s", S.conf.i2c_addr, ap);
        ee_row(k, iw, _("Dirección"), v, addr_cb, NULL);
        const aos_io_port_t *p = aos_io_port_find(S.conf.i2c_port);
        if (p) {
            char t[96];
            snprintf(t, sizeof t, "SDA GPIO%d · SCL GPIO%d · %u kHz", p->pins[0], p->pins[1], (unsigned)(p->freq / 1000));
            ee_caption(k, t, iw);
        }
    } else if (c->fam == EE_SPI || c->fam == EE_FLASH) {
        ee_row(k, iw, _("Puerto"), S.conf.spi_port, port_cb, NULL);
        const aos_io_port_t *p = aos_io_port_find(S.conf.spi_port);
        if (S.conf.spi_cs < 0) snprintf(v, sizeof v, "%s (GPIO%d)", _("del puerto"), p ? p->pins[3] : -1);
        else snprintf(v, sizeof v, "GPIO%d", S.conf.spi_cs);
        ee_row(k, iw, _("Chip select"), v, cs_cb, NULL);
        hz_text(S.conf.spi_hz, v, sizeof v);
        ee_row(k, iw, _("Reloj"), v, clk_cb, NULL);
        if (p) {
            char t[96];
            snprintf(t, sizeof t, "SCK GPIO%d · MOSI GPIO%d · MISO GPIO%d · %s 0", p->pins[0], p->pins[1], p->pins[2], _("modo"));
            ee_caption(k, t, iw);
        }
    } else {
        static const char *const NAME[4] = { "CS", "SK", "DI", "DO" };
        for (int i = 0; i < 4; i++) {
            snprintf(v, sizeof v, "GPIO%d", S.conf.mw[i]);
            ee_row(k, iw, NAME[i], v, mw_cb, (void *)(intptr_t)i);
        }
        const char *org[2] = { _("x8 (ORG a GND)"), _("x16 (ORG a 3V3)") };
        ee_seg(k, iw, org, 2, S.conf.mw_org == 16, org_cb);
    }
}

static void card_detect(lv_obj_t *col, int32_t w)
{
    const ee_chip_t *c = ee_cur();
    ee_title(col, _("DETECTAR"));
    lv_obj_t *k = ee_card(col, w);
    lv_obj_t *row = ee_box(k, w - 40, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 12, 0);
    ee_set_disabled(ee_pill(row, AOS_SYM_MAGNIFY, _("Detectar"), AOS_C_ACCENT, detect_cb, NULL), J.busy);
    if (c->fam == EE_I2C || c->fam == EE_SPI)
        ee_set_disabled(ee_pill(row, AOS_SYM_RULER, _("Probar tamaño"), AOS_C_CARD2, size_cb, NULL), J.busy);
    const char *t = S.det[0] ? S.det
                  : c->fam == EE_I2C   ? _("Busca quién responde entre 0x50 y 0x57 y, leyendo, cuántos bytes de dirección toma y dónde da la vuelta.")
                  : c->fam == EE_FLASH ? _("Pide el JEDEC id: fabricante y tamaño.")
                  : c->fam == EE_SPI   ? _("Lee el estado, la firma del 25xx1024 y, leyendo, los bytes de dirección y dónde da la vuelta.")
                                       : _("Cuenta los bits de dirección hasta el 0 de DO: eso da el chip y el ORG.");
    lv_obj_t *l = aos_label(k, t, aos_font_small, S.det[0] ? AOS_C_TEXT : AOS_C_DIM);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, w - 40);
    if (sugg_differs()) {
        char txt[64];
        const ee_chip_t *s = ee_chip_at(S.sugg_chip);
        if (s->fam == EE_I2C && S.sugg_addr >= 0x50) snprintf(txt, sizeof txt, _("Usar %s en 0x%02X"), s->name, S.sugg_addr);
        else if (s->fam == EE_MW && S.sugg_org) snprintf(txt, sizeof txt, _("Usar %s x%d"), s->name, S.sugg_org);
        else snprintf(txt, sizeof txt, _("Usar %s"), s->name);
        ee_pill(k, AOS_SYM_CHECK, txt, lv_color_hex(0x15803D), use_cb, NULL);
    }
}

static void card_protect(lv_obj_t *col, int32_t w)
{
    const ee_chip_t *c = ee_cur();
    ee_title(col, _("PROTECCIÓN"));
    lv_obj_t *k = ee_card(col, w);
    int32_t iw = w - 40;
    if (c->fam == EE_I2C) {
        ee_caption(k, _("Las 24xx se protegen con la pata WP (7): a GND se escribe, a 3V3 queda protegida toda. No hay nada que cambiar desde acá."), iw);
        return;
    }
    if (c->fam == EE_MW) {
        ee_caption(k, _("Las 93xx no guardan protección: la app manda EWEN antes de escribir y EWDS al terminar."), iw);
        return;
    }
    char v[64];
    sr_text(v, sizeof v);
    ee_row(k, iw, _("Estado"), v, status_cb, NULL);
    if (c->fam == EE_SPI) {
        int cur = S.have_sr ? (S.sr >> 2) & 3 : 0;
        ee_row(k, iw, _("Bloques protegidos"), S.have_sr ? _(BP_NAME[cur]) : "-", bp_cb, NULL);
        ee_row(k, iw, "WPEN", S.have_sr ? ((S.sr & 0x80) ? _("puesto") : _("no")) : "-", wpen_cb, NULL);
        ee_caption(k, _("BP1 y BP0 protegen un cuarto, la mitad o toda la memoria desde arriba; con WPEN, /WP en bajo traba el estado."), iw);
    } else {
        lv_obj_t *p = ee_pill(k, AOS_SYM_LOCK_OPEN_VARIANT, _("Quitar protección"), AOS_C_CARD2, unprotect_cb, NULL);
        ee_set_disabled(p, J.busy || !S.have_sr || !(S.sr & 0xFC));
        ee_caption(k, _("La flash protege con BP2-BP0 del estado; la app no escribe mientras estén puestos."), iw);
    }
}

void ee_pg_chip(lv_obj_t *parent)
{
    int32_t w = U.CW;
    if (!U.land) {
        lv_obj_t *col = ee_column(parent, w, U.CH);
        card_chip(col, w);
        card_conn(col, w);
        card_detect(col, w);
        card_protect(col, w);
        return;
    }
    int32_t lw = (w - AOS_UI_PAD) / 2, rw = w - lw - AOS_UI_PAD;
    lv_obj_t *l = ee_column(parent, lw, U.CH);
    card_chip(l, lw);
    card_conn(l, lw);
    lv_obj_t *r = ee_column(parent, rw, U.CH);
    lv_obj_set_x(r, lw + AOS_UI_PAD);
    card_detect(r, rw);
    card_protect(r, rw);
}

void ee_pg_chip_job_done(void)
{
    ee_rebuild();
}
