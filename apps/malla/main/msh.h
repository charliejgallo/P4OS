/*
 * Malla - a Meshtastic client for P4OS: the model and the session.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Three layers, so each can change without the others:
 *
 *   msh_link_*   the way to the node (serial, TCP, BLE): whole protobufs
 *   msh_sess.c   the client API over a link, in a thread of its own: the
 *                want_config handshake, the heartbeat, what goes out, what
 *                comes in, and coming back by itself when the link drops
 *   msh_model.c  what is known: nodes, messages, telemetry, the node's
 *                config and channels. Filled by the session, read by the UI
 *                and the portal, behind one mutex (msh_lock).
 *
 * The UI (msh_ui_*.c) only reads the model and calls the msh_send_* and
 * msh_set_* functions, which encode a ToRadio and queue it for the session.
 */
#pragma once

#include "msh_link.h"
#include "msh_util.h"

#include "meshtastic/admin.pb.h"
#include "meshtastic/apponly.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/telemetry.pb.h"

#include <stdbool.h>
#include <stdint.h>

#define MSH_BROADCAST   0xFFFFFFFFu
#define MSH_NODES_MAX   300
#define MSH_MSGS_MAX    800
#define MSH_TELE_NODES  64          /* nodes whose telemetry is kept over time */
#define MSH_TELE_N      96          /* samples kept for each */
#define MSH_CHANNELS    8
#define MSH_TEXT_MAX    234         /* Data.payload holds 233 bytes */
#define MSH_LOG_LINES   24          /* the node's own log, the last lines */

/* ---- nodes ---- */

typedef struct {
    uint32_t num;
    char     long_name[40];
    char     short_name[8];
    int      hw_model;              /* meshtastic_HardwareModel */
    int      role;                  /* meshtastic_Config_DeviceConfig_Role */
    bool     has_user;
    bool     has_pos;
    int32_t  lat_i, lon_i;          /* 1e-7 degrees */
    int32_t  alt;                   /* metres */
    uint32_t pos_time;              /* the position's own time, epoch */
    float    snr;
    int16_t  rssi;
    bool     has_rssi;
    int8_t   hops;                  /* -1 unknown */
    uint8_t  channel;               /* the channel it was heard on (for a DM) */
    uint32_t last_heard;            /* epoch, from the node */
    uint32_t heard_up;              /* msh_now_s() when this session heard it, 0 never */
    int16_t  battery;               /* %, 101 = powered, -1 unknown */
    float    voltage, ch_util, air_tx;
    uint32_t uptime;
    bool     has_dev;
    bool     has_env;
    float    temp, hum, press;
    bool     favorite, via_mqtt, has_pkc;
    int16_t  tele;                  /* index into the telemetry history, -1 none */
} msh_node_t;

/* ---- telemetry over time ---- */

enum {
    MSH_T_BATT = 1 << 0, MSH_T_VOLT = 1 << 1, MSH_T_CHU = 1 << 2, MSH_T_AIR = 1 << 3,
    MSH_T_TEMP = 1 << 4, MSH_T_HUM = 1 << 5, MSH_T_PRESS = 1 << 6,
};

typedef struct {
    uint32_t up;                    /* msh_now_s() when it came */
    uint32_t epoch;                 /* its own time, 0 unknown */
    uint8_t  has;                   /* MSH_T_* */
    uint8_t  batt;
    float    volt, chu, air, temp, hum, press;
} msh_tsample_t;

typedef struct {
    uint32_t num;
    int      n, head;               /* a ring, oldest at head */
    msh_tsample_t s[MSH_TELE_N];
} msh_tele_t;

/* ---- messages ---- */

typedef enum {
    MSH_MSG_RX = 0,                 /* came from the mesh */
    MSH_MSG_QUEUED,                 /* ours, not yet out of the node */
    MSH_MSG_SENT,                   /* the node transmitted it */
    MSH_MSG_ACKED,                  /* the other end said so (a DM), or someone repeated it (a channel) */
    MSH_MSG_FAILED,
} msh_msg_state_t;

typedef struct {
    uint32_t id;                    /* the packet's */
    uint32_t from, to;
    uint32_t peer;                  /* the conversation: the other node for a DM, 0 for a channel */
    uint8_t  ch;                    /* the channel's index */
    uint8_t  state;                 /* msh_msg_state_t */
    uint8_t  err;                   /* meshtastic_Routing_Error when FAILED */
    uint8_t  unread;
    uint32_t epoch;                 /* when, if known */
    uint32_t up;                    /* msh_now_s() when it came or went */
    int8_t   snr;
    int16_t  rssi;
    int8_t   hops;
    uint32_t reply_id;              /* an answer to, or a reaction to */
    uint8_t  reaction;              /* the text is an emoji put on reply_id */
    char     text[MSH_TEXT_MAX];
} msh_msg_t;

/* ---- traceroute ---- */

typedef struct {
    uint32_t target, req_id;
    uint8_t  state;                 /* 0 none, 1 waiting, 2 done, 3 no answer */
    uint32_t t0;                    /* msh_now_s() when asked */
    int      n_to, n_back;
    uint32_t to[8], back[8];
    int8_t   snr_to[9], snr_back[9];/* quarter dB, as they come; INT8_MIN unknown */
} msh_trace_t;

/* ---- the session ---- */

typedef enum {
    MSH_ST_IDLE = 0,                /* nothing chosen, or stopped */
    MSH_ST_OPENING,                 /* reaching the node */
    MSH_ST_CONFIG,                  /* the node is sending its database */
    MSH_ST_READY,
    MSH_ST_RETRY,                   /* the link dropped: trying again soon */
} msh_state_t;

/* ---- all of it ---- */

typedef struct {
    void *mx;
    /* changes, for whoever shows them */
    uint32_t seq_nodes, seq_msgs, seq_cfg, seq_state, seq_trace;
    uint32_t rx_count;              /* new incoming messages, ever */

    /* the session */
    msh_state_t state;
    msh_target_t target;
    char err[96];                   /* why the last try failed: a key for _() */
    const char *note;
    msh_ask_t ask;
    uint32_t ask_num;               /* the number a pairing shows (MSH_ASK_SHOW, _COMPARE) */
    uint32_t ready_up;              /* msh_now_s() when it became READY */
    uint32_t pkts_in, pkts_out;
    char log[MSH_LOG_LINES][96];
    int log_n, log_head;

    /* the node we talk to */
    uint32_t my_num;
    char fw[20];
    int hw_model;
    int role;
    bool has_wifi, has_bt, has_pkc;
    uint16_t nodedb_count;
    bool have_lora;
    meshtastic_Config_LoRaConfig lora;
    bool have_dev;
    meshtastic_Config_DeviceConfig dev;
    bool have_bt_cfg;
    meshtastic_Config_BluetoothConfig bt_cfg;
    meshtastic_Channel ch[MSH_CHANNELS];

    /* what is known */
    msh_node_t *nodes;              /* MSH_NODES_MAX */
    int n_nodes;
    msh_msg_t *msgs;                /* a ring of MSH_MSGS_MAX, oldest at msg_head */
    int msg_head, msg_n;
    msh_tele_t *tele;               /* MSH_TELE_NODES */
    int n_tele;
    msh_trace_t trace;
    bool dirty;                     /* something to save */

    /* what the node says to the person (ClientNotification), for a toast */
    char notice[160];
    uint32_t seq_notice;
    /* the newest time the mesh told us and when: the board may have no
     * clock of its own, the mesh often has one (msh_epoch_guess) */
    uint32_t node_epoch, node_epoch_up;
} msh_model_t;

extern msh_model_t MSH;

/* msh_model.c */
bool msh_model_init(void);
void msh_model_free(void);
void msh_lock(void);
void msh_unlock(void);
/* Everything below is called with the lock held. */
msh_node_t *msh_node_find(uint32_t num);
msh_node_t *msh_node_get(uint32_t num);         /* found or added */
msh_msg_t  *msh_msg_at(int i);                  /* 0 the oldest */
msh_msg_t  *msh_msg_by_id(uint32_t id);
msh_msg_t  *msh_msg_add(void);                  /* a new one at the end, zeroed */
msh_tele_t *msh_tele_of(uint32_t num, bool make);
const char *msh_node_name(uint32_t num, char *buf, size_t cap);  /* long name, or "!a1b2c3d4" */
const char *msh_node_short(uint32_t num, char *buf, size_t cap); /* short name, or the last 4 hex */
const char *msh_channel_name(int idx, char *buf, size_t cap);    /* "LongFast" for an unnamed primary... */
/* Now, in seconds since 1970: the board's clock, or the mesh's last word
 * moved on by the uptime since, or 0. */
uint32_t    msh_epoch_guess(void);
/* How long since the node was heard, in seconds, or -1. */
int32_t     msh_node_age(const msh_node_t *n);
/* The distance from our node, in metres, or -1. */
float       msh_node_dist(const msh_node_t *n);
void        msh_log_add(const char *line);
/* Applies one FromRadio (msh_sess.c calls it). */
void        msh_model_apply(const meshtastic_FromRadio *fr);
const char *msh_hw_name(int model);             /* pb/msh_hw_names.c, NULL unknown */
const char *msh_preset_name(int preset);
const char *msh_region_name(int region);
const char *msh_role_name(int role);
const char *msh_routing_error(int err);         /* a key for _() */
int         msh_unread(uint32_t peer, int ch);  /* in one conversation */
int         msh_unread_all(void);
void        msh_mark_read(uint32_t peer, int ch);

/* msh_store.c: the card. A file per node we talked to (its number), with
 * the nodes, the messages and the telemetry; the chosen link in a pref. */
void msh_store_load(uint32_t my_num);
void msh_store_save(void);                      /* if dirty; takes the lock itself */
void msh_store_save_locked(void);               /* the same, with the lock held */
bool msh_target_load(msh_target_t *t);
void msh_target_save(const msh_target_t *t);
void msh_target_text(const msh_target_t *t, char *out, size_t cap);   /* "TCP 10.0.0.5:4403" */

/* msh_sess.c */
void        msh_sess_start(const msh_target_t *t);
void        msh_sess_stop(void);                /* waits for the thread */
bool        msh_sess_running(void);
void        msh_sess_passkey(uint32_t pin);     /* the answer to MSH_ASK_PIN (the PIN) or _COMPARE (1/0) */
/* What goes out. Each returns false when it could not be queued. The
 * message is in the model already as MSH_MSG_QUEUED when send_text
 * returns true. */
bool msh_send_text(uint32_t to, int ch, const char *text, uint32_t reply_id, bool reaction);
bool msh_send_traceroute(uint32_t to);
bool msh_send_telemetry_req(uint32_t to);
bool msh_send_position_req(uint32_t to);
bool msh_set_owner(const char *long_name, const char *short_name);
bool msh_set_lora(const meshtastic_Config_LoRaConfig *lora);
bool msh_set_channel(const meshtastic_Channel *c);
bool msh_set_favorite(uint32_t num, bool on);
/* A channel set from a URL: replaces the channels (and the LoRa settings
 * when it has them) inside one edit, so the node restarts once. */
bool msh_apply_channel_set(const meshtastic_ChannelSet *cs);

/* msh_url.c: the meshtastic.org/e/#... links */
bool msh_url_make(char *out, size_t cap);       /* the node's channels now; takes the lock */
bool msh_url_parse(const char *url, meshtastic_ChannelSet *out);
