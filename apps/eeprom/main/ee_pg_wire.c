/*
 * P4OS - EEPROM: the Wiring page.
 *
 * The chosen chip as a DIP-8 (or SOIC-8, the same pin-out) seen from above,
 * notch up, pin 1 at the top left, and next to each pin where it goes on the
 * board's 40-pin header: the GPIOs of the port or the ones chosen for
 * Microwire, the address pins tied to 3.3 V or ground as the chosen address
 * asks, WP, HOLD and ORG where they must be, and the nearest 3V3 and GND
 * pins of the header to the data lines. Below, the things that are easy to
 * get wrong: pull-ups, never 5 V, the 24LC1025's A2.
 */
#include "ee.h"

enum { W_VCC, W_GND, W_GPIO, W_HI, W_LO, W_NC };

typedef struct {
    const char *name;
    int kind;
    int gpio;
    lv_color_t color;
} wpin_t;

#define C_PWR  lv_color_hex(0xFF453A)
#define C_GND  lv_color_hex(0x8E8E93)
#define C_DATA AOS_C_TEAL
#define C_CLK  AOS_C_YELLOW
#define C_SEL  AOS_C_PURPLE
#define C_OUT  AOS_C_GREEN
#define C_TIE  lv_color_hex(0xD1A05A)

static int hdr_pin(int gpio)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    return p ? p->pin : -1;
}

/* The header pin labelled 'label' ("3V3", "GND") nearest to pin 'ref'. */
static int near_pin(const char *label, int ref)
{
    const aos_io_pin_t *h = aos_io_header();
    int best = -1, bd = 99;
    for (int i = 0; i < 40; i++) {
        if (strcmp(h[i].label, label)) continue;
        int d = ref > 0 ? abs(h[i].pin - ref) : h[i].pin;
        if (d < bd) { bd = d; best = h[i].pin; }
    }
    return best;
}

/* The eight pins of the chosen chip, pin 1 first. */
static int pins_of(wpin_t *p, int *ref)
{
    const ee_chip_t *c = ee_cur();
    *ref = -1;
    if (c->fam == EE_I2C) {
        const aos_io_port_t *pt = aos_io_port_find(S.conf.i2c_port);
        int sda = pt ? pt->pins[0] : -1, scl = pt ? pt->pins[1] : -1;
        for (int b = 0; b < 3; b++) {
            static const char *const A[3] = { "A0", "A1", "A2" };
            bool used = c->apins & (1 << b);
            bool hi = used ? ((S.conf.i2c_addr >> b) & 1) : (c->blk == 4 && b == 2);
            p[b] = (wpin_t){ A[b], hi ? W_HI : W_LO, -1, C_TIE };
        }
        p[3] = (wpin_t){ "VSS", W_GND, -1, C_GND };
        p[4] = (wpin_t){ "SDA", W_GPIO, sda, C_DATA };
        p[5] = (wpin_t){ "SCL", W_GPIO, scl, C_CLK };
        p[6] = (wpin_t){ "WP", W_LO, -1, C_TIE };
        p[7] = (wpin_t){ "VCC", W_VCC, -1, C_PWR };
        *ref = hdr_pin(sda);
    } else if (c->fam == EE_SPI || c->fam == EE_FLASH) {
        const aos_io_port_t *pt = aos_io_port_find(S.conf.spi_port);
        int sck = pt ? pt->pins[0] : -1, mosi = pt ? pt->pins[1] : -1, miso = pt ? pt->pins[2] : -1;
        int cs = S.conf.spi_cs >= 0 ? S.conf.spi_cs : pt ? pt->pins[3] : -1;
        bool fl = c->fam == EE_FLASH;
        p[0] = (wpin_t){ "/CS", W_GPIO, cs, C_SEL };
        p[1] = (wpin_t){ fl ? "DO" : "SO", W_GPIO, miso, C_OUT };
        p[2] = (wpin_t){ "/WP", W_HI, -1, C_TIE };
        p[3] = (wpin_t){ "VSS", W_GND, -1, C_GND };
        p[4] = (wpin_t){ fl ? "DI" : "SI", W_GPIO, mosi, C_DATA };
        p[5] = (wpin_t){ fl ? "CLK" : "SCK", W_GPIO, sck, C_CLK };
        p[6] = (wpin_t){ "/HOLD", W_HI, -1, C_TIE };
        p[7] = (wpin_t){ "VCC", W_VCC, -1, C_PWR };
        *ref = hdr_pin(sck);
    } else {
        p[0] = (wpin_t){ "CS", W_GPIO, S.conf.mw[MW_CS], C_SEL };
        p[1] = (wpin_t){ "SK", W_GPIO, S.conf.mw[MW_SK], C_CLK };
        p[2] = (wpin_t){ "DI", W_GPIO, S.conf.mw[MW_DI], C_DATA };
        p[3] = (wpin_t){ "DO", W_GPIO, S.conf.mw[MW_DO], C_OUT };
        p[4] = (wpin_t){ "VSS", W_GND, -1, C_GND };
        p[5] = (wpin_t){ "ORG", S.conf.mw_org == 16 ? W_HI : W_LO, -1, C_TIE };
        p[6] = (wpin_t){ "NC", W_NC, -1, C_GND };
        p[7] = (wpin_t){ "VCC", W_VCC, -1, C_PWR };
        *ref = hdr_pin(S.conf.mw[MW_SK]);
    }
    return 8;
}

static void tag_text(const wpin_t *p, int ref, char *out, size_t len)
{
    switch (p->kind) {
    case W_GPIO: {
        int hp = hdr_pin(p->gpio);
        if (hp > 0) snprintf(out, len, "GPIO%d · %s %d", p->gpio, _("pata"), hp);
        else snprintf(out, len, "GPIO%d", p->gpio);
        break;
    }
    case W_VCC:
    case W_HI:  snprintf(out, len, "3V3 · %s %d", _("pata"), near_pin("3V3", ref)); break;
    case W_GND:
    case W_LO:  snprintf(out, len, "GND · %s %d", _("pata"), near_pin("GND", ref)); break;
    default:    snprintf(out, len, "%s", _("sin conectar")); break;
    }
}

/* -------------------------------------------------------------------------- */
/* The drawing                                                                 */
/* -------------------------------------------------------------------------- */

static void draw_chip(lv_obj_t *parent, int32_t w)
{
    const ee_chip_t *c = ee_cur();
    wpin_t p[8];
    int ref;
    pins_of(p, &ref);
    const int32_t pitch = U.land ? 92 : 104, bw = U.land ? 130 : 150;
    const int32_t bh = 4 * pitch + 24, top = 24;
    const int32_t h = bh + 2 * top;
    lv_obj_t *k = ee_box(parent, w, h);
    lv_obj_set_style_bg_color(k, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(k, AOS_UI_RADIUS, 0);
    int32_t bx = (w - bw) / 2;

    /* the legs first, so the body covers their inner ends */
    for (int i = 0; i < 8; i++) {
        bool left = i < 4;
        int slot = left ? i : 7 - i;
        int32_t y = top + 12 + slot * pitch + pitch / 2;
        lv_obj_t *leg = ee_box(k, 40, 16);
        lv_obj_set_style_bg_color(leg, lv_color_hex(0xC7C7CC), 0);
        lv_obj_set_style_bg_opa(leg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(leg, 3, 0);
        lv_obj_set_pos(leg, left ? bx - 28 : bx + bw - 12, y - 8);
    }
    lv_obj_t *body = ee_box(k, bw, bh);
    lv_obj_set_pos(body, bx, top);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 10, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x48484A), 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_t *notch = ee_box(k, 44, 44);
    lv_obj_set_style_bg_color(notch, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(notch, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(notch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(notch, bx + bw / 2 - 22, top - 22);
    lv_obj_t *dot = ee_box(body, 16, 16);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(dot, 34, 26);
    lv_obj_t *nm = aos_label(body, c->name, aos_font_caption, AOS_C_TEXT);
    lv_obj_set_style_transform_rotation(nm, 900, 0);
    lv_obj_update_layout(nm);
    lv_obj_set_style_transform_pivot_x(nm, lv_obj_get_width(nm) / 2, 0);
    lv_obj_set_style_transform_pivot_y(nm, lv_obj_get_height(nm) / 2, 0);
    lv_obj_center(nm);

    int32_t side = bx - 28 - 16;
    for (int i = 0; i < 8; i++) {
        bool left = i < 4;
        int slot = left ? i : 7 - i;
        int32_t y = top + 12 + slot * pitch + pitch / 2;
        /* its number inside the body */
        lv_obj_t *n = aos_label(body, "", aos_font_tiny, AOS_C_DIM);
        lv_label_set_text_fmt(n, "%d", i + 1);
        lv_obj_update_layout(n);
        lv_obj_set_pos(n, left ? 10 : bw - 10 - lv_obj_get_width(n), y - top - 30);
        /* the name and where it goes, outside */
        lv_obj_t *l = aos_label(k, p[i].name, aos_font_body, p[i].color);
        lv_obj_t *tg = aos_label(k, "", aos_font_caption, p[i].kind == W_NC ? AOS_C_DIM : AOS_C_TEXT);
        char t[48];
        tag_text(&p[i], ref, t, sizeof t);
        lv_label_set_text(tg, t);
        lv_label_set_long_mode(tg, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(tg, side, 0);
        lv_obj_update_layout(l);
        lv_obj_update_layout(tg);
        if (left) {
            lv_obj_set_pos(l, bx - 28 - 12 - lv_obj_get_width(l), y - 40);
            lv_obj_set_pos(tg, bx - 28 - 12 - lv_obj_get_width(tg), y + 2);
        } else {
            lv_obj_set_pos(l, bx + bw + 28 + 12, y - 40);
            lv_obj_set_pos(tg, bx + bw + 28 + 12, y + 2);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* The notes                                                                   */
/* -------------------------------------------------------------------------- */

static void note(lv_obj_t *k, int32_t w, const char *t)
{
    lv_obj_t *row = ee_box(k, w, LV_SIZE_CONTENT);
    lv_obj_t *b = aos_label(row, "•", aos_font_small, C_AMBER);
    lv_obj_set_pos(b, 0, 0);
    lv_obj_t *l = aos_label(row, t, aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, w - 28);
    lv_obj_set_pos(l, 28, 0);
}

static void notes(lv_obj_t *parent, int32_t w)
{
    const ee_chip_t *c = ee_cur();
    ee_title(parent, _("A TENER EN CUENTA"));
    lv_obj_t *k = ee_card(parent, w);
    int32_t iw = w - 40;
    note(k, iw, _("Todo a 3V3: el header no aguanta 5 V. Si el chip está en una placa a 5 V, no lo conectes así."));
    note(k, iw, _("Un capacitor de 100 nF entre VCC y GND, pegado al chip."));
    char t[200];
    switch (c->fam) {
    case EE_I2C:
        note(k, iw, _("SDA y SCL necesitan pull-ups de 4,7 kΩ a 3V3, si el módulo no los trae."));
        if (c->apins) {
            snprintf(t, sizeof t, _("A0-A2 eligen la dirección: para 0x%02X van como dice el dibujo (1 = 3V3, 0 = GND)."), S.conf.i2c_addr);
            note(k, iw, t);
        } else {
            note(k, iw, _("Este chip no usa A0-A2 (los bits altos de la dirección van ahí): a GND."));
        }
        if (c->blk == 4) note(k, iw, _("En el 24xx1025, A2 (pata 3) va a 3V3: no es dirección, y sin eso no anda."));
        note(k, iw, _("WP (pata 7) a GND para escribir; a 3V3 queda protegida toda."));
        break;
    case EE_SPI:
    case EE_FLASH:
        note(k, iw, _("/WP (3) y /HOLD (7) a 3V3: en bajo, /HOLD congela el bus y /WP traba la protección."));
        if (S.conf.spi_cs >= 0) {
            snprintf(t, sizeof t, _("El chip select es el GPIO%d, no el cs= del puerto."), S.conf.spi_cs);
            note(k, iw, t);
        }
        if (c->fam == EE_FLASH) note(k, iw, _("Una flash soldada en una placa puede estar alimentando al resto: mejor sacarla, o con la placa sin su micro."));
        break;
    case EE_MW:
        note(k, iw, _("ORG (6) elige palabras de 16 bits (a 3V3) o de 8 (a GND): tiene que coincidir con lo elegido en Chip."));
        note(k, iw, _("DO usa el pull-up interno; con cables largos, uno de 10 kΩ a 3V3."));
        note(k, iw, _("Hay 93xx en SOIC con las patas rotadas (las \"X\" de Microchip): mirá la hoja de datos."));
        note(k, iw, _("CS es activo en alto, al revés que en SPI."));
        break;
    }
}

void ee_pg_wire(lv_obj_t *parent)
{
    int32_t w = U.CW;
    if (!U.land) {
        lv_obj_t *col = ee_column(parent, w, U.CH);
        lv_obj_t *t = aos_label(col, "", aos_font_title, AOS_C_TEXT);
        lv_label_set_text_fmt(t, _("Cómo se conecta el %s"), ee_cur()->name);
        lv_obj_set_style_pad_left(t, 8, 0);
        draw_chip(col, w);
        notes(col, w);
        return;
    }
    int32_t lw = 640, rw = w - lw - AOS_UI_PAD;
    lv_obj_t *l = ee_column(parent, lw, U.CH);
    draw_chip(l, lw);
    lv_obj_t *r = ee_column(parent, rw, U.CH);
    lv_obj_set_x(r, lw + AOS_UI_PAD);
    lv_obj_t *t = aos_label(r, "", aos_font_title, AOS_C_TEXT);
    lv_label_set_text_fmt(t, _("Cómo se conecta el %s"), ee_cur()->name);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, rw);
    notes(r, rw);
}
