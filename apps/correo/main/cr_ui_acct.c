/*
 * P4OS - Correo: the settings and an account (cr_ui.h).
 *
 * A new account starts from its provider: Gmail, Outlook, iCloud, Fastmail
 * and Yahoo fill in their servers (cr_store.c) and say how to get the app
 * password each of them asks for; "Otro" guesses imap.<domain> and
 * smtp.<domain>, and every field can be changed by hand. "Probar y guardar"
 * logs in to both servers on the network thread (CR_J_TEST) before keeping
 * anything; a failure says which server and why, and the account can be
 * kept anyway (a server that is down today).
 *
 * The password goes to its preference (cr_pass_<id>) and nowhere else; the
 * field shows it blank on an account that has one, and leaving it blank
 * keeps it.
 *
 * OAuth2 (what Google and Microsoft would rather have) is not here: it
 * needs a client registered with each of them, a browser on the person's
 * phone for the consent (a QR code and the device-code flow would do), the
 * tokens kept like the passwords (cr_token_<id>) and refreshed before they
 * expire, and SASL XOAUTH2 in cr_sync.c and cr_net.c. README.md has it.
 */
#include "cr_ui.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>

static const uint32_t COLORS[] = {0x0A84FF, 0x30D158, 0xFF9F0A, 0xBF5AF2, 0xFF375F, 0x40C8E0};

static struct {
    int            idx;            /* -1 new */
    cr_acct_cfg_t  cfg;
    char           pass[128];
    bool           pass_typed;
    bool           manual;         /* the servers were changed by hand */
    bool           testing;
    uint32_t       token;
    char           result[300];
    bool           failed;
    lv_obj_t      *col;
    lv_obj_t      *ta_name, *ta_email, *ta_pass, *ta_label, *ta_sig;
    lv_obj_t      *ta_ih, *ta_ip, *ta_iu, *ta_sh, *ta_sp, *ta_su;
    lv_obj_t      *dd_is, *dd_ss;
    lv_obj_t      *help;
} A;

/* ---- the settings ------------------------------------------------------------------------------ */

static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    ui_pop();
}

static void acct_row_cb(lv_event_t *e)
{
    cr_acct_edit((int)(intptr_t)lv_event_get_user_data(e));
}

static void new_acct_cb(lv_event_t *e)
{
    (void)e;
    cr_acct_edit(-1);
}

static void opt_threads_cb(lv_event_t *e)
{
    CR.threads = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    cr_store_save_options();
    cr_lock();
    cr_touch();
    cr_unlock();
}

static void opt_notify_cb(lv_event_t *e)
{
    CR.notify = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    cr_store_save_options();
}

static void opt_boot_cb(lv_event_t *e)
{
    CR.boot = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    aos_ui_boot_start_set(CR_APP_ID, CR.boot);
    cr_store_save_options();
}

static const int CACHE_MB[] = {50, 100, 250, 500, 1000, 2000};

static void cache_pick(int i, void *ud)
{
    (void)ud;
    if (i < 0 || i >= (int)(sizeof CACHE_MB / sizeof CACHE_MB[0])) return;
    CR.cache_mb = CACHE_MB[i];
    cr_store_save_options();
    cr_cache_trim((uint32_t)CR.cache_mb * 1024u * 1024u);
    ui_rebuild();
}

static void cache_cb(lv_event_t *e)
{
    (void)e;
    static char t[6][24];
    static const char *items[6];
    for (int i = 0; i < 6; i++) {
        if (CACHE_MB[i] >= 1000) snprintf(t[i], sizeof t[i], "%d GB", CACHE_MB[i] / 1000);
        else snprintf(t[i], sizeof t[i], "%d MB", CACHE_MB[i]);
        items[i] = t[i];
    }
    ui_sheet(_("Lo más que guarda en la tarjeta"), items, 6, cache_pick, NULL);
}

static void empty_pick(int i, void *ud)
{
    (void)ud;
    if (i != 0) return;
    cr_cache_trim(0);
    aos_ui_toast(_("Caché vaciada"), 1500);
    ui_rebuild();
}

static void empty_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm(_("Se borran los mensajes guardados en la tarjeta (no los del servidor): se vuelven a bajar al abrirlos."),
               _("Vaciar"), true, empty_pick, NULL);
}

void cr_settings_build(lv_obj_t *page)
{
    int32_t pw = lv_obj_get_style_width(page, 0);
    ui_bar(page, _("Buzones"), _("Ajustes"), settings_back_cb);
    lv_obj_t *col = ui_col(page, pw, U.H - 96);
    lv_obj_set_y(col, 96);
    int32_t w = pw - 2 * AOS_UI_PAD;
    ui_label(col, _("Cuentas"), aos_font_small, AOS_C_DIM);
    cr_lock();
    for (int i = 0; i < CR.na; i++) {
        lv_obj_t *row = ui_box(col, lv_pct(100), 104);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, acct_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dot = ui_box(row, 20, 20);
        lv_obj_set_style_radius(dot, 10, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(CR.a[i].cfg.color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t *n = ui_label_dots(row, ui_acct_name(i), aos_font_body, AOS_C_TEXT, w - 140);
        lv_obj_set_pos(n, 60, 14);
        lv_obj_t *s = ui_label_dots(row, CR.a[i].cfg.email, aos_font_caption, AOS_C_DIM, w - 140);
        lv_obj_set_pos(s, 60, 58);
        lv_obj_t *ch = ui_label(row, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    cr_unlock();
    if (CR.na < CR_ACCT_MAX - 1) ui_btn(col, AOS_SYM_PLUS, _("Agregar una cuenta"), AOS_C_CARD2, new_acct_cb, NULL);

    lv_obj_t *l = ui_label(col, _("Opciones"), aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 20, 0);
    lv_obj_t *card = ui_card(col);
    ui_switch_row(card, _("Agrupar por conversación"), CR.threads, opt_threads_cb);
    ui_switch_row(card, _("Avisar del correo nuevo con la app en segundo plano"), CR.notify, opt_notify_cb);
    ui_label_wrap(card, _("Correo se queda conectado esperando (IDLE) y avisa apenas llega algo."), aos_font_caption,
                  AOS_C_DIM);
    ui_switch_row(card, _("Abrir Correo al prender la placa"), CR.boot, opt_boot_cb);

    l = ui_label(col, _("En la tarjeta"), aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 20, 0);
    card = ui_card(col);
    char used[32], cap[32], t[160];
    ui_size(used, sizeof used, cr_cache_used());
    if (CR.cache_mb >= 1000) snprintf(cap, sizeof cap, "%d GB", CR.cache_mb / 1000);
    else snprintf(cap, sizeof cap, "%d MB", CR.cache_mb);
    snprintf(t, sizeof t, _("Mensajes guardados: %s de %s"), used, cap);
    ui_label_wrap(card, t, aos_font_body, AOS_C_TEXT);
    lv_obj_t *br = ui_box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(br, 12, 0);
    ui_btn(br, NULL, _("Cambiar el tope"), AOS_C_CARD2, cache_cb, NULL);
    ui_btn(br, AOS_SYM_DELETE, _("Vaciar"), AOS_C_CARD2, empty_cb, NULL);
    ui_label_wrap(card, _("Los adjuntos que guardás van a la carpeta Correo de la tarjeta."), aos_font_caption,
                  AOS_C_DIM);
}

/* ---- an account -------------------------------------------------------------------------------- */

static void from_provider(const cr_provider_t *p, const char *email)
{
    cr_copy(A.cfg.provider, sizeof A.cfg.provider, p ? p->key : "");
    if (!p) return;
    const char *at = strchr(email, '@');
    const char *dom = at ? at + 1 : "";
    if (p->imap[0]) cr_copy(A.cfg.imap_host, sizeof A.cfg.imap_host, p->imap);
    else if (dom[0]) snprintf(A.cfg.imap_host, sizeof A.cfg.imap_host, "imap.%.80s", dom);
    if (p->smtp[0]) cr_copy(A.cfg.smtp_host, sizeof A.cfg.smtp_host, p->smtp);
    else if (dom[0]) snprintf(A.cfg.smtp_host, sizeof A.cfg.smtp_host, "smtp.%.80s", dom);
    A.cfg.imap_port = p->imap_port;
    A.cfg.imap_sec = (uint8_t)p->imap_sec;
    A.cfg.smtp_port = p->smtp_port;
    A.cfg.smtp_sec = (uint8_t)p->smtp_sec;
    A.cfg.save_sent = p->save_sent;
    if (p->user_local && at) cr_ncopy(A.cfg.imap_user, sizeof A.cfg.imap_user, email, (size_t)(at - email));
    else cr_copy(A.cfg.imap_user, sizeof A.cfg.imap_user, email);
    cr_copy(A.cfg.smtp_user, sizeof A.cfg.smtp_user, email);
}

void cr_acct_edit(int i)
{
    memset(&A, 0, sizeof A);
    A.idx = i;
    if (i >= 0 && i < CR.na) {
        A.cfg = CR.a[i].cfg;
        A.manual = true;
    } else {
        A.cfg.imap_port = 993;
        A.cfg.smtp_port = 465;
        A.cfg.save_sent = true;
        A.cfg.color = COLORS[CR.na % 6];
        from_provider(cr_provider_key("gmail"), "");
    }
    ui_go(SCR_ACCT);
}

static void read_fields(void)
{
    if (!A.ta_email) return;
    cr_copy(A.cfg.name, sizeof A.cfg.name, lv_textarea_get_text(A.ta_name));
    cr_copy(A.cfg.email, sizeof A.cfg.email, lv_textarea_get_text(A.ta_email));
    cr_trim(A.cfg.email);
    const char *p = lv_textarea_get_text(A.ta_pass);
    if (p[0]) {
        cr_copy(A.pass, sizeof A.pass, p);
        A.pass_typed = true;
    }
    cr_copy(A.cfg.label, sizeof A.cfg.label, lv_textarea_get_text(A.ta_label));
    cr_copy(A.cfg.sig, sizeof A.cfg.sig, lv_textarea_get_text(A.ta_sig));
    cr_copy(A.cfg.imap_host, sizeof A.cfg.imap_host, lv_textarea_get_text(A.ta_ih));
    A.cfg.imap_port = atoi(lv_textarea_get_text(A.ta_ip));
    cr_copy(A.cfg.imap_user, sizeof A.cfg.imap_user, lv_textarea_get_text(A.ta_iu));
    cr_copy(A.cfg.smtp_host, sizeof A.cfg.smtp_host, lv_textarea_get_text(A.ta_sh));
    A.cfg.smtp_port = atoi(lv_textarea_get_text(A.ta_sp));
    cr_copy(A.cfg.smtp_user, sizeof A.cfg.smtp_user, lv_textarea_get_text(A.ta_su));
    A.cfg.imap_sec = (uint8_t)lv_dropdown_get_selected(A.dd_is);
    A.cfg.smtp_sec = (uint8_t)lv_dropdown_get_selected(A.dd_ss);
    cr_trim(A.cfg.imap_host);
    cr_trim(A.cfg.smtp_host);
}

static void provider_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    read_fields();
    from_provider(&CR_PROVIDERS[k], A.cfg.email);
    A.manual = false;
    ui_rebuild();
}

static void email_done_cb(lv_event_t *e)
{
    (void)e;
    read_fields();
    if (A.manual && A.idx >= 0) return;
    const cr_provider_t *p = cr_provider_for(A.cfg.email);
    if (!p) p = cr_provider_key(A.cfg.provider[0] ? A.cfg.provider : "propio");
    if (!p) p = cr_provider_key("propio");
    from_provider(p, A.cfg.email);
    ui_rebuild();
}

static void edited_cb(lv_event_t *e)
{
    (void)e;
    A.manual = true;
}

static bool valid(void)
{
    if (!cr_addr_valid(A.cfg.email)) {
        aos_ui_toast(_("La dirección de correo no está bien escrita"), 2500);
        return false;
    }
    if (!A.cfg.imap_host[0] || !A.cfg.smtp_host[0] || A.cfg.imap_port <= 0 || A.cfg.smtp_port <= 0) {
        aos_ui_toast(_("Faltan los servidores"), 2500);
        return false;
    }
    if (A.idx < 0 && !A.pass_typed) {
        aos_ui_toast(_("Falta la contraseña"), 2000);
        return false;
    }
    return true;
}

static void save(void)
{
    cr_lock();
    int i = A.idx;
    if (i < 0) {
        if (CR.na >= CR_ACCT_MAX - 1) {
            cr_unlock();
            return;
        }
        i = CR.na++;
        cr_folder_t *f = CR.a[i].f;
        memset(&CR.a[i], 0, sizeof CR.a[i]);
        CR.a[i].f = f;
        cr_acct_new_id(A.cfg.id);
    }
    bool servers_changed = strcmp(CR.a[i].cfg.imap_host, A.cfg.imap_host) || CR.a[i].cfg.imap_port != A.cfg.imap_port ||
                           strcmp(CR.a[i].cfg.imap_user, A.cfg.imap_user) || CR.a[i].cfg.imap_sec != A.cfg.imap_sec;
    CR.a[i].cfg = A.cfg;
    if (A.idx < 0 || servers_changed) {
        for (int k = 0; k < CR.a[i].nf; k++) cr_free(CR.a[i].f[k].h);
        CR.a[i].nf = 0;
        CR.a[i].unseen = 0;
        CR.gen++;
    } else {
        cr_touch();
    }
    cr_unlock();
    if (A.pass_typed) cr_pass_set(A.cfg.id, A.pass);
    memset(A.pass, 0, sizeof A.pass);
    cr_store_save_accounts();
    cr_sync_kick(i);
    cr_sync_reload();
}

static void keep_pick(int k, void *ud)
{
    (void)ud;
    if (k != 0) return;
    save();
    ui_pop();
    if (ui_top() == SCR_WELCOME || ui_top() == SCR_SETTINGS) {
        U.depth = 2;
        U.stack[0] = SCR_HOME;
        U.stack[1] = SCR_LIST;
        U.l_acct = -1;
        ui_rebuild();
    }
}

static void test_cb(lv_event_t *e)
{
    (void)e;
    if (A.testing) return;
    read_fields();
    if (!valid()) return;
    cr_job_t *j = cr_job_new(CR_J_TEST);
    if (!j) return;
    j->cfg = cr_alloc(sizeof(cr_acct_cfg_t));
    if (!j->cfg) {
        cr_job_free(j);
        return;
    }
    *j->cfg = A.cfg;
    if (A.pass_typed) cr_copy(j->pass, sizeof j->pass, A.pass);
    else cr_pass_get(A.cfg.id, j->pass, sizeof j->pass);
    A.token = j->token = ui_token(OWN_ACCT);
    A.testing = true;
    A.result[0] = '\0';
    cr_job_put(j);
    ui_rebuild();
}

static void keep_anyway_cb(lv_event_t *e)
{
    (void)e;
    keep_pick(0, NULL);
}

void cr_acct_job(cr_job_t *j)
{
    if (j->token != A.token) return;
    A.testing = false;
    if (j->ok) {
        aos_ui_toast(_("Las dos conexiones andan: cuenta guardada"), 2500);
        keep_pick(0, NULL);
        return;
    }
    A.failed = true;
    snprintf(A.result, sizeof A.result, "%s\n%s", ui_err_text(j->err_kind), j->why);
    if (ui_top() == SCR_ACCT) ui_rebuild();
}

static void delete_pick(int k, void *ud)
{
    (void)ud;
    if (k != 0 || A.idx < 0) return;
    cr_lock();
    int i = A.idx;
    char id[8];
    cr_copy(id, sizeof id, CR.a[i].cfg.id);
    cr_folder_t *f = CR.a[i].f;
    for (int k2 = 0; k2 < CR.a[i].nf; k2++) cr_free(f[k2].h);
    memmove(&CR.a[i], &CR.a[i + 1], sizeof CR.a[0] * (size_t)(CR.na - i - 1));
    CR.na--;
    CR.a[CR.na].f = f;
    CR.a[CR.na].nf = 0;
    CR.gen++;
    cr_unlock();
    cr_pass_erase(id);
    cr_cache_account_drop(id);
    cr_store_save_accounts();
    cr_sync_reload();
    U.depth = 1;
    U.stack[0] = CR.na ? SCR_HOME : SCR_WELCOME;
    if (CR.na) {
        U.stack[1] = SCR_SETTINGS;
        U.depth = 2;
    }
    U.l_acct = -1;
    ui_rebuild();
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm(_("Se borra la cuenta de la placa, con lo que guardó en la tarjeta. En el servidor no se toca nada."),
               _("Borrar la cuenta"), true, delete_pick, NULL);
}

static void acct_back_cb(lv_event_t *e)
{
    (void)e;
    ui_pop();
}

bool cr_acct_back(void)
{
    return false;
}

static lv_obj_t *server_block(lv_obj_t *col, const char *title, const char *host, int port, int sec, const char *user,
                              lv_obj_t **ta_h, lv_obj_t **ta_p, lv_obj_t **dd, lv_obj_t **ta_u)
{
    lv_obj_t *card = ui_card(col);
    ui_label(card, title, aos_font_body, AOS_C_TEXT);
    *ta_h = ui_field(card, _("Servidor"), host, "mail.example.com", true);
    lv_obj_t *row = ui_box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *pc = ui_box(row, lv_pct(34), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pc, LV_FLEX_FLOW_COLUMN);
    char p[12];
    snprintf(p, sizeof p, "%d", port);
    *ta_p = ui_field(pc, _("Puerto"), p, NULL, true);
    lv_textarea_set_accepted_chars(*ta_p, "0123456789");
    lv_obj_t *sc = ui_box(row, lv_pct(62), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    ui_label(sc, _("Seguridad"), aos_font_small, AOS_C_DIM);
    *dd = lv_dropdown_create(sc);
    lv_obj_set_width(*dd, lv_pct(100));
    lv_dropdown_set_options(*dd, "SSL/TLS\nSTARTTLS\nNinguna");
    lv_dropdown_set_selected(*dd, (uint32_t)sec);
    lv_obj_set_style_text_font(*dd, aos_font_body, 0);
    lv_obj_set_style_bg_color(*dd, AOS_C_CARD2, 0);
    lv_obj_set_style_border_width(*dd, 0, 0);
    lv_obj_add_event_cb(*dd, edited_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *list = lv_dropdown_get_list(*dd);
    if (list) lv_obj_set_style_text_font(list, aos_font_body, 0);
    *ta_u = ui_field(card, _("Usuario"), user, NULL, true);
    lv_obj_add_event_cb(*ta_h, edited_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(*ta_p, edited_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (sec == CR_SEC_NONE) {
        ui_label_wrap(card, _("Sin cifrar, la contraseña viaja a la vista: sólo para un servidor de tu red."),
                      aos_font_caption, AOS_C_ORANGE);
    }
    if (sec == CR_SEC_STARTTLS) {
        ui_label_wrap(card, ui_err_text(CR_E_STARTTLS), aos_font_caption, AOS_C_ORANGE);
    }
    return card;
}

static void color_cb(lv_event_t *e)
{
    read_fields();
    A.cfg.color = COLORS[(int)(intptr_t)lv_event_get_user_data(e)];
    ui_rebuild();
}

static void sent_cb(lv_event_t *e)
{
    A.cfg.save_sent = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

void cr_acct_build(lv_obj_t *page)
{
    int32_t pw = lv_obj_get_style_width(page, 0);
    lv_obj_t *bar = ui_bar(page, _("Cancelar"), A.idx < 0 ? _("Cuenta nueva") : ui_acct_name(A.idx), acct_back_cb);
    (void)bar;
    A.col = ui_col(page, pw, U.H - 96);
    lv_obj_set_y(A.col, 96);
    lv_obj_set_style_pad_row(A.col, 10, 0);
    lv_obj_t *col = A.col;
    const cr_provider_t *prov = cr_provider_key(A.cfg.provider);
    if (A.idx < 0) {
        ui_label(col, _("Proveedor"), aos_font_small, AOS_C_DIM);
        lv_obj_t *row = ui_box(col, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(row, 10, 0);
        for (int i = 0; i < CR_NPROVIDERS; i++) {
            ui_chip(row, _(CR_PROVIDERS[i].name), prov == &CR_PROVIDERS[i], provider_cb, (void *)(intptr_t)i);
        }
    }
    A.ta_name = ui_field(col, _("Tu nombre (el que ven los demás)"), A.cfg.name, "Ana Ferreyra", true);
    A.ta_email = ui_field(col, _("Dirección de correo"), A.cfg.email, "ana@example.com", true);
    lv_obj_add_event_cb(A.ta_email, email_done_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(A.ta_email, email_done_cb, LV_EVENT_READY, NULL);
    A.ta_pass = ui_field(col, prov && strcmp(prov->key, "propio") ? _("Contraseña de aplicación") : _("Contraseña"),
                         A.pass_typed ? A.pass : "",
                         A.idx >= 0 ? _("(la que está guardada)") : NULL, true);
    lv_textarea_set_password_mode(A.ta_pass, true);
    if (prov) {
        lv_obj_t *h = ui_card(col);
        lv_obj_set_style_bg_color(h, lv_color_hex(0x15233A), 0);
        ui_label_wrap(h, _(prov->help), aos_font_small, lv_color_hex(0xC8D6F0));
    }
    if (A.testing || A.result[0]) {
        lv_obj_t *r = ui_card(col);
        if (A.testing) {
            ui_label(r, _("Probando la conexión..."), aos_font_body, AOS_C_TEXT);
        } else {
            ui_label_wrap(r, A.result, aos_font_small, A.failed ? AOS_C_ORANGE : AOS_C_GREEN);
            if (A.failed) ui_btn(r, NULL, _("Guardar igual"), AOS_C_CARD2, keep_anyway_cb, NULL);
        }
    }
    ui_btn(col, AOS_SYM_CHECK, A.testing ? _("Probando...") : _("Probar y guardar"), AOS_C_ACCENT, test_cb, NULL);

    lv_obj_t *l = ui_label(col, _("Servidores"), aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 18, 0);
    server_block(col, _("Entrante (IMAP)"), A.cfg.imap_host, A.cfg.imap_port, A.cfg.imap_sec, A.cfg.imap_user, &A.ta_ih,
                 &A.ta_ip, &A.dd_is, &A.ta_iu);
    server_block(col, _("Saliente (SMTP)"), A.cfg.smtp_host, A.cfg.smtp_port, A.cfg.smtp_sec, A.cfg.smtp_user, &A.ta_sh,
                 &A.ta_sp, &A.dd_ss, &A.ta_su);
    lv_obj_set_style_pad_top(ui_label(col, _("En la placa"), aos_font_small, AOS_C_DIM), 18, 0);
    lv_obj_t *card = ui_card(col);
    A.ta_label = ui_field(card, _("Nombre de la cuenta"), A.cfg.label, _("Personal, Trabajo..."), true);
    ui_label(card, _("Color"), aos_font_small, AOS_C_DIM);
    lv_obj_t *cr = ui_box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(cr, 14, 0);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *c = ui_box(cr, 60, 60);
        lv_obj_set_style_radius(c, 30, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(COLORS[i]), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        if (COLORS[i] == A.cfg.color) {
            lv_obj_set_style_border_width(c, 5, 0);
            lv_obj_set_style_border_color(c, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    A.ta_sig = ui_field(card, _("Firma"), A.cfg.sig, _("Se agrega al final de lo que escribís"), false);
    lv_obj_set_height(A.ta_sig, 160);
    ui_switch_row(card, _("Guardar lo enviado en la carpeta Enviados"), A.cfg.save_sent, sent_cb);
    ui_label_wrap(card, _("Gmail lo guarda solo: con Gmail, apagado."), aos_font_caption, AOS_C_DIM);
    if (A.idx >= 0) {
        lv_obj_t *d = ui_btn(col, AOS_SYM_DELETE, _("Borrar la cuenta"), AOS_C_CARD2, delete_cb, NULL);
        lv_obj_set_style_pad_top(d, 0, 0);
    }
    ui_kb_attach(page, A.col);
}
