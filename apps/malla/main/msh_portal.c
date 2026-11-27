/*
 * Malla - the portal's page: chatting from the phone's browser.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Through the live channel (aos_hal_live_*, docs/PORTAL-PAGES.md "Live
 * data"), only while a page is looking: three JSON values the page reads,
 *
 *   state   the link, our node, the channels
 *   nodes   every node, short
 *   msgs    the newest messages of every conversation
 *
 * each put again when its part of the model changed (and every 10 s, so the
 * ages move), and the page's lines taken from the app's timer:
 *
 *   send <peer> <ch> <text>   a message; peer 0 is the channel ch
 *   read <peer> <ch>          the conversation was read there
 *   url <link>                take a channel set (the page asks first)
 */
#include "msh_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSGS_OUT   240
#define BUF_MAX    (60 * 1024)

static struct {
    char *buf;
    size_t len;
    bool over;
    uint32_t put_nodes, put_msgs, put_state, put_cfg;
    uint32_t last_put;
} P;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void out(const char *fmt, ...)
{
    if (P.over) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(P.buf + P.len, BUF_MAX - P.len, fmt, ap);
    va_end(ap);
    if (n < 0 || P.len + (size_t)n >= BUF_MAX - 64) { P.over = true; return; }
    P.len += (size_t)n;
}

static void str(const char *s)
{
    if (P.over) return;
    if (P.len + strlen(s) * 2 + 4 >= BUF_MAX - 64) { P.over = true; return; }
    P.buf[P.len++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { P.buf[P.len++] = '\\'; P.buf[P.len++] = (char)*p; }
        else if (*p == '\n') { P.buf[P.len++] = '\\'; P.buf[P.len++] = 'n'; }
        else if (*p < 0x20) P.buf[P.len++] = ' ';
        else P.buf[P.len++] = (char)*p;
    }
    P.buf[P.len++] = '"';
}

static void begin(void)
{
    P.len = 0;
    P.over = false;
}

static void put(const char *key)
{
    if (P.over) aos_hal_log(MSH_TAG, "portal: %s cut short", key);
    aos_hal_live_put(MSH_APP_ID, key, "application/json", P.buf, P.len);
}

static void put_state(void)
{
    begin();
    char t[96];
    msh_target_text(&MSH.target, t, sizeof t);
    static const char *const ST[] = { "idle", "opening", "config", "ready", "retry" };
    char id[12];
    msh_node_id(MSH.my_num, id);
    char name[48];
    msh_node_name(MSH.my_num, name, sizeof name);
    out("{\"v\":1,\"state\":\"%s\",\"link\":", ST[MSH.state]);
    str(t);
    out(",\"err\":");
    str(MSH.err[0] ? _(MSH.err) : "");
    out(",\"my\":%lu,\"my_id\":\"%s\",\"my_name\":", (unsigned long)MSH.my_num, MSH.my_num ? id : "");
    str(MSH.my_num ? name : "");
    out(",\"fw\":");
    str(MSH.fw);
    out(",\"chans\":[");
    bool first = true;
    for (int i = 0; i < MSH_CHANNELS; i++) {
        if (MSH.ch[i].role == meshtastic_Channel_Role_DISABLED && i) continue;
        char cn[24];
        msh_channel_name(i, cn, sizeof cn);
        out("%s{\"i\":%d,\"name\":", first ? "" : ",", i);
        str(cn);
        out(",\"unread\":%d}", msh_unread(0, i));
        first = false;
    }
    out("]}");
    put("state");
}

static void put_nodes(void)
{
    begin();
    out("[");
    for (int i = 0; i < MSH.n_nodes; i++) {
        const msh_node_t *n = &MSH.nodes[i];
        out("%s{\"n\":%lu,\"l\":", i ? "," : "", (unsigned long)n->num);
        str(n->long_name);
        out(",\"s\":");
        str(n->short_name);
        out(",\"age\":%ld,\"hops\":%d,\"bat\":%d,\"snr\":%d,\"pkc\":%d", (long)msh_node_age(n), n->hops, n->battery,
            (int)(n->snr * 4), n->has_pkc ? 1 : 0);
        if (n->has_pos) out(",\"lat\":%ld,\"lon\":%ld", (long)n->lat_i, (long)n->lon_i);
        float d = msh_node_dist(n);
        if (d >= 0) out(",\"dist\":%ld", (long)d);
        out(",\"u\":%d}", n->num != MSH.my_num ? msh_unread(n->num, 0) : 0);
    }
    out("]");
    put("nodes");
}

static void put_msgs(void)
{
    begin();
    out("[");
    int from = MSH.msg_n > MSGS_OUT ? MSH.msg_n - MSGS_OUT : 0;
    for (int i = from; i < MSH.msg_n; i++) {
        const msh_msg_t *m = msh_msg_at(i);
        out("%s{\"id\":%lu,\"f\":%lu,\"t\":%lu,\"p\":%lu,\"c\":%d,\"st\":%d,\"e\":%d,\"w\":%lu,\"age\":%ld,\"r\":%d,\"rid\":%lu,\"txt\":",
            i > from ? "," : "", (unsigned long)m->id, (unsigned long)m->from, (unsigned long)m->to,
            (unsigned long)m->peer, m->ch, m->state, m->err, (unsigned long)m->epoch,
            m->up ? (long)(msh_now_s() - m->up) : -1L, m->reaction, (unsigned long)m->reply_id);
        str(m->text);
        out("}");
    }
    out("]");
    put("msgs");
}

static void command(char *line)
{
    if (!strncmp(line, "send ", 5)) {
        char *p = line + 5, *e;
        uint32_t peer = (uint32_t)strtoul(p, &e, 10);
        int ch = (int)strtol(e, &e, 10);
        if (*e == ' ') e++;
        if (*e) msh_send_text(peer ? peer : MSH_BROADCAST, ch, e, 0, false);
    } else if (!strncmp(line, "read ", 5)) {
        char *e;
        uint32_t peer = (uint32_t)strtoul(line + 5, &e, 10);
        int ch = (int)strtol(e, &e, 10);
        msh_lock();
        msh_mark_read(peer, ch);
        msh_unlock();
    } else if (!strncmp(line, "url ", 4)) {
        meshtastic_ChannelSet *cs = (meshtastic_ChannelSet *)msh_alloc(sizeof *cs);
        if (cs && msh_url_parse(line + 4, cs) && msh_apply_channel_set(cs))
            aos_ui_toast(_("Canales cambiados desde el portal: el nodo se reinicia"), 2500);
        free(cs);
    }
}

void msh_portal_tick(void)
{
    char line[AOS_LIVE_MSG_MAX + 1];
    int n;
    while ((n = aos_hal_live_take(MSH_APP_ID, line, AOS_LIVE_MSG_MAX)) > 0) {
        line[n] = 0;
        command(line);
    }
    if (aos_hal_live_idle_ms(MSH_APP_ID) > 15000) return;      /* nobody looks */
    if (!P.buf) P.buf = (char *)msh_alloc(BUF_MAX);
    if (!P.buf) return;
    uint32_t now = msh_now_s();
    bool tick = now - P.last_put >= 10;
    msh_lock();
    if (tick || MSH.seq_state != P.put_state || MSH.seq_cfg != P.put_cfg || MSH.seq_msgs != P.put_msgs) {
        P.put_state = MSH.seq_state;
        P.put_cfg = MSH.seq_cfg;
        put_state();
    }
    if (tick || MSH.seq_nodes != P.put_nodes) {
        P.put_nodes = MSH.seq_nodes;
        put_nodes();
    }
    if (tick || MSH.seq_msgs != P.put_msgs) {
        P.put_msgs = MSH.seq_msgs;
        put_msgs();
    }
    msh_unlock();
    if (tick) P.last_put = now;
}

void msh_portal_stop(void)
{
    aos_hal_live_clear(MSH_APP_ID);
    free(P.buf);
    memset(&P, 0, sizeof P);
}
