/*
 * Malla - a Meshtastic client for P4OS.
 *
 * Copyright (C) 2026 P4OS contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. It is distributed WITHOUT ANY WARRANTY; see
 * apps/malla/LICENSE. (The rest of P4OS is MIT; this app is GPL-3.0
 * because Meshtastic's protobuf definitions, compiled into it, are.)
 *
 * The board talks to a Meshtastic node the way the phone apps do, over USB
 * or a header UART, over Wi-Fi (TCP 4403) or over Bluetooth, and shows the
 * mesh: chats by channel and direct messages, the nodes, a map, telemetry,
 * traceroute and the basics of the node's setup. apps/malla/README.md has
 * the protocol, what was tried and what is missing.
 *
 * Here: the app's life. The session (msh_sess.c) runs in its own thread
 * for as long as the app lives, also hidden: while it is connected the app
 * tells the system it is busy (aos_ui_set_busy) so it is kept, and new
 * messages become notifications (a direct one marked important). In front,
 * a direct message from another conversation is a toast; the channels only
 * move the badge.
 */
#include "msh_ui.h"

#include "aos_icon_ops.h"
#include "aos_notif_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Three nodes and the one in the middle, joined: a mesh. */
static const uint8_t MALLA_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -11, -5, 44, 4, 2, AIC_C_TEXT, 200),
    AIC_ROT(1200),
    AIC_RECT(AIC_CENTER, 11, -5, 44, 4, 2, AIC_C_TEXT, 200),
    AIC_ROT(600),
    AIC_RECT(AIC_CENTER, 0, 14, 44, 4, 2, AIC_C_TEXT, 200),
    AIC_RECT(AIC_CENTER, 0, -11, 4, 26, 2, AIC_C_TEXT, 140),
    AIC_RECT(AIC_CENTER, -11, 8, 25, 4, 2, AIC_C_TEXT, 140),
    AIC_ROT(1514),
    AIC_RECT(AIC_CENTER, 11, 8, 25, 4, 2, AIC_C_TEXT, 140),
    AIC_ROT(286),
    AIC_RECT(AIC_CENTER, 0, -24, 17, 17, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -22, 14, 17, 17, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 22, 14, 17, 17, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 0, 2, 13, 13, AIC_CIRCLE, AIC_C_LIT(0x0B3D2A), 255),
    AIC_BORDER(AIC_DIV(26), AIC_C_TEXT, 255),
    AIC_END
};

static lv_timer_t *s_timer;

/* ---- what the person chose ---- */

void msh_app_connect(const msh_target_t *t)
{
    msh_target_save(t);
    msh_sess_start(t);
    char txt[96];
    msh_target_text(t, txt, sizeof txt);
    aos_hal_log(MSH_TAG, "connecting: %s", txt);
    UI.dirty = true;
}

void msh_app_disconnect(void)
{
    msh_sess_stop();
    msh_target_t none = { 0 };
    msh_target_save(&none);
    msh_lock();
    MSH.target = none;
    MSH.state = MSH_ST_IDLE;
    MSH.err[0] = 0;
    MSH.seq_state++;
    msh_unlock();
    UI.dirty = true;
}

/* ---- new messages: a toast in front, a notification behind ---- */

static void announce(const msh_msg_t *m)
{
    char who[48], where[40], title[64];
    msh_node_name(m->from, who, sizeof who);
    if (m->peer) snprintf(title, sizeof title, "%s", who);
    else snprintf(title, sizeof title, "%.30s %s %.24s", who, _("en"), msh_channel_name(m->ch, where, sizeof where));
    bool here = !UI.hidden && UI.view == MSH_V_CHAT && UI.chat_peer == m->peer && (m->peer || UI.chat_ch == m->ch);
    if (here) {
        aos_hal_beep(1800, 25);
        return;
    }
    if (!UI.hidden) {
        /* in front, only a direct message interrupts: a busy channel would
         * be a toast every few seconds, and the tab's badge already counts */
        if (!m->peer) return;
        char t[160], safe[200];
        snprintf(t, sizeof t, "%s: %s", title, m->text);
        msh_ui_safe(safe, sizeof safe, t);
        aos_ui_toast(safe, 2500);
        aos_hal_beep(1500, 40);
        return;
    }
    aos_notif_t n;
    memset(&n, 0, sizeof n);
    n.uid = 0x4D500000u | (m->id & 0xFFFFFu);
    n.category = AOS_NOTIF_SOCIAL;
    snprintf(n.app, sizeof n.app, "Malla");
    msh_scpy(n.title, sizeof n.title, title);
    msh_scpy(n.message, sizeof n.message, m->text);
    n.when = time(NULL);
    n.important = m->peer != 0;
    aos_notif_push(&n);
}

static void check_new(void)
{
    msh_lock();
    uint32_t rx = MSH.rx_count;
    if (rx == UI.seen_rx) {
        msh_unlock();
        return;
    }
    int fresh = (int)(rx - UI.seen_rx);
    UI.seen_rx = rx;
    /* the newest unread ones that came in, oldest of them first */
    msh_msg_t *list[4];
    int n = 0;
    for (int i = MSH.msg_n - 1; i >= 0 && n < fresh && n < 4; i--) {
        msh_msg_t *m = msh_msg_at(i);
        if (m->state == MSH_MSG_RX && m->unread) list[n++] = m;
    }
    for (int k = n - 1; k >= 0; k--) {
        msh_msg_t copy = *list[k];
        msh_unlock();
        announce(&copy);
        msh_lock();
    }
    msh_unlock();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    msh_ui_scan_tick();
    check_new();
    msh_ui_refresh();
    msh_portal_tick();
    static bool busy;
    bool now = MSH.target.kind != MSH_LINK_NONE && msh_sess_running();
    if (now != busy) {
        busy = now;
        aos_ui_set_busy(MSH_APP_ID, busy);
    }
}

/* ---- the app's life ---- */

static void *malla_create(aos_app_t *self, lv_obj_t *root)
{
    memset(&UI, 0, sizeof UI);
    UI.self = self;
    UI.root = root;
    UI.chat_ch = 0;
    if (!msh_model_init()) {
        lv_obj_t *l = lv_label_create(root);
        lv_label_set_text(l, _("No hay memoria para la malla"));
        lv_obj_center(l);
        return &UI;
    }
    msh_target_t t;
    bool have = msh_target_load(&t);
    if (have) {
        msh_lock();
        MSH.target = t;
        msh_unlock();
    }
    msh_ui_build();
    msh_lock();
    UI.seen_rx = MSH.rx_count;
    msh_unlock();
    if (have) msh_sess_start(&t);
    s_timer = lv_timer_create(timer_cb, 250, NULL);
    return &UI;
}

static void malla_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    msh_ui_sheet_close();
    msh_ui_scan_stop();
    msh_portal_stop();
    msh_sess_stop();                /* the thread out before the model goes */
    msh_store_save();
    msh_model_free();
    aos_ui_set_busy(MSH_APP_ID, false);
    memset(&UI, 0, sizeof UI);
}

static void malla_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    UI.hidden = true;
    msh_ui_sheet_close();
    msh_store_save();
}

static void malla_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    UI.hidden = false;
    UI.dirty = true;
}

static bool malla_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    return msh_ui_back();
}

static bool malla_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    UI.root = root;
    msh_ui_build();
    return true;
}

static bool malla_init(aos_app_t *app)
{
    app->desc.id       = "aos.malla";
    app->desc.name     = "Malla";
    app->desc.icon     = LV_SYMBOL_WIFI;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x34C77B;
    app->desc.color_b  = 0x0B6B44;
    app->desc.order    = 330;
    app->desc.flags    = AOS_APP_FLAG_KEEP;
    aos_icon_set_ops(app, MALLA_ICON, sizeof MALLA_ICON);

    app->create  = malla_create;
    app->destroy = malla_destroy;
    app->hide    = malla_hide;
    app->show    = malla_show;
    app->back    = malla_back;
    app->resize  = malla_resize;
    return true;
}

AOS_APP_ENTRY(malla_init);
