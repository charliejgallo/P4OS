/*
 * P4OS - Home Assistant client service (aos_ha.c).
 *
 * One connection to HA's WebSocket API, owned by a thread of its own: it
 * authenticates with a long-lived token, loads the areas, the entity
 * registry and every state, subscribes to state_changed, and keeps a table
 * of the entities worth showing (lights, switches, covers, climates,
 * sensors...) up to date. The app and the home widget read that table; the
 * service calls they make go through a queue to the same thread.
 *
 * Configuration: the URL and the token live in the preferences. The token is
 * 180-odd characters, not something to type on a screen: put a file ha.txt at
 * the root of the card
 *
 *     url=http://192.168.1.10:8123
 *     token=eyJhbGciOi...
 *
 * and the service moves the token into the preferences and blanks it in the
 * file (aos_ha_import_file, also tried on its own every few seconds while
 * there is no token).
 *
 * Reading the table: take aos_ha_lock(), read, aos_ha_unlock(); never keep a
 * pointer after unlocking. aos_ha_version() changes whenever anything in it
 * does, so a UI can poll it cheaply.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AOS_HA_UNCONFIGURED = 0,    /* no URL or no token */
    AOS_HA_WAITING_NET,         /* configured, the Wi-Fi is not up */
    AOS_HA_CONNECTING,
    AOS_HA_AUTH_FAILED,         /* the token was refused: no retries until it changes */
    AOS_HA_LOADING,             /* authenticated, loading states */
    AOS_HA_READY,
    AOS_HA_ERROR,               /* lost or refused; retrying */
} aos_ha_state_t;

typedef enum {
    HA_LIGHT = 0, HA_SWITCH, HA_INPUT_BOOLEAN, HA_FAN, HA_COVER, HA_CLIMATE, HA_LOCK,
    HA_MEDIA, HA_SCENE, HA_SCRIPT, HA_BUTTON, HA_SENSOR, HA_BINARY_SENSOR, HA_DOMAIN_COUNT
} aos_ha_domain_t;

#define AOS_HA_MAX_ENTITIES 400
#define AOS_HA_MAX_AREAS    32

typedef struct {
    char     id[64];                /* entity_id */
    char     name[48];              /* friendly_name */
    char     state[32];             /* "on", "23.4", "open", "unavailable"... */
    char     unit[12];
    char     device_class[20];
    char     icon[32];              /* "mdi:coffee-maker" if HA has one */
    uint8_t  domain;                /* aos_ha_domain_t */
    int8_t   area;                  /* index in the areas, -1 = none */
    int16_t  brightness;            /* 0-255, -1 = not dimmable */
    int16_t  position;              /* cover 0-100 / fan percentage, -1 = none */
    float    current_temp, target_temp, temp_min, temp_max, temp_step;   /* climate */
    char     hvac_action[12];
    uint16_t hvac_modes;            /* bit per AOS_HA_HVAC_* */
    bool     pending;               /* a call was sent, the new state has not come back */
    uint32_t changed_ms;            /* uptime when it last changed here */
} aos_ha_entity_t;

enum { AOS_HA_HVAC_OFF, AOS_HA_HVAC_HEAT, AOS_HA_HVAC_COOL, AOS_HA_HVAC_HEAT_COOL, AOS_HA_HVAC_AUTO,
       AOS_HA_HVAC_DRY, AOS_HA_HVAC_FAN_ONLY, AOS_HA_HVAC_COUNT };
extern const char *const aos_ha_hvac_names[AOS_HA_HVAC_COUNT];

/* Starts the service thread (idempotent). The app and the widget call it. */
void aos_ha_start(void);

aos_ha_state_t aos_ha_state(void);
const char *aos_ha_error(void);         /* the last reason, for the user */
const char *aos_ha_location(void);      /* HA's name for the house ("Casa") */
uint32_t    aos_ha_version(void);

void aos_ha_lock(void);
void aos_ha_unlock(void);
int                    aos_ha_count(void);
const aos_ha_entity_t *aos_ha_at(int i);
int                    aos_ha_find(const char *entity_id);   /* index or -1 */
int                    aos_ha_area_count(void);
const char            *aos_ha_area_name(int i);

/* Queues a service call; data_json is the service_data object ("{}" or NULL
 * for none). The entity is marked pending and, for the obvious ones
 * (toggle, turn_on/off, open/close), its state is flipped at once so the
 * tile answers the finger; HA's state_changed then confirms or corrects it. */
bool aos_ha_call(const char *domain, const char *service, const char *entity_id, const char *data_json);
/* The usual tap on a tile: toggle, open/close, run, play/pause. */
bool aos_ha_tap(const char *entity_id);

/* Favourites: the first page of the app and the home widget. Kept in the
 * preferences as a list of entity ids. */
bool aos_ha_is_fav(const char *entity_id);
void aos_ha_set_fav(const char *entity_id, bool on);
int  aos_ha_favs(char ids[][64], int max);      /* in the order they were added */

/* History of one entity, for the sheet's chart (history/history_during_period):
 * one request at a time, the latest wins. request() is false without a
 * connection or a valid clock. history() copies up to 'max' points
 * (values, and seconds from the start of the period), with the range and
 * the period's length; it returns how many, or -AOS_HA_HIST_* while there is
 * nothing to show (none asked for this entity, queued, waiting, empty,
 * failed). Numeric states only. */
#define AOS_HA_HIST_MAX 360
enum { AOS_HA_HIST_NONE = 0, AOS_HA_HIST_QUEUED, AOS_HA_HIST_WAITING, AOS_HA_HIST_READY, AOS_HA_HIST_EMPTY, AOS_HA_HIST_FAILED };
bool aos_ha_history_request(const char *entity_id, int hours);
int  aos_ha_history(const char *entity_id, float *v, uint32_t *t, int max, float *lo, float *hi, uint32_t *span_s);

/* Notifications: HA's persistent notifications (added, dismissed) and the
 * event "p4os_notify" (data: title, message) arrive in the system's
 * notification centre, from "Home Assistant". Nothing to call. */

/* Configuration */
bool        aos_ha_configured(void);
const char *aos_ha_url(void);
bool        aos_ha_set_url(const char *url);    /* http://host:port; reconnects */
void        aos_ha_set_token(const char *token);
bool        aos_ha_has_token(void);
bool        aos_ha_import_file(void);           /* ha.txt on the card; true if it brought something */
void        aos_ha_reconnect(void);

#ifdef __cplusplus
}
#endif
