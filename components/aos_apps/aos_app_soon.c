/*
 * P4OS - The apps that are planned but not written yet.
 *
 * Each one is a real entry on the home screen with its final id, name, icon
 * and colours, and opens to a card that says which phase of PLAN.md brings
 * it. That way the home screen is laid out with what it will actually hold,
 * the ids are fixed from day one (menu.txt names them), and the roadmap is
 * on the device.
 *
 * When an app gets written, its line here goes and its own file comes in.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <string.h>

typedef struct {
    const char *id, *name, *glyph;
    uint32_t a, b;
    int phase;
    const char *what;
} soon_t;

/* The order field of each app is its index here, times ten, plus 100. */
static const soon_t SOON[] = {
    { "aos.ha",       "Home Assistant", AOS_SYM_HOME_ASSISTANT, 0x41BDF5, 0x0B6FB0, 5,
      N_("Paneles con tus entidades, escenas y sensores, por la API WebSocket.") },
    { "aos.serial",   "Terminal",       AOS_SYM_SERIAL_PORT,    0x2F3B45, 0x10161B, 5,
      N_("Monitor serie de los puertos del header y del USB, con disparadores y grabación.") },
    { "aos.flasher",  "Programador",    AOS_SYM_CHIP,           0x8B5CF6, 0x4C1D95, 5,
      N_("Graba ESP32, ESP8266 y STM32 por el header, o desde la Mac por RFC2217.") },
    { "aos.bus",      "Bus",            AOS_SYM_CONNECTION,     0xF59E0B, 0xB45309, 5,
      N_("Scanner I2C, registros, GPIO en vivo, PWM y ADC.") },
    { "aos.modbus",   "Modbus",         AOS_SYM_LAN,            0x10B981, 0x047857, 5,
      N_("Maestro RTU y TCP, con perfiles para el gateway DOMCOM y la Riden.") },
    { "aos.elec",     "Electrónica",    AOS_SYM_LIGHTNING_BOLT, 0xFACC15, 0xCA8A04, 5,
      N_("Colores de resistencias, códigos SMD, Ohm, divisores, 555.") },
    { "aos.settings", "Ajustes",        AOS_SYM_COG,            0x8E8E93, 0x48484A, 4,
      N_("WiFi, pantalla, sonido, idioma, módulos, respaldo.") },
    { "aos.clock",    "Reloj",          AOS_SYM_CLOCK_OUTLINE,  0x2C2C2E, 0x000000, 4,
      N_("Reloj mundial, alarmas, temporizador, cronómetro y pomodoro, en una app.") },
    { "aos.music",    "Música",         AOS_SYM_MUSIC,          0xFF375F, 0xB0123F, 4,
      N_("Las canciones de la SD y la radio por internet.") },
    { "aos.photos",   "Fotos",          AOS_SYM_IMAGE,          0xFBBF24, 0xEC4899, 4,
      N_("Las fotos de la SD, con zoom de dos dedos.") },
    { "aos.calendar", "Calendario",     AOS_SYM_CALENDAR,       0xF87171, 0xDC2626, 4,
      N_("El mes, y más adelante el calendario de Home Assistant.") },
};
#define N_SOON (sizeof SOON / sizeof SOON[0])

static const soon_t *find(const char *id)
{
    for (size_t i = 0; i < N_SOON; i++) if (!strcmp(SOON[i].id, id)) return &SOON[i];
    return NULL;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    const soon_t *s = find(self->desc.id);
    lv_obj_t *page = aos_page(root);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 20, 0);
    lv_obj_set_style_pad_hor(page, 60, 0);

    lv_obj_t *icon = aos_icon_create(page, &self->desc, 200);
    (void)icon;
    lv_obj_t *name = aos_label(page, aos_tr(self->desc.name), aos_font_title, AOS_C_TEXT);
    (void)name;
    lv_obj_t *what = aos_label(page, s ? aos_tr(s->what) : "", aos_font_body, AOS_C_DIM);
    lv_obj_set_width(what, lv_pct(100));
    lv_label_set_long_mode(what, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(what, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *chip = lv_obj_create(page);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(chip, 30, 0);
    lv_obj_set_style_bg_color(chip, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, 28, 0);
    lv_obj_set_style_pad_ver(chip, 12, 0);
    lv_obj_t *cl = lv_label_create(chip);
    lv_obj_set_style_text_font(cl, aos_font_small, 0);
    lv_label_set_text_fmt(cl, _("En construcción · fase %d"), s ? s->phase : 0);
    return NULL;
}

void aos_app_soon_register(void)
{
    for (size_t i = 0; i < N_SOON; i++) {
        const soon_t *s = &SOON[i];
        if (aos_ui_app_find(s->id)) continue;     /* the real one is in */
        aos_app_t app = {
            .desc = {
                .id = s->id, .name = s->name, .icon = s->glyph,
                .color_a = s->a, .color_b = s->b,
                .flags = AOS_APP_FLAG_NONE,
                .order = 100 + (int32_t)i * 10,
            },
            .create = create,
        };
        aos_ui_register_app(&app);
    }
}
