/*
 * P4OS - MQTT 3.1.1 client service (aos_mqtt.c).
 *
 * One connection to one broker, owned by a thread of its own, over the HAL's
 * plain TCP (aos_hal_tcp_*; no TLS). It connects with a client id, an
 * optional user and password and a keepalive (clean session), subscribes to
 * the base subscription ("#" by default: everything) plus whatever other
 * code asked for, and keeps a table of every topic it has seen: the last
 * payload, how many messages, when, whether it came retained, and a short
 * history of every number in it (the payload itself when it is a number,
 * each numeric field when it is a JSON object: {"temperature":22.4} gives the
 * series "temperature", {"ENERGY":{"Power":45}} gives "ENERGY.Power").
 *
 * The table is bounded: AOS_MQTT_MAX_TOPICS topics (the one heard from least
 * recently goes when a new one comes in) and a shared pool of
 * AOS_MQTT_SERIES series of AOS_MQTT_HIST points: ~360 KB, allocated in
 * PSRAM when the service starts (plus the 8 KB receive buffer, the queue).
 *
 * It reconnects on its own after a growing pause (1, 2, 5, 10, 30 s) and
 * subscribes again; a broker that refuses the user or the password is not
 * retried until the settings change.
 *
 * Settings live in the preferences (keys mqtt_*). A file mqtt.txt at the
 * root of the card
 *
 *     host=192.168.1.10
 *     port=1883
 *     user=p4os
 *     password=secret
 *     client_id=p4os-bench
 *     sub=zigbee2mqtt/#, home/#
 *
 * is read into them (aos_mqtt_import_file, also tried on its own every few
 * seconds while there is no host); the password moves into the preferences
 * and its line on the card is rewritten.
 *
 * Reading the table: take aos_mqtt_lock(), read, aos_mqtt_unlock(); never
 * keep a pointer after unlocking. aos_mqtt_version() changes whenever
 * anything does, so a UI can poll it cheaply.
 *
 * Everything here may be called from any task (LVGL's included): nothing
 * blocks on the network; publishing only queues.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_MQTT_MAX_TOPICS   512
#define AOS_MQTT_TOPIC_MAX    128     /* longer topics are counted, not kept */
#define AOS_MQTT_PAYLOAD_MAX  256     /* what is kept of each payload */
#define AOS_MQTT_PUB_MAX      512     /* the largest payload aos_mqtt_publish takes */
#define AOS_MQTT_FIELD_MAX    32
#define AOS_MQTT_FIELDS       10      /* numeric series per topic */
#define AOS_MQTT_HIST         40      /* points per series */
#define AOS_MQTT_SERIES       384     /* series in the pool, for all topics */

typedef enum {
    AOS_MQTT_OFF = 0,           /* switched off in the settings */
    AOS_MQTT_UNCONFIGURED,      /* no broker address */
    AOS_MQTT_WAITING_NET,       /* the Wi-Fi is not up */
    AOS_MQTT_CONNECTING,
    AOS_MQTT_REFUSED,           /* user or password refused: waits for new settings */
    AOS_MQTT_CONNECTED,
    AOS_MQTT_ERROR,             /* lost or refused; retrying */
} aos_mqtt_state_t;

typedef enum {
    AOS_MQTT_EMPTY = 0,
    AOS_MQTT_TEXT,
    AOS_MQTT_NUMBER,            /* the whole payload is a number */
    AOS_MQTT_JSON,              /* an object or an array */
    AOS_MQTT_BINARY,            /* not UTF-8 text */
} aos_mqtt_kind_t;

typedef struct {
    char     topic[AOS_MQTT_TOPIC_MAX];
    char     payload[AOS_MQTT_PAYLOAD_MAX];   /* NUL-terminated, cut at the size */
    uint32_t len;               /* the payload's real length */
    uint32_t count;             /* messages since it was first seen */
    uint32_t first_ms, last_ms; /* uptime */
    int64_t  last_wall;         /* time() of the last one, 0 without a clock */
    uint32_t seq;               /* aos_mqtt_version() when it last changed */
    uint32_t hash;
    uint8_t  qos;
    bool     retained;          /* the last one came as a retained message */
    uint8_t  kind;              /* aos_mqtt_kind_t */
    uint8_t  nfields;           /* numeric series it has */
    int16_t  series;            /* internal: first series in the pool */
    bool     used;
} aos_mqtt_topic_t;

typedef struct {
    uint32_t rx_msgs, tx_msgs;      /* PUBLISH packets in and out */
    uint32_t rx_bytes, tx_bytes;    /* everything on the socket */
    uint32_t connects, drops;       /* sessions opened, sessions lost */
    uint32_t pending;               /* QoS 1 publishes waiting for their PUBACK */
    uint32_t queued;                /* publishes waiting to be sent */
    uint32_t cut, evicted, too_long;/* payloads kept cut, topics dropped for room, topics too long to keep */
    uint32_t connected_ms;          /* uptime when this session began, 0 if none */
    int32_t  ping_ms;               /* last PINGREQ -> PINGRESP, -1 unknown */
    int      topics;                /* in the table */
    int      subs_refused;          /* filters the broker refused in its SUBACK */
} aos_mqtt_stats_t;

typedef struct {
    bool enabled;
    char host[64];
    int  port;                  /* 1883 */
    char user[48];
    char pass[64];
    char client_id[48];         /* "" = <device name>-xxxx, made up once */
    char sub[128];              /* one or more filters, comma-separated; "#" */
    int  keepalive;             /* seconds, 30 */
} aos_mqtt_config_t;

/* Starts the service thread (idempotent). The app, the portal and
 * aos_mqtt_publish() call it. */
void aos_mqtt_start(void);

aos_mqtt_state_t aos_mqtt_state(void);
const char      *aos_mqtt_error(void);      /* the last reason, for the user ("" if none) */
uint32_t         aos_mqtt_version(void);
void             aos_mqtt_stats(aos_mqtt_stats_t *out);

/* Configuration. set_config saves and reconnects; the password is only
 * changed when c->pass is not the placeholder AOS_MQTT_PASS_KEPT. */
#define AOS_MQTT_PASS_KEPT "(guardada en P4OS)"
void aos_mqtt_config(aos_mqtt_config_t *out);
bool aos_mqtt_set_config(const aos_mqtt_config_t *c);
void aos_mqtt_set_enabled(bool on);
bool aos_mqtt_import_file(void);            /* mqtt.txt on the card; true if it brought something */
void aos_mqtt_reconnect(void);
const char *aos_mqtt_client_id(void);       /* the one in use */

/* Publishing: queued for the service's thread, sent as soon as there is a
 * connection (a publish waits up to 30 s for one). false if there is no
 * broker configured, the topic is empty or has wildcards, the payload is
 * longer than AOS_MQTT_PUB_MAX or the queue (8) is full. len -1 = strlen. */
bool aos_mqtt_publish(const char *topic, const char *payload, int qos, bool retain);
bool aos_mqtt_publish_len(const char *topic, const void *payload, int len, int qos, bool retain);

/* Extra subscriptions for other code, kept across reconnections (up to 8,
 * besides the base one). The messages land in the same table. */
bool aos_mqtt_subscribe(const char *filter, int qos);
bool aos_mqtt_unsubscribe(const char *filter);

/* The table */
void aos_mqtt_lock(void);
void aos_mqtt_unlock(void);
int                     aos_mqtt_topic_count(void);        /* topics in use */
const aos_mqtt_topic_t *aos_mqtt_topic_at(int slot);       /* 0..AOS_MQTT_MAX_TOPICS-1, NULL if free */
int                     aos_mqtt_topic_find(const char *topic);   /* slot or -1 */
/* The numeric series of a topic: their names ("" for a payload that is a
 * number itself), in the order they appeared, and the last value of each
 * (last may be NULL). Returns how many. */
int  aos_mqtt_fields(int slot, char names[][AOS_MQTT_FIELD_MAX], float *last, int max);
/* One series, oldest first: values and the uptime of each (t_ms may be
 * NULL). Returns how many points. */
int  aos_mqtt_history(int slot, const char *field, float *v, uint32_t *t_ms, int max);
/* Forgets every topic (the subscription stays: retained messages come back
 * on the next connection). */
void aos_mqtt_clear(void);

/* Whether a topic matches a filter with + and # (MQTT 3.1.1 rules). */
bool aos_mqtt_match(const char *filter, const char *topic);

/* A JSON text re-indented two spaces a level into out (always
 * NUL-terminated); a payload that is not JSON is copied as it is. Tolerant
 * of a payload cut short. */
void aos_mqtt_pretty(const char *json, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
