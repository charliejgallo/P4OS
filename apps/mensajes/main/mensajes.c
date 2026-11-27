/*
 * P4OS - Mensajes: a client for Telegram with the person's own account.
 *
 * Telegram's own protocol, MTProto 2.0, written in C for the board
 * (mj_mtp.c over mj_crypto.c and the TL tables tools/tl_gen.py makes): no
 * TDLib, no server in the middle. The network thread (mj_api.c) keeps the
 * connection, signs in and fills the store; this file is the app around
 * it: the life cycle, the icon, the 100 ms timer that redraws what moved
 * in the store, the notifications of new messages while the app is hidden
 * (it is kept alive for that: AOS_APP_FLAG_BACKGROUND), the number on the
 * icon, and the portal's page (mj_web.c).
 *
 * Not Telegram's app nor its name or logo: the API's terms ask that an
 * unofficial client have its own. "for Telegram" is said in the
 * description.
 */
#include <time.h>

#include "aos_icon_ops.h"
#include "aos_notif_internal.h"
#include "mj_ui.h"

/* Two speech bubbles, one over the other, with a paper plane's fold */
static const uint8_t MENSAJES_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -10, -12, 70, 52, 22, AIC_C_LIT(0xFFFFFF), 255),
    AIC_RECT(AIC_CENTER, -34, 8, 16, 16, 3, AIC_C_LIT(0xFFFFFF), 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, 14, 16, 58, 42, 18, AIC_C_LIT(0x7FD3FF), 255),
    AIC_RECT(AIC_CENTER, 34, 32, 14, 14, 3, AIC_C_LIT(0x7FD3FF), 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, -22, -14, 12, 12, AIC_CIRCLE, AIC_C_LIT(0x2A7BD8), 255),
    AIC_RECT(AIC_CENTER, -4, -14, 12, 12, AIC_CIRCLE, AIC_C_LIT(0x2A7BD8), 255),
    AIC_RECT(AIC_CENTER, 14, -14, 12, 12, AIC_CIRCLE, AIC_C_LIT(0x2A7BD8), 255),
    AIC_END
};

mj_ui_t U;
static lv_timer_t *s_timer;
static uint32_t s_list_ms, s_status_ms;
static bool s_list_due;

/* ---- notifications (the network thread, the store locked) ---- */

static void on_incoming(const mj_msg_t *m)
{
    if (!U.notif_on) return;
    if (U.visible && mj_pkey_eq(m->chat, U.open)) return;
    mj_dialog_t *d = mj_store_dialog(m->chat);
    if (d && d->muted) return;
    aos_notif_t n;
    memset(&n, 0, sizeof n);
    n.uid = 0x4D000000u ^ ((uint32_t)m->id * 2654435761u) ^ (uint32_t)m->chat.id;
    n.category = AOS_NOTIF_SOCIAL;
    snprintf(n.app, sizeof n.app, "Mensajes");
    const char *chat = mj_peer_name(m->chat);
    if (m->chat.type != MJ_PEER_USER && m->from.type == MJ_PEER_USER) {
        mj_peer_t *p = mj_store_peer(m->from);
        snprintf(n.title, sizeof n.title, "%s \xC2\xB7 %s", p ? (p->first[0] ? p->first : p->name) : "?", chat);
    } else {
        snprintf(n.title, sizeof n.title, "%s", chat);
    }
    mj_msg_line(m, n.message, sizeof n.message);
    n.when = time(NULL);
    n.repeticiones = 1;
    aos_notif_push(&n);
}

/* ---- the screen ---- */

static void panes(void)
{
    U.list_pane = U.chat_pane = NULL;
    U.list = U.search_ta = U.net_lbl = U.list_title = NULL;
    if (U.land) {
        U.list_w = 460;
        U.list_pane = mj_box(U.root, U.list_w, U.H);
        lv_obj_t *sep = mj_box(U.root, 1, U.H);
        lv_obj_set_pos(sep, U.list_w, 0);
        lv_obj_set_style_bg_color(sep, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        U.chat_pane = mj_box(U.root, U.W - U.list_w - 1, U.H);
        lv_obj_set_pos(U.chat_pane, U.list_w + 1, 0);
        mj_list_build(U.list_pane);
        mj_chat_build(U.chat_pane);
    } else if (U.open.type) {
        U.chat_pane = mj_box(U.root, U.W, U.H);
        mj_chat_build(U.chat_pane);
    } else {
        U.list_pane = mj_box(U.root, U.W, U.H);
        mj_list_build(U.list_pane);
    }
}

void mj_ui_rebuild(void)
{
    /* everything on the root goes: what pointed at it, first */
    mj_kb_hide();
    if (U.sheet) {
        lv_obj_delete(U.sheet);
        U.sheet = NULL;
    }
    U.viewer = NULL;
    U.emoji_panel = NULL;
    U.login = NULL;
    mj_chat_free_rows();
    U.chat_list = NULL;
    lv_obj_clean(U.root);
    U.chat_pane = U.list_pane = NULL;
    mj_store_lock();
    bool ready = mj_S.auth == MJ_AUTH_READY;
    mj_store_unlock();
    if (!ready) {
        mj_login_update();
        return;
    }
    panes();
}

void mj_ui_open_chat(mj_pkey_t k)
{
    if (mj_pkey_eq(k, U.open) && U.chat_list) return;
    U.open = k;
    U.reply_to = U.editing = 0;
    mj_api_open_chat(k);
    if (U.land && U.chat_pane) {
        mj_kb_hide();
        mj_chat_free_rows();
        U.chat_list = NULL;
        U.emoji_panel = NULL;
        lv_obj_clean(U.chat_pane);
        mj_chat_build(U.chat_pane);
        mj_list_refresh();
    } else {
        mj_ui_rebuild();
    }
}

void mj_ui_close_chat(void)
{
    if (!U.open.type) return;
    U.open.type = 0;
    U.open.id = 0;
    U.reply_to = U.editing = 0;
    mj_api_close_chat();
    mj_ui_rebuild();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    mj_store_lock();
    int auth = mj_S.auth;
    uint32_t va = mj_S.auth_ver, vd = mj_S.dialogs_ver, vp = mj_S.peers_ver, vc = mj_S.chat_ver,
             vm = mj_S.media_ver, vt = mj_S.toast_ver, vn = mj_S.net_ver, vs = mj_S.search_ver;
    int toast = mj_S.toast;
    char targ[64];
    snprintf(targ, sizeof targ, "%s", mj_S.toast_arg);
    int unread = mj_S.unread_total;
    bool chat_here = mj_pkey_eq(mj_S.chat, U.open);
    mj_store_unlock();
    uint32_t vi = mj_img_ver();
    uint32_t now = mj_now_ms();

    if (vt != U.v_toast) {
        U.v_toast = vt;
        if (U.visible) mj_toast_code(toast, targ);
    }
    if (unread != U.badge) {
        U.badge = unread;
        aos_ui_set_badge(MJ_APP_ID, unread);
    }
    if (auth != MJ_AUTH_READY) {
        if (va != U.v_auth || !U.login) {
            U.v_auth = va;
            if (!U.login) mj_ui_rebuild();
            else mj_login_update();
        }
        mj_web_tick();
        return;
    }
    if (U.login) {
        /* just signed in */
        U.v_auth = va;
        mj_ui_rebuild();
        return;
    }
    if (vd != U.v_dialogs || vp != U.v_peers) {
        U.v_dialogs = vd;
        U.v_peers = vp;
        s_list_due = true;
    }
    if (s_list_due && (uint32_t)(now - s_list_ms) > 300) {
        s_list_due = false;
        s_list_ms = now;
        if (!U.show_found) mj_list_refresh();
        mj_list_net();
    }
    if (vs != U.v_search) {
        U.v_search = vs;
        if (U.show_found) mj_list_refresh();
    }
    if (vc != U.v_chat && chat_here) {
        U.v_chat = vc;
        mj_chat_refresh(false);
    }
    if (vm != U.v_media || vi != U.v_img) {
        U.v_media = vm;
        U.v_img = vi;
        mj_avatars_refresh(U.root);
        mj_chat_media();
    }
    if (vn != U.v_net) {
        U.v_net = vn;
        mj_list_net();
        mj_chat_status();
    }
    if ((uint32_t)(now - s_status_ms) > 1000) {
        s_status_ms = now;
        mj_chat_status();
    }
    mj_web_tick();
}

/* ---- life cycle ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&U, 0, sizeof U);
    mj_port_init();
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.visible = true;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    int32_t v = 1;
    if (!aos_hal_pref_get_i32("msj_notif", &v)) v = 1;
    U.notif_on = v != 0;

    mj_store_init();
    mj_api_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s/mensajes", aos_hal_path_data());
    const char *sd = aos_hal_path_sd_root();
    snprintf(cfg.dl_dir, sizeof cfg.dl_dir, "%s/Mensajes", sd ? sd : aos_hal_path_data());
    snprintf(U.data_dir, sizeof U.data_dir, "%s", cfg.data_dir);
    snprintf(U.dl_dir, sizeof U.dl_dir, "%s", cfg.dl_dir);
    const char *lang = aos_i18n_current();
    snprintf(cfg.lang, sizeof cfg.lang, "%s", lang && *lang ? lang : "es");
    snprintf(cfg.device, sizeof cfg.device, "Waveshare ESP32-P4");
    snprintf(cfg.system, sizeof cfg.system, "P4OS");
    snprintf(cfg.version, sizeof cfg.version, "1.0");
    cfg.cache_max = 48u * 1024 * 1024;
    cfg.on_incoming = on_incoming;
    mj_img_start();
    if (!mj_api_start(&cfg)) aos_hal_log("mensajes", "the network thread did not start");
    mj_ui_rebuild();
    s_timer = lv_timer_create(timer_cb, 100, NULL);
    aos_hal_log("mensajes", "started; %u KB held", (unsigned)(mj_mem_used() / 1024));
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    mj_web_stop();
    uint32_t t0 = mj_now_ms();
    mj_api_stop(8000);
    aos_hal_log("mensajes", "closed in %u ms; it held %u KB (peak %u KB) and %u KB of pictures",
                (unsigned)(mj_now_ms() - t0), (unsigned)(mj_mem_used() / 1024), (unsigned)(mj_mem_peak() / 1024),
                (unsigned)(mj_img_bytes() / 1024));
    mj_kb_hide();
    U.sheet = U.viewer = NULL;
    mj_chat_free_rows();
    lv_obj_clean(U.root);
    mj_img_stop();
    mj_list_free();
    mj_store_lock();
    mj_store_free();
    mj_store_unlock();
    aos_ui_set_badge(MJ_APP_ID, 0);
    memset(&U, 0, sizeof U);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    mj_ui_rebuild();
    return true;
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.sheet) {
        mj_sheet_close();
        return true;
    }
    if (U.kb) {
        mj_kb_hide();
        return true;
    }
    if (mj_chat_back()) return true;
    if (U.show_found || U.filter[0]) {
        U.show_found = false;
        U.filter[0] = 0;
        if (U.search_ta) lv_textarea_set_text(U.search_ta, "");
        mj_list_refresh();
        return true;
    }
    if (U.open.type && !U.land) {
        mj_ui_close_chat();
        return true;
    }
    return false;
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    U.visible = true;
    if (U.open.type) mj_api_mark_read();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    U.visible = false;
    mj_kb_hide();
}

static bool mensajes_init(aos_app_t *app)
{
    app->desc.id       = "aos.mensajes";
    app->desc.name     = "Mensajes";
    app->desc.icon     = "M";
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x3AA0F0;
    app->desc.color_b  = 0x1A5FB4;
    app->desc.order    = 44;
    app->desc.flags    = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, MENSAJES_ICON, sizeof MENSAJES_ICON);

    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    return true;
}

AOS_APP_ENTRY(mensajes_init);
