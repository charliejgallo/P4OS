/*
 * Malla - the client session: Meshtastic's API over whatever link.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * One thread of the app's own (aos_hal_thread_start, its stack in PSRAM):
 *
 *   1. open the link (msh_link_t: serial, TCP or BLE);
 *   2. write ToRadio{want_config_id = a random nonce}; the node answers with
 *      my_info, metadata, every node_info, its config and module config,
 *      its channels, and config_complete_id with the same nonce. Anything
 *      with another nonce belongs to an older request and is still applied
 *      (it is the same database), but only ours makes the session READY;
 *   3. from then on apply every FromRadio as it comes, write what the UI
 *      queued, and send a heartbeat every minute so a serial node knows a
 *      client is still there;
 *   4. when the link drops, close it and try again: 2 s, then 5, 10, 20 and
 *      every 30 s, until the app stops the session. A node that restarts
 *      (a new LoRa setting does that) comes back the same way.
 *
 * What the UI sends is encoded here, on the caller's thread, into a ring of
 * ready ToRadio frames; the session writes them in order. A text message is
 * in the model as QUEUED the moment it is accepted, and the node's
 * QueueStatus and Routing answers move it on (msh_model.c).
 *
 * Admin messages go to our own node (to = my_node_num). From the client API
 * the node takes them without a session key: that key is for admin over
 * the mesh.
 */
#include "msh.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include "pb_decode.h"
#include "pb_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Q_MAX         24
#define HEARTBEAT_S   60
#define SAVE_S        60
#define TRACE_WAIT_S  90

typedef struct {
    uint16_t len;
    uint8_t  b[MSH_FRAME_MAX];
} frame_t;

static struct {
    volatile bool running, stop;
    void *qmx;
    frame_t *q;                     /* Q_MAX, in PSRAM */
    int qhead, qn;
    msh_link_t *volatile link;
    meshtastic_FromRadio *fr;       /* decoded into, by the thread only */
    uint8_t rx[MSH_FRAME_MAX];
    uint32_t nonce;
} S;

/* ---- the queue ---- */

static bool q_push(const uint8_t *b, int len)
{
    if (!S.qmx || !S.q || len <= 0 || len > MSH_FRAME_MAX) return false;
    aos_hal_mutex_lock(S.qmx);
    bool ok = S.qn < Q_MAX;
    if (ok) {
        frame_t *f = &S.q[(S.qhead + S.qn) % Q_MAX];
        f->len = (uint16_t)len;
        memcpy(f->b, b, (size_t)len);
        S.qn++;
    }
    aos_hal_mutex_unlock(S.qmx);
    return ok;
}

static bool q_pop(frame_t *out)
{
    aos_hal_mutex_lock(S.qmx);
    bool ok = S.qn > 0;
    if (ok) {
        *out = S.q[S.qhead];
        S.qhead = (S.qhead + 1) % Q_MAX;
        S.qn--;
    }
    aos_hal_mutex_unlock(S.qmx);
    return ok;
}

static bool encode_to_radio(const meshtastic_ToRadio *tr)
{
    uint8_t buf[MSH_FRAME_MAX];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);
    if (!pb_encode(&os, meshtastic_ToRadio_fields, tr)) {
        aos_hal_log(MSH_TAG, "encode: %s", PB_GET_ERROR(&os));
        return false;
    }
    return q_push(buf, (int)os.bytes_written);
}

/* ---- the thread ---- */

static void set_state(msh_state_t st)
{
    msh_lock();
    if (MSH.state != st) {
        MSH.state = st;
        MSH.seq_state++;
        if (st == MSH_ST_READY) MSH.ready_up = msh_now_s();
    }
    msh_unlock();
}

static void set_err(const char *e)
{
    msh_lock();
    msh_scpy(MSH.err, sizeof MSH.err, e ? e : "");
    MSH.seq_state++;
    msh_unlock();
}

static void on_log(void *ctx, const char *line)
{
    (void)ctx;
    msh_lock();
    msh_log_add(line);
    msh_unlock();
}

/* What the link asks of the person, copied for the UI the moment it changes. */
static void on_changed(msh_link_t *l)
{
    msh_lock();
    MSH.ask = l->ask;
    MSH.ask_num = l->num;
    MSH.note = l->note;
    MSH.seq_state++;
    msh_unlock();
}

static bool send_now(msh_link_t *l, const meshtastic_ToRadio *tr)
{
    uint8_t buf[MSH_FRAME_MAX];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);
    if (!pb_encode(&os, meshtastic_ToRadio_fields, tr)) return false;
    msh_lock();
    MSH.pkts_out++;
    msh_unlock();
    return l->ops->send(l, buf, (int)os.bytes_written);
}

static void want_config(msh_link_t *l)
{
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    S.nonce = msh_random();
    tr.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    tr.want_config_id = S.nonce;
    send_now(l, &tr);
}

/* Sleeps ms in steps, returning early when the session is told to stop. */
static void nap(int ms)
{
    for (int t = 0; t < ms && !S.stop; t += 50) aos_hal_sleep_ms(50);
}

/* Talks over an open link until it drops or the session stops. */
static void talk(msh_link_t *l)
{
    want_config(l);
    set_state(MSH_ST_CONFIG);
    uint32_t last_hb = msh_now_s(), last_save = msh_now_s();
    char err[96] = "";
    while (!S.stop) {
        int n = l->ops->recv(l, S.rx, sizeof S.rx, 40, err, sizeof err);
        if (n < 0) {
            set_err(err);
            return;
        }
        if (n > 0) {
            memset(S.fr, 0, sizeof *S.fr);
            pb_istream_t is = pb_istream_from_buffer(S.rx, (size_t)n);
            if (pb_decode(&is, meshtastic_FromRadio_fields, S.fr)) {
                bool complete = S.fr->which_payload_variant == meshtastic_FromRadio_config_complete_id_tag
                             && S.fr->config_complete_id == S.nonce;
                bool rebooted = S.fr->which_payload_variant == meshtastic_FromRadio_rebooted_tag;
                msh_lock();
                msh_model_apply(S.fr);
                msh_unlock();
                if (complete) {
                    set_state(MSH_ST_READY);
                    set_err("");
                    aos_hal_log(MSH_TAG, "ready: node %08lx, %d nodes", (unsigned long)MSH.my_num, MSH.n_nodes);
                }
                if (rebooted) {
                    aos_hal_log(MSH_TAG, "the node restarted: asking again");
                    want_config(l);
                    set_state(MSH_ST_CONFIG);
                }
            } else {
                aos_hal_log(MSH_TAG, "FromRadio of %d bytes: %s", n, PB_GET_ERROR(&is));
            }
        }
        /* what the UI queued, once the node has told us who it is */
        if (MSH.state == MSH_ST_READY) {
            frame_t f;
            while (q_pop(&f)) {
                if (!l->ops->send(l, f.b, f.len)) {
                    set_err(N_("No se pudo escribir al nodo"));
                    return;
                }
                msh_lock();
                MSH.pkts_out++;
                msh_unlock();
            }
        }
        uint32_t now = msh_now_s();
        if (now - last_hb >= HEARTBEAT_S) {
            last_hb = now;
            meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
            tr.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
            tr.heartbeat.nonce = msh_random();
            if (!send_now(l, &tr)) {
                set_err(N_("No se pudo escribir al nodo"));
                return;
            }
        }
        if (now - last_save >= SAVE_S) {
            last_save = now;
            msh_store_save();
        }
        msh_lock();
        if (MSH.trace.state == 1 && now - MSH.trace.t0 > TRACE_WAIT_S) {
            MSH.trace.state = 3;
            MSH.seq_trace++;
        }
        msh_unlock();
    }
    /* stopping: say goodbye on a stream, so a serial node drops the API mode */
    if (l->t.kind != MSH_LINK_BLE) {
        meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
        tr.which_payload_variant = meshtastic_ToRadio_disconnect_tag;
        tr.disconnect = true;
        send_now(l, &tr);
    }
}

static void sess_thread(void *arg)
{
    (void)arg;
    static const int BACKOFF[] = { 2000, 5000, 10000, 20000, 30000 };
    int tries = 0;
    while (!S.stop) {
        set_state(MSH_ST_OPENING);
        msh_link_t *l = msh_link_new(&MSH.target);
        if (!l) {
            set_err(N_("Sin memoria"));
            break;
        }
        l->log_line = on_log;
        l->changed = on_changed;
        S.link = l;
        char err[96] = "";
        if (l->ops->open(l, err, sizeof err)) {
            msh_lock();
            msh_log_add("-- connected --");
            msh_unlock();
            talk(l);
            if (MSH.state == MSH_ST_READY) tries = 0;
        } else {
            set_err(err);
        }
        S.link = NULL;
        msh_link_free(l);
        msh_lock();
        MSH.ask = MSH_ASK_NONE;
        MSH.note = NULL;
        msh_unlock();
        if (S.stop) break;
        set_state(MSH_ST_RETRY);
        aos_hal_log(MSH_TAG, "link down (%s), again in %d s", MSH.err, BACKOFF[tries] / 1000);
        nap(BACKOFF[tries]);
        if (tries < (int)(sizeof BACKOFF / sizeof BACKOFF[0]) - 1) tries++;
    }
    msh_store_save();
    set_state(MSH_ST_IDLE);
    S.running = false;
}

void msh_sess_start(const msh_target_t *t)
{
    msh_sess_stop();
    if (!S.qmx) S.qmx = aos_hal_mutex_create();
    if (!S.q) S.q = (frame_t *)msh_alloc(sizeof(frame_t) * Q_MAX);
    if (!S.fr) S.fr = (meshtastic_FromRadio *)msh_alloc(sizeof *S.fr);
    if (!S.q || !S.fr || !S.qmx) return;
    msh_lock();
    MSH.target = *t;
    MSH.err[0] = 0;
    MSH.seq_state++;
    msh_unlock();
    S.stop = false;
    S.running = true;
    if (!aos_hal_thread_start("malla", sess_thread, NULL, 12288, 4)) {
        S.running = false;
        set_err(N_("No se pudo empezar la conexión"));
    }
}

void msh_sess_stop(void)
{
    if (!S.running) return;
    S.stop = true;
    msh_link_t *l = S.link;
    if (l) l->cancel = true;
    /* a BLE open can take its 45 s; everything else answers within a second */
    for (int i = 0; i < 1200 && S.running; i++) aos_hal_sleep_ms(50);
    aos_hal_mutex_lock(S.qmx);
    S.qn = 0;
    aos_hal_mutex_unlock(S.qmx);
}

bool msh_sess_running(void) { return S.running; }

void msh_sess_passkey(uint32_t pin)
{
    msh_link_t *l = S.link;
    if (!l) return;
    l->pin = pin;
    l->answered = true;
}

/* ---- what goes out ---- */

static meshtastic_ToRadio *new_packet(uint32_t to, int ch, meshtastic_PortNum port)
{
    meshtastic_ToRadio *tr = (meshtastic_ToRadio *)msh_alloc(sizeof *tr);
    if (!tr) return NULL;
    tr->which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *p = &tr->packet;
    p->to = to;
    p->channel = (uint8_t)(ch < 0 ? 0 : ch);
    p->id = msh_random();
    p->hop_limit = (uint8_t)(MSH.have_lora && MSH.lora.hop_limit ? MSH.lora.hop_limit : 3);
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = port;
    return tr;
}

static bool payload_of(meshtastic_Data *d, const pb_msgdesc_t *fields, const void *msg)
{
    pb_ostream_t os = pb_ostream_from_buffer(d->payload.bytes, sizeof d->payload.bytes);
    if (!pb_encode(&os, fields, msg)) {
        aos_hal_log(MSH_TAG, "payload: %s", PB_GET_ERROR(&os));
        return false;
    }
    d->payload.size = (pb_size_t)os.bytes_written;
    return true;
}

static int channel_of(uint32_t num)
{
    msh_lock();
    msh_node_t *n = msh_node_find(num);
    int ch = n ? n->channel : 0;
    msh_unlock();
    return ch;
}

bool msh_send_text(uint32_t to, int ch, const char *text, uint32_t reply_id, bool reaction)
{
    size_t len = strlen(text);
    if (!len) return false;
    if (len > MSH_TEXT_MAX - 1) len = MSH_TEXT_MAX - 1;
    if (to != MSH_BROADCAST) ch = channel_of(to);
    meshtastic_ToRadio *tr = new_packet(to, ch, meshtastic_PortNum_TEXT_MESSAGE_APP);
    if (!tr) return false;
    meshtastic_MeshPacket *p = &tr->packet;
    p->want_ack = true;
    memcpy(p->decoded.payload.bytes, text, len);
    p->decoded.payload.size = (pb_size_t)len;
    p->decoded.reply_id = reply_id;
    if (reaction) p->decoded.emoji = 1;
    bool ok = encode_to_radio(tr);
    if (ok) {
        msh_lock();
        msh_msg_t *m = msh_msg_add();
        m->id = p->id;
        m->from = MSH.my_num;
        m->to = to;
        m->peer = to == MSH_BROADCAST ? 0 : to;
        m->ch = (uint8_t)ch;
        m->state = MSH_MSG_QUEUED;
        m->reply_id = reply_id;
        m->reaction = reaction;
        memcpy(m->text, text, len);
        m->text[len] = 0;
        msh_unlock();
    }
    free(tr);
    return ok;
}

bool msh_send_traceroute(uint32_t to)
{
    meshtastic_ToRadio *tr = new_packet(to, channel_of(to), meshtastic_PortNum_TRACEROUTE_APP);
    if (!tr) return false;
    tr->packet.decoded.want_response = true;
    meshtastic_RouteDiscovery rd = meshtastic_RouteDiscovery_init_zero;
    bool ok = payload_of(&tr->packet.decoded, meshtastic_RouteDiscovery_fields, &rd) && encode_to_radio(tr);
    if (ok) {
        msh_lock();
        memset(&MSH.trace, 0, sizeof MSH.trace);
        MSH.trace.target = to;
        MSH.trace.req_id = tr->packet.id;
        MSH.trace.state = 1;
        MSH.trace.t0 = msh_now_s();
        MSH.seq_trace++;
        msh_unlock();
    }
    free(tr);
    return ok;
}

bool msh_send_telemetry_req(uint32_t to)
{
    meshtastic_ToRadio *tr = new_packet(to, channel_of(to), meshtastic_PortNum_TELEMETRY_APP);
    if (!tr) return false;
    tr->packet.decoded.want_response = true;
    meshtastic_Telemetry *t = (meshtastic_Telemetry *)msh_alloc(sizeof *t);
    bool ok = false;
    if (t) {
        t->which_variant = meshtastic_Telemetry_device_metrics_tag;
        ok = payload_of(&tr->packet.decoded, meshtastic_Telemetry_fields, t) && encode_to_radio(tr);
        free(t);
    }
    free(tr);
    return ok;
}

bool msh_send_position_req(uint32_t to)
{
    meshtastic_ToRadio *tr = new_packet(to, channel_of(to), meshtastic_PortNum_POSITION_APP);
    if (!tr) return false;
    tr->packet.decoded.want_response = true;
    meshtastic_Position pos = meshtastic_Position_init_zero;
    bool ok = payload_of(&tr->packet.decoded, meshtastic_Position_fields, &pos) && encode_to_radio(tr);
    free(tr);
    return ok;
}

static bool admin_send(const meshtastic_AdminMessage *a)
{
    meshtastic_ToRadio *tr = new_packet(MSH.my_num, 0, meshtastic_PortNum_ADMIN_APP);
    if (!tr) return false;
    tr->packet.decoded.want_response = true;
    tr->packet.hop_limit = 0;
    bool ok = MSH.my_num && payload_of(&tr->packet.decoded, meshtastic_AdminMessage_fields, a) && encode_to_radio(tr);
    free(tr);
    return ok;
}

static meshtastic_AdminMessage *new_admin(pb_size_t which)
{
    meshtastic_AdminMessage *a = (meshtastic_AdminMessage *)msh_alloc(sizeof *a);
    if (a) a->which_payload_variant = which;
    return a;
}

static bool admin_flag(pb_size_t which)
{
    meshtastic_AdminMessage *a = new_admin(which);
    if (!a) return false;
    if (which == meshtastic_AdminMessage_begin_edit_settings_tag) a->begin_edit_settings = true;
    if (which == meshtastic_AdminMessage_commit_edit_settings_tag) a->commit_edit_settings = true;
    bool ok = admin_send(a);
    free(a);
    return ok;
}

bool msh_set_owner(const char *long_name, const char *short_name)
{
    meshtastic_AdminMessage *a = new_admin(meshtastic_AdminMessage_set_owner_tag);
    if (!a) return false;
    msh_lock();
    msh_node_t *me = msh_node_find(MSH.my_num);
    if (me) {
        a->set_owner.hw_model = (meshtastic_HardwareModel)me->hw_model;
        a->set_owner.role = (meshtastic_Config_DeviceConfig_Role)me->role;
    }
    msh_unlock();
    /* the firmware keeps 24 bytes of the long name and 4 of the short one */
    char ln[25], sn[5];
    msh_scpy(ln, sizeof ln, long_name);
    msh_scpy(sn, sizeof sn, short_name);
    msh_scpy(a->set_owner.long_name, sizeof a->set_owner.long_name, ln);
    msh_scpy(a->set_owner.short_name, sizeof a->set_owner.short_name, sn);
    bool ok = admin_send(a);
    free(a);
    if (ok) {
        msh_lock();
        msh_node_t *n = msh_node_get(MSH.my_num);
        if (n) {
            msh_scpy(n->long_name, sizeof n->long_name, ln);
            msh_scpy(n->short_name, sizeof n->short_name, sn);
            MSH.seq_nodes++;
        }
        msh_unlock();
    }
    return ok;
}

bool msh_set_lora(const meshtastic_Config_LoRaConfig *lora)
{
    meshtastic_AdminMessage *a = new_admin(meshtastic_AdminMessage_set_config_tag);
    if (!a) return false;
    a->set_config.which_payload_variant = meshtastic_Config_lora_tag;
    a->set_config.payload_variant.lora = *lora;
    bool ok = admin_send(a);
    free(a);
    if (ok) {
        msh_lock();
        MSH.lora = *lora;
        MSH.seq_cfg++;
        msh_unlock();
    }
    return ok;
}

bool msh_set_channel(const meshtastic_Channel *c)
{
    meshtastic_AdminMessage *a = new_admin(meshtastic_AdminMessage_set_channel_tag);
    if (!a) return false;
    a->set_channel = *c;
    bool ok = admin_send(a);
    free(a);
    if (ok && c->index >= 0 && c->index < MSH_CHANNELS) {
        msh_lock();
        MSH.ch[c->index] = *c;
        MSH.seq_cfg++;
        msh_unlock();
    }
    return ok;
}

bool msh_set_favorite(uint32_t num, bool on)
{
    meshtastic_AdminMessage *a = new_admin(on ? meshtastic_AdminMessage_set_favorite_node_tag
                                              : meshtastic_AdminMessage_remove_favorite_node_tag);
    if (!a) return false;
    if (on) a->set_favorite_node = num;
    else a->remove_favorite_node = num;
    bool ok = admin_send(a);
    free(a);
    if (ok) {
        msh_lock();
        msh_node_t *n = msh_node_find(num);
        if (n) n->favorite = on;
        MSH.seq_nodes++;
        MSH.dirty = true;
        msh_unlock();
    }
    return ok;
}

bool msh_apply_channel_set(const meshtastic_ChannelSet *cs)
{
    if (!cs->settings_count) return false;
    bool ok = admin_flag(meshtastic_AdminMessage_begin_edit_settings_tag);
    for (int i = 0; ok && i < MSH_CHANNELS; i++) {
        meshtastic_Channel c = meshtastic_Channel_init_zero;
        c.index = (int8_t)i;
        if (i < (int)cs->settings_count) {
            c.has_settings = true;
            c.settings = cs->settings[i];
            c.role = i == 0 ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
        } else {
            c.role = meshtastic_Channel_Role_DISABLED;
        }
        ok = msh_set_channel(&c);
    }
    if (ok && cs->has_lora_config) {
        meshtastic_Config_LoRaConfig lora;
        msh_lock();
        lora = MSH.lora;
        msh_unlock();
        /* the URL brings the radio settings of the mesh; the region is ours
         * unless it says one */
        const meshtastic_Config_LoRaConfig *in = &cs->lora_config;
        lora.use_preset = in->use_preset;
        lora.modem_preset = in->modem_preset;
        lora.bandwidth = in->bandwidth;
        lora.spread_factor = in->spread_factor;
        lora.coding_rate = in->coding_rate;
        lora.channel_num = in->channel_num;
        if (in->hop_limit) lora.hop_limit = in->hop_limit;
        if (in->region) lora.region = in->region;
        ok = msh_set_lora(&lora);
    }
    if (ok) ok = admin_flag(meshtastic_AdminMessage_commit_edit_settings_tag);
    return ok;
}
