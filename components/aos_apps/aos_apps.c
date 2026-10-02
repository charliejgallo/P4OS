#include "aos_apps.h"
#include "aos_ui.h"
#include "aos_sensors.h"
#include "aos_leds.h"

/* The built-in apps that are ported so far (the rest wait in _pending/, see
 * docs/plan/APPS.md), then the placeholders of the planned ones. */
void aos_apps_register_builtin(void)
{
    void (*const getters[])(aos_app_t *) = {
        aos_app_calc_get,
        aos_app_serial_get,
        aos_app_clock_get,
        aos_app_elec_get,
        aos_app_settings_get,
        aos_app_music_get,
        aos_app_photos_get,
        aos_app_calendar_get,
        aos_app_convert_get,
        aos_app_life_get,
        aos_app_bus_get,
        aos_app_ha_get,
        aos_app_modbus_get,
        aos_app_flasher_get,
        aos_app_bench_get,
        aos_app_files_get,
        aos_app_mqtt_get,
        aos_app_net_get,
        aos_app_sysmon_get,
        aos_app_claude_get,
        aos_app_modules_get,
        aos_app_leds_get,
        aos_app_macropad_get,
    };
    for (size_t i = 0; i < sizeof getters / sizeof getters[0]; i++) {
        aos_app_t app;
        getters[i](&app);
        aos_ui_register_app(&app);
    }
    aos_app_soon_register();
    aos_ha_widget_register();       /* "widget ha" on the home screen */
    aos_claude_widget_register();   /* "widget claude" */
}

/* The services of the built-in apps that must run with the app closed (the
 * alarms). Called 5 times a second from the main loop, with the lock. */
void aos_apps_service_tick(void)
{
    aos_clock_service_tick();
    aos_app_flasher_service_tick();
    aos_sensors_autostart();        /* the sensors of modules.txt, read from boot */
    aos_leds_autostart();           /* a LED strip left on, back as it was */
}
