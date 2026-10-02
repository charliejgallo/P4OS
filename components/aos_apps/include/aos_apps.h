#pragma once
#include "aos_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers every built-in app with the UI runtime. */
void aos_apps_register_builtin(void);

/* Each app exposes its descriptor+callbacks through this function. */
void aos_app_calc_get(aos_app_t *app);
void aos_app_serial_get(aos_app_t *app);
void aos_app_clock_get(aos_app_t *app);
void aos_app_elec_get(aos_app_t *app);
void aos_app_settings_get(aos_app_t *app);
void aos_app_music_get(aos_app_t *app);
void aos_app_photos_get(aos_app_t *app);
void aos_app_calendar_get(aos_app_t *app);
void aos_app_convert_get(aos_app_t *app);
void aos_app_life_get(aos_app_t *app);
void aos_app_bus_get(aos_app_t *app);
void aos_app_ha_get(aos_app_t *app);
void aos_app_modbus_get(aos_app_t *app);
void aos_app_bench_get(aos_app_t *app);
void aos_app_files_get(aos_app_t *app);
void aos_app_mqtt_get(aos_app_t *app);
void aos_app_net_get(aos_app_t *app);
void aos_app_sysmon_get(aos_app_t *app);       /* also registers "widget sysmon" */
void aos_app_claude_get(aos_app_t *app);
void aos_claude_widget_register(void);
void aos_app_modules_get(aos_app_t *app);      /* the header, modules.txt, the sensors */
void aos_app_leds_get(aos_app_t *app);         /* Tiras LED: addressable strips, WLED's way */
void aos_app_macropad_get(aos_app_t *app);     /* layout and runner in aos_macropad.h */
void aos_app_flasher_get(aos_app_t *app);
void aos_ha_widget_register(void);

/* Services that run without their app (alarms); from the main loop, 5 Hz. */
void aos_apps_service_tick(void);
void aos_clock_service_tick(void);
void aos_app_flasher_service_tick(void);    /* "Grabación terminada" with the app closed */
bool aos_alarm_get(int index, int *minute_of_day, bool *enabled, int *days);
bool aos_alarm_set(int index, int minute_of_day, bool enabled, int days);

/* The planned apps, as placeholders (aos_app_soon.c). */
void aos_app_soon_register(void);

#ifdef __cplusplus
}
#endif
