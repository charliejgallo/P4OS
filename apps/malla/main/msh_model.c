/*
 * Malla - what is known about the mesh, and how each FromRadio changes it.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * The node sends its whole picture at connect time (my_info, metadata, its
 * config, its channels, a node_info for every node in its database, then
 * config_complete_id) and after that every packet it hears that is for us
 * or for a channel we have. Here all of it lands in plain structs in PSRAM:
 * the nanopb ones are big (a FromRadio is ~1.5 KB) and full of things the
 * screens never show.
 *
 * Delivery of what we send, as the firmware reports it:
 *   QueueStatus with our packet's id: it left the phone queue (res 0 is
 *     "taken", anything else is the queue refusing it);
 *   a Routing packet whose request_id is our id: error NONE from the
 *     destination is a real ACK; NONE from our own node is the implicit
 *     ACK (it heard a neighbour repeat it), which for a channel message is
 *     as good as it gets; any other error (MAX_RETRANSMIT, NO_ROUTE...) is
 *     a failure, kept with its reason.
 */
#include "msh.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include "pb_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

msh_model_t MSH;

bool msh_model_init(void)
{
    if (!MSH.mx) MSH.mx = aos_hal_mutex_create();
    if (!MSH.nodes) MSH.nodes = (msh_node_t *)msh_alloc(sizeof(msh_node_t) * MSH_NODES_MAX);
    if (!MSH.msgs) MSH.msgs = (msh_msg_t *)msh_alloc(sizeof(msh_msg_t) * MSH_MSGS_MAX);
    if (!MSH.tele) MSH.tele = (msh_tele_t *)msh_alloc(sizeof(msh_tele_t) * MSH_TELE_NODES);
    return MSH.mx && MSH.nodes && MSH.msgs && MSH.tele;
}

void msh_model_free(void)
{
    free(MSH.nodes);
    free(MSH.msgs);
    free(MSH.tele);
    void *mx = MSH.mx;
    memset(&MSH, 0, sizeof MSH);
    MSH.mx = mx;                    /* the mutex stays: there is no destroy in the HAL */
}

void msh_lock(void) { aos_hal_mutex_lock(MSH.mx); }
void msh_unlock(void) { aos_hal_mutex_unlock(MSH.mx); }

/* ---- lookups ---- */

msh_node_t *msh_node_find(uint32_t num)
{
    for (int i = 0; i < MSH.n_nodes; i++)
        if (MSH.nodes[i].num == num) return &MSH.nodes[i];
    return NULL;
}

msh_node_t *msh_node_get(uint32_t num)
{
    msh_node_t *n = msh_node_find(num);
    if (n) return n;
    if (MSH.n_nodes >= MSH_NODES_MAX) {
        /* full: the one heard longest ago goes, never ours */
        int old = -1;
        int32_t worst = -2;
        for (int i = 0; i < MSH.n_nodes; i++) {
            if (MSH.nodes[i].num == MSH.my_num || MSH.nodes[i].favorite) continue;
            int32_t a = msh_node_age(&MSH.nodes[i]);
            if (a < 0) a = 0x7FFFFFFF;
            if (a > worst) { worst = a; old = i; }
        }
        if (old < 0) return NULL;
        MSH.nodes[old] = MSH.nodes[--MSH.n_nodes];
    }
    n = &MSH.nodes[MSH.n_nodes++];
    memset(n, 0, sizeof *n);
    n->num = num;
    n->hops = -1;
    n->battery = -1;
    n->tele = -1;
    MSH.seq_nodes++;
    return n;
}

msh_msg_t *msh_msg_at(int i)
{
    if (i < 0 || i >= MSH.msg_n) return NULL;
    return &MSH.msgs[(MSH.msg_head + i) % MSH_MSGS_MAX];
}

msh_msg_t *msh_msg_by_id(uint32_t id)
{
    if (!id) return NULL;
    for (int i = MSH.msg_n - 1; i >= 0; i--) {
        msh_msg_t *m = msh_msg_at(i);
        if (m->id == id) return m;
    }
    return NULL;
}

msh_msg_t *msh_msg_add(void)
{
    msh_msg_t *m;
    if (MSH.msg_n < MSH_MSGS_MAX) {
        m = &MSH.msgs[(MSH.msg_head + MSH.msg_n) % MSH_MSGS_MAX];
        MSH.msg_n++;
    } else {
        m = &MSH.msgs[MSH.msg_head];
        MSH.msg_head = (MSH.msg_head + 1) % MSH_MSGS_MAX;
    }
    memset(m, 0, sizeof *m);
    m->up = msh_now_s();
    m->epoch = msh_epoch_guess();
    m->hops = -1;
    MSH.seq_msgs++;
    MSH.dirty = true;
    return m;
}

msh_tele_t *msh_tele_of(uint32_t num, bool make)
{
    msh_node_t *n = msh_node_find(num);
    if (n && n->tele >= 0 && n->tele < MSH.n_tele && MSH.tele[n->tele].num == num) return &MSH.tele[n->tele];
    for (int i = 0; i < MSH.n_tele; i++)
        if (MSH.tele[i].num == num) {
            if (n) n->tele = (int16_t)i;
            return &MSH.tele[i];
        }
    if (!make) return NULL;
    int slot = MSH.n_tele;
    if (slot >= MSH_TELE_NODES) {
        /* full: the one with the oldest last sample goes */
        uint32_t oldest = 0xFFFFFFFFu;
        slot = 0;
        for (int i = 0; i < MSH.n_tele; i++) {
            msh_tele_t *t = &MSH.tele[i];
            uint32_t last = t->n ? t->s[(t->head + t->n - 1) % MSH_TELE_N].up : 0;
            if (last < oldest) { oldest = last; slot = i; }
        }
        msh_node_t *was = msh_node_find(MSH.tele[slot].num);
        if (was) was->tele = -1;
    } else {
        MSH.n_tele++;
    }
    memset(&MSH.tele[slot], 0, sizeof MSH.tele[slot]);
    MSH.tele[slot].num = num;
    if (n) n->tele = (int16_t)slot;
    return &MSH.tele[slot];
}

const char *msh_node_name(uint32_t num, char *buf, size_t cap)
{
    if (num == MSH_BROADCAST) { msh_scpy(buf, cap, _("Todos")); return buf; }
    msh_node_t *n = msh_node_find(num);
    if (n && n->long_name[0]) msh_scpy(buf, cap, n->long_name);
    else {
        char id[12];
        msh_node_id(num, id);
        msh_scpy(buf, cap, id);
    }
    return buf;
}

const char *msh_node_short(uint32_t num, char *buf, size_t cap)
{
    msh_node_t *n = msh_node_find(num);
    if (n && n->short_name[0]) msh_scpy(buf, cap, n->short_name);
    else snprintf(buf, cap, "%04lx", (unsigned long)(num & 0xFFFF));
    return buf;
}

const char *msh_preset_name(int p)
{
    static const char *const N[] = {
        "LongFast", "LongSlow", "VeryLongSlow", "MediumSlow", "MediumFast", "ShortSlow",
        "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo", "LiteFast", "LiteSlow",
        "NarrowFast", "NarrowSlow", "TinyFast", "TinySlow", "MediumTurbo",
    };
    return p >= 0 && p < (int)(sizeof N / sizeof N[0]) ? N[p] : "?";
}

const char *msh_region_name(int r)
{
    static const char *const N[] = {
        "", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN", "NZ_865",
        "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923", "PH_433",
        "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902", "ITU1_2M",
        "ITU2_2M", "EU_866", "EU_874", "EU_917", "EU_N_868", "ITU3_2M", "ITU1_70CM",
        "ITU2_70CM", "ITU3_70CM", "ITU2_125CM",
    };
    if (r == 0) return _("Sin región");
    return r > 0 && r < (int)(sizeof N / sizeof N[0]) ? N[r] : "?";
}

const char *msh_role_name(int r)
{
    static const char *const N[] = {
        "CLIENT", "CLIENT_MUTE", "ROUTER", "ROUTER_CLIENT", "REPEATER", "TRACKER", "SENSOR",
        "TAK", "CLIENT_HIDDEN", "LOST_AND_FOUND", "TAK_TRACKER", "ROUTER_LATE", "CLIENT_BASE",
    };
    return r >= 0 && r < (int)(sizeof N / sizeof N[0]) ? N[r] : "?";
}

const char *msh_routing_error(int e)
{
    switch (e) {
    case meshtastic_Routing_Error_NONE: return N_("bien");
    case meshtastic_Routing_Error_NO_ROUTE: return N_("no hay ruta");
    case meshtastic_Routing_Error_GOT_NAK: return N_("lo rechazaron");
    case meshtastic_Routing_Error_TIMEOUT: return N_("no hubo respuesta");
    case meshtastic_Routing_Error_NO_INTERFACE: return N_("sin radio");
    case meshtastic_Routing_Error_MAX_RETRANSMIT: return N_("nadie lo confirmó");
    case meshtastic_Routing_Error_NO_CHANNEL: return N_("canal desconocido");
    case meshtastic_Routing_Error_TOO_LARGE: return N_("demasiado largo");
    case meshtastic_Routing_Error_NO_RESPONSE: return N_("sin respuesta");
    case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT: return N_("límite de uso del canal");
    case meshtastic_Routing_Error_PKI_FAILED:
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
    case meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY: return N_("falta su clave pública");
    case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED: return N_("demasiados seguidos");
    case meshtastic_Routing_Error_NOT_AUTHORIZED: return N_("no autorizado");
    default: return N_("error");
    }
}

const char *msh_channel_name(int idx, char *buf, size_t cap)
{
    const meshtastic_Channel *c = idx >= 0 && idx < MSH_CHANNELS ? &MSH.ch[idx] : NULL;
    if (c && c->has_settings && c->settings.name[0]) msh_scpy(buf, cap, c->settings.name);
    else if (idx == 0 && MSH.have_lora && MSH.lora.use_preset) msh_scpy(buf, cap, msh_preset_name(MSH.lora.modem_preset));
    else if (idx == 0) msh_scpy(buf, cap, _("Primario"));
    else snprintf(buf, cap, _("Canal %d"), idx);
    return buf;
}

uint32_t msh_epoch_guess(void)
{
    uint32_t e = msh_epoch();
    if (e) return e;
    if (MSH.node_epoch) return MSH.node_epoch + (msh_now_s() - MSH.node_epoch_up);
    return 0;
}

static void saw_epoch(uint32_t t)
{
    /* only a plausible time (after 2024), and only forward */
    if (t > 1704067200u && t > MSH.node_epoch + (msh_now_s() - MSH.node_epoch_up)) {
        MSH.node_epoch = t;
        MSH.node_epoch_up = msh_now_s();
    }
}

int32_t msh_node_age(const msh_node_t *n)
{
    if (n->heard_up) return (int32_t)(msh_now_s() - n->heard_up);
    uint32_t now = msh_epoch_guess();
    if (now && n->last_heard && n->last_heard <= now + 60) {
        uint32_t a = now > n->last_heard ? now - n->last_heard : 0;
        return a > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)a;
    }
    return -1;
}

float msh_node_dist(const msh_node_t *n)
{
    const msh_node_t *me = msh_node_find(MSH.my_num);
    if (!me || !me->has_pos || !n->has_pos || n == me) return -1;
    return msh_distance_m(me->lat_i, me->lon_i, n->lat_i, n->lon_i);
}

void msh_log_add(const char *line)
{
    int i = (MSH.log_head + MSH.log_n) % MSH_LOG_LINES;
    if (MSH.log_n < MSH_LOG_LINES) MSH.log_n++;
    else MSH.log_head = (MSH.log_head + 1) % MSH_LOG_LINES;
    msh_scpy(MSH.log[i], sizeof MSH.log[i], line);
}

int msh_unread(uint32_t peer, int ch)
{
    int u = 0;
    for (int i = 0; i < MSH.msg_n; i++) {
        msh_msg_t *m = msh_msg_at(i);
        if (m->unread && m->peer == peer && (peer || m->ch == ch)) u++;
    }
    return u;
}

int msh_unread_all(void)
{
    int u = 0;
    for (int i = 0; i < MSH.msg_n; i++) u += msh_msg_at(i)->unread ? 1 : 0;
    return u;
}

void msh_mark_read(uint32_t peer, int ch)
{
    bool any = false;
    for (int i = 0; i < MSH.msg_n; i++) {
        msh_msg_t *m = msh_msg_at(i);
        if (m->unread && m->peer == peer && (peer || m->ch == ch)) {
            m->unread = 0;
            any = true;
        }
    }
    if (any) {
        MSH.seq_msgs++;
        MSH.dirty = true;
    }
}

/* ---- applying what comes ---- */

static void user_to_node(msh_node_t *n, const meshtastic_User *u)
{
    msh_scpy(n->long_name, sizeof n->long_name, u->long_name);
    msh_scpy(n->short_name, sizeof n->short_name, u->short_name);
    n->hw_model = u->hw_model;
    n->role = u->role;
    n->has_user = true;
    n->has_pkc = u->public_key.size == 32;
}

static void pos_to_node(msh_node_t *n, const meshtastic_Position *p)
{
    if (!p->has_latitude_i || !p->has_longitude_i || (p->latitude_i == 0 && p->longitude_i == 0)) return;
    n->has_pos = true;
    n->lat_i = p->latitude_i;
    n->lon_i = p->longitude_i;
    n->alt = p->has_altitude ? p->altitude : 0;
    n->pos_time = p->time;
    saw_epoch(p->time);
}

static void dev_to_node(msh_node_t *n, const meshtastic_DeviceMetrics *d)
{
    n->has_dev = true;
    if (d->has_battery_level) n->battery = (int16_t)d->battery_level;
    if (d->has_voltage) n->voltage = d->voltage;
    if (d->has_channel_utilization) n->ch_util = d->channel_utilization;
    if (d->has_air_util_tx) n->air_tx = d->air_util_tx;
    if (d->has_uptime_seconds) n->uptime = d->uptime_seconds;
}

static void node_info(const meshtastic_NodeInfo *ni)
{
    msh_node_t *n = msh_node_get(ni->num);
    if (!n) return;
    if (ni->has_user) user_to_node(n, &ni->user);
    if (ni->has_position) pos_to_node(n, &ni->position);
    if (ni->has_device_metrics) dev_to_node(n, &ni->device_metrics);
    n->snr = ni->snr;
    if (ni->last_heard > n->last_heard) n->last_heard = ni->last_heard;
    saw_epoch(ni->last_heard);
    n->channel = ni->channel;
    n->via_mqtt = ni->via_mqtt;
    n->hops = ni->has_hops_away ? (int8_t)ni->hops_away : (ni->num == MSH.my_num ? 0 : -1);
    n->favorite = ni->is_favorite;
    MSH.seq_nodes++;
    MSH.dirty = true;
}

static void tele_sample(uint32_t num, const meshtastic_Telemetry *t)
{
    msh_node_t *n = msh_node_get(num);
    if (!n) return;
    msh_tele_t *h = msh_tele_of(num, true);
    msh_tsample_t s = { 0 };
    s.up = msh_now_s();
    s.epoch = t->time;
    saw_epoch(t->time);
    if (t->which_variant == meshtastic_Telemetry_device_metrics_tag) {
        const meshtastic_DeviceMetrics *d = &t->variant.device_metrics;
        dev_to_node(n, d);
        if (d->has_battery_level) { s.has |= MSH_T_BATT; s.batt = (uint8_t)(d->battery_level > 255 ? 255 : d->battery_level); }
        if (d->has_voltage) { s.has |= MSH_T_VOLT; s.volt = d->voltage; }
        if (d->has_channel_utilization) { s.has |= MSH_T_CHU; s.chu = d->channel_utilization; }
        if (d->has_air_util_tx) { s.has |= MSH_T_AIR; s.air = d->air_util_tx; }
    } else if (t->which_variant == meshtastic_Telemetry_environment_metrics_tag) {
        const meshtastic_EnvironmentMetrics *e = &t->variant.environment_metrics;
        n->has_env = true;
        if (e->has_temperature) { s.has |= MSH_T_TEMP; s.temp = n->temp = e->temperature; }
        if (e->has_relative_humidity) { s.has |= MSH_T_HUM; s.hum = n->hum = e->relative_humidity; }
        if (e->has_barometric_pressure) { s.has |= MSH_T_PRESS; s.press = n->press = e->barometric_pressure; }
    }
    if (h && s.has) {
        int i = (h->head + h->n) % MSH_TELE_N;
        if (h->n < MSH_TELE_N) h->n++;
        else h->head = (h->head + 1) % MSH_TELE_N;
        h->s[i] = s;
    }
    MSH.seq_nodes++;
    MSH.dirty = true;
}

static void routing(const meshtastic_MeshPacket *p, const meshtastic_Routing *r)
{
    uint32_t rid = p->decoded.request_id;
    if (MSH.trace.state == 1 && rid == MSH.trace.req_id && r->which_variant == meshtastic_Routing_error_reason_tag
        && r->error_reason != meshtastic_Routing_Error_NONE) {
        MSH.trace.state = 3;
        MSH.seq_trace++;
    }
    msh_msg_t *m = msh_msg_by_id(rid);
    if (!m || m->state == MSH_MSG_RX || r->which_variant != meshtastic_Routing_error_reason_tag) return;
    if (r->error_reason == meshtastic_Routing_Error_NONE) {
        if (p->from == m->to || m->to == MSH_BROADCAST) m->state = MSH_MSG_ACKED;
        else if (m->state < MSH_MSG_SENT) m->state = MSH_MSG_SENT;   /* a relay heard it: on its way */
    } else if (m->state != MSH_MSG_ACKED) {
        m->state = MSH_MSG_FAILED;
        m->err = (uint8_t)r->error_reason;
    }
    MSH.seq_msgs++;
    MSH.dirty = true;
}

static void traceroute(const meshtastic_MeshPacket *p, const meshtastic_RouteDiscovery *rd)
{
    msh_trace_t *t = &MSH.trace;
    if (t->state != 1) return;
    if (p->decoded.request_id != t->req_id && p->from != t->target) return;
    t->n_to = rd->route_count > 8 ? 8 : (int)rd->route_count;
    for (int i = 0; i < t->n_to; i++) t->to[i] = rd->route[i];
    for (int i = 0; i < 9; i++) t->snr_to[i] = i < (int)rd->snr_towards_count ? rd->snr_towards[i] : INT8_MIN;
    t->n_back = rd->route_back_count > 8 ? 8 : (int)rd->route_back_count;
    for (int i = 0; i < t->n_back; i++) t->back[i] = rd->route_back[i];
    for (int i = 0; i < 9; i++) t->snr_back[i] = i < (int)rd->snr_back_count ? rd->snr_back[i] : INT8_MIN;
    t->state = 2;
    MSH.seq_trace++;
}

static void text_message(const meshtastic_MeshPacket *p)
{
    if (p->from == MSH.my_num && msh_msg_by_id(p->id)) return;      /* our own, echoed */
    if (msh_msg_by_id(p->id) && p->id) return;                      /* heard twice */
    msh_msg_t *m = msh_msg_add();
    m->id = p->id;
    m->from = p->from;
    m->to = p->to;
    m->ch = p->channel;
    m->peer = p->to == MSH_BROADCAST ? 0 : (p->from == MSH.my_num ? p->to : p->from);
    m->state = MSH_MSG_RX;
    m->unread = p->from != MSH.my_num;
    if (p->has_rx_time && p->rx_time) {
        m->epoch = p->rx_time;
        saw_epoch(p->rx_time);
    }
    m->snr = (int8_t)(p->rx_snr < -128 ? -128 : p->rx_snr > 127 ? 127 : p->rx_snr);
    m->rssi = p->has_rx_rssi ? (int16_t)p->rx_rssi : 0;
    m->hops = p->hop_start ? (int8_t)(p->hop_start - p->hop_limit) : -1;
    m->reply_id = p->decoded.reply_id;
    m->reaction = p->decoded.emoji != 0;
    int len = p->decoded.payload.size;
    if (len > MSH_TEXT_MAX - 1) len = MSH_TEXT_MAX - 1;
    memcpy(m->text, p->decoded.payload.bytes, (size_t)len);
    m->text[len] = 0;
    if (m->unread) MSH.rx_count++;
}

static void packet(const meshtastic_MeshPacket *p)
{
    MSH.pkts_in++;
    if (p->from && p->from != MSH.my_num) {
        msh_node_t *n = msh_node_get(p->from);
        if (n) {
            n->heard_up = msh_now_s();
            if (p->has_rx_time && p->rx_time) {
                n->last_heard = p->rx_time;
                saw_epoch(p->rx_time);
            }
            if (p->rx_snr != 0 || p->has_rx_rssi) {
                n->snr = p->rx_snr;
                n->has_rssi = p->has_rx_rssi;
                n->rssi = (int16_t)p->rx_rssi;
            }
            if (p->hop_start) n->hops = (int8_t)(p->hop_start - p->hop_limit);
            n->channel = p->channel;
            n->via_mqtt = p->via_mqtt;
            MSH.seq_nodes++;
        }
    }
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag) return;     /* not for us to read */
    const meshtastic_Data *d = &p->decoded;
    switch (d->portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        text_message(p);
        break;
    case meshtastic_PortNum_POSITION_APP: {
        meshtastic_Position pos = meshtastic_Position_init_zero;
        pb_istream_t s = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
        if (pb_decode(&s, meshtastic_Position_fields, &pos)) {
            msh_node_t *n = msh_node_get(p->from);
            if (n) pos_to_node(n, &pos);
            MSH.seq_nodes++;
            MSH.dirty = true;
        }
        break;
    }
    case meshtastic_PortNum_NODEINFO_APP: {
        meshtastic_User u = meshtastic_User_init_zero;
        pb_istream_t s = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
        if (pb_decode(&s, meshtastic_User_fields, &u)) {
            msh_node_t *n = msh_node_get(p->from);
            if (n) user_to_node(n, &u);
            MSH.seq_nodes++;
            MSH.dirty = true;
        }
        break;
    }
    case meshtastic_PortNum_TELEMETRY_APP: {
        meshtastic_Telemetry *t = (meshtastic_Telemetry *)msh_alloc(sizeof *t);
        if (!t) break;
        pb_istream_t s = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
        if (pb_decode(&s, meshtastic_Telemetry_fields, t)) tele_sample(p->from, t);
        free(t);
        break;
    }
    case meshtastic_PortNum_ROUTING_APP: {
        meshtastic_Routing r = meshtastic_Routing_init_zero;
        pb_istream_t s = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
        if (pb_decode(&s, meshtastic_Routing_fields, &r)) routing(p, &r);
        break;
    }
    case meshtastic_PortNum_TRACEROUTE_APP: {
        meshtastic_RouteDiscovery rd = meshtastic_RouteDiscovery_init_zero;
        pb_istream_t s = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
        if (pb_decode(&s, meshtastic_RouteDiscovery_fields, &rd)) traceroute(p, &rd);
        break;
    }
    default:
        break;
    }
}

void msh_model_apply(const meshtastic_FromRadio *fr)
{
    switch (fr->which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        if (fr->my_info.my_node_num != MSH.my_num) {
            msh_store_save_locked(); /* what belonged to the node before, if any */
            MSH.my_num = fr->my_info.my_node_num;
            msh_store_load(MSH.my_num);
        }
        MSH.nodedb_count = fr->my_info.nodedb_count;
        MSH.seq_cfg++;
        break;
    case meshtastic_FromRadio_metadata_tag:
        msh_scpy(MSH.fw, sizeof MSH.fw, fr->metadata.firmware_version);
        MSH.hw_model = fr->metadata.hw_model;
        MSH.role = fr->metadata.role;
        MSH.has_wifi = fr->metadata.hasWifi;
        MSH.has_bt = fr->metadata.hasBluetooth;
        MSH.has_pkc = fr->metadata.hasPKC;
        MSH.seq_cfg++;
        break;
    case meshtastic_FromRadio_node_info_tag:
        node_info(&fr->node_info);
        break;
    case meshtastic_FromRadio_config_tag:
        if (fr->config.which_payload_variant == meshtastic_Config_lora_tag) {
            MSH.lora = fr->config.payload_variant.lora;
            MSH.have_lora = true;
        } else if (fr->config.which_payload_variant == meshtastic_Config_device_tag) {
            MSH.dev = fr->config.payload_variant.device;
            MSH.have_dev = true;
        } else if (fr->config.which_payload_variant == meshtastic_Config_bluetooth_tag) {
            MSH.bt_cfg = fr->config.payload_variant.bluetooth;
            MSH.have_bt_cfg = true;
        }
        MSH.seq_cfg++;
        break;
    case meshtastic_FromRadio_channel_tag:
        if (fr->channel.index >= 0 && fr->channel.index < MSH_CHANNELS) MSH.ch[fr->channel.index] = fr->channel;
        MSH.seq_cfg++;
        break;
    case meshtastic_FromRadio_queueStatus_tag: {
        msh_msg_t *m = msh_msg_by_id(fr->queueStatus.mesh_packet_id);
        if (m && m->state == MSH_MSG_QUEUED) {
            if (fr->queueStatus.res == 0) m->state = MSH_MSG_SENT;
            else {
                m->state = MSH_MSG_FAILED;
                m->err = meshtastic_Routing_Error_NO_INTERFACE;
            }
            MSH.seq_msgs++;
        }
        break;
    }
    case meshtastic_FromRadio_packet_tag:
        packet(&fr->packet);
        break;
    case meshtastic_FromRadio_log_record_tag:
        msh_log_add(fr->log_record.message);
        break;
    case meshtastic_FromRadio_clientNotification_tag:
        msh_scpy(MSH.notice, sizeof MSH.notice, fr->clientNotification.message);
        MSH.seq_notice++;
        msh_log_add(fr->clientNotification.message);
        break;
    default:
        break;
    }
}
