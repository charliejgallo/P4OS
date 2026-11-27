/*
 * P4OS - Correo: writing a message, and picking a file of the card (cr_ui.h).
 *
 * What is typed lives in C as it is typed (every field copies itself on
 * change), so turning the screen or a sheet over it loses nothing. A reply
 * quotes the original under the signature, keeps the conversation's
 * In-Reply-To and References, and marks the original \Answered once sent;
 * a forward carries the original's attachments. Leaving with something
 * written asks whether to keep it as a draft (APPEND to Drafts, \Draft).
 *
 * The addresses offer the contacts (cr_store.c) that start with what is
 * being typed, by how often they were written to or from.
 */
#include "cr_ui.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>

#define ATT_MAX 16

static struct {
    bool          active;
    int           acct;
    char          to[1024], cc[1024], bcc[512], subject[300];
    char         *body;
    char          irt[200];
    char         *refs;
    cr_out_att_t  att[ATT_MAX];
    int           natt;
    int           reply_folder;
    uint32_t      reply_uid;
    bool          forward;
    int           draft_folder;
    uint32_t      draft_uid;
    bool          show_cc;
    bool          sending;
    uint32_t      token, draft_token;
    uint32_t      start_hash;           /* what was there when it opened */
    lv_obj_t     *col, *ta_to, *ta_cc, *ta_bcc, *ta_subj, *ta_body, *sugg, *send;
    lv_obj_t     *sugg_for;
    cr_contact_t  sg[4];
    int           nsg;
} C;

static uint32_t content_hash(void)
{
    uint32_t h = cr_hash(C.to, strlen(C.to)) ^ cr_hash(C.subject, strlen(C.subject)) * 3;
    if (C.body) h ^= cr_hash(C.body, strlen(C.body)) * 7;
    return h ^ (uint32_t)C.natt;
}

void cr_compose_free(void)
{
    cr_free(C.body);
    cr_free(C.refs);
    for (int i = 0; i < C.natt; i++) cr_free(C.att[i].data);
    memset(&C, 0, sizeof C);
}

static int default_acct(int acct)
{
    if (acct >= 0 && acct < CR.na) return acct;
    return CR.na ? 0 : -1;
}

static void body_set(const char *s)
{
    cr_free(C.body);
    C.body = cr_strdup(s ? s : "");
}

static void sig_body(cr_buf_t *b)
{
    if (C.acct >= 0 && CR.a[C.acct].cfg.sig[0]) {
        cr_buf_str(b, "\n\n-- \n");
        cr_buf_str(b, CR.a[C.acct].cfg.sig);
    }
}

static void open_fresh(int acct)
{
    cr_compose_free();
    C.active = true;
    C.acct = default_acct(acct);
    C.reply_folder = C.draft_folder = -1;
}

void cr_compose_new(int acct, const char *to)
{
    if (!CR.na) {
        aos_ui_toast(_("Primero agregá una cuenta"), 2000);
        return;
    }
    open_fresh(acct);
    if (to) {
        /* mailto:a@b?subject=... */
        const char *q = strchr(to, '?');
        cr_ncopy(C.to, sizeof C.to, to, q ? (size_t)(q - to) : strlen(to));
        if (q && cr_istarts(q + 1, "subject=")) {
            char s[300];
            const char *v = q + 9;
            size_t k = 0;
            for (; *v && *v != '&' && k + 1 < sizeof s; v++) {
                if (*v == '%' && v[1] && v[2]) {
                    char hx[3] = {v[1], v[2], 0};
                    s[k++] = (char)strtol(hx, NULL, 16);
                    v += 2;
                } else {
                    s[k++] = *v == '+' ? ' ' : *v;
                }
            }
            s[k] = '\0';
            cr_copy(C.subject, sizeof C.subject, s);
        }
    }
    cr_buf_t b = {0};
    sig_body(&b);
    body_set(b.p);
    cr_buf_free(&b);
    C.start_hash = content_hash();
    ui_go(SCR_COMPOSE);
}

static void add_list(cr_buf_t *b, const char *list, const char *skip_self)
{
    cr_addr_t *ad = cr_alloc(sizeof(cr_addr_t) * 40);
    if (!ad) return;
    int n = cr_addr_parse(list, ad, 40);
    for (int i = 0; i < n; i++) {
        bool own = false;
        for (int k = 0; k < CR.na; k++) {
            if (!strcasecmp(ad[i].addr, CR.a[k].cfg.email)) own = true;
        }
        if (skip_self && own) continue;
        /* not twice */
        if (b->p && cr_istr(b->p, ad[i].addr)) continue;
        if (b->n) cr_buf_str(b, ", ");
        if (ad[i].name[0]) cr_buf_printf(b, "%s <%s>", ad[i].name, ad[i].addr);
        else cr_buf_str(b, ad[i].addr);
    }
    cr_free(ad);
}

static const char *prefixed(const char *pre, const char *subj, char *out, size_t n)
{
    const char *s = subj;
    while (*s == ' ') s++;
    if (cr_istarts(s, pre)) cr_copy(out, n, s);
    else snprintf(out, n, "%s %s", pre, s);
    return out;
}

void cr_compose_reply(const cr_msg_t *m, int mode)
{
    open_fresh(m->acct);
    cr_buf_t b = {0};
    if (mode != CR_FORWARD) {
        add_list(&b, m->reply_to[0] ? m->reply_to : m->from, NULL);
        cr_copy(C.to, sizeof C.to, b.p ? b.p : "");
        cr_buf_free(&b);
        if (mode == CR_REPLY_ALL) {
            cr_buf_t cc = {0};
            cr_buf_str(&cc, "");
            add_list(&cc, m->to, "self");
            add_list(&cc, m->cc, "self");
            /* the one answered is already in To */
            cr_addr_t first[1];
            if (cr_addr_parse(C.to, first, 1) && cc.p && cr_istr(cc.p, first[0].addr) && !strchr(cc.p, ',')) cc.n = 0;
            cr_copy(C.cc, sizeof C.cc, cc.n ? cc.p : "");
            cr_buf_free(&cc);
            C.show_cc = C.cc[0] != '\0';
        }
        prefixed("Re:", m->subject, C.subject, sizeof C.subject);
        cr_copy(C.irt, sizeof C.irt, m->msgid);
        cr_buf_t r = {0};
        if (m->refs[0]) cr_buf_printf(&r, "%s ", m->refs);
        else if (m->irt[0]) cr_buf_printf(&r, "%s ", m->irt);
        cr_buf_str(&r, m->msgid);
        C.refs = cr_buf_take(&r);
        C.reply_folder = m->folder;
        C.reply_uid = m->uid;
    } else {
        prefixed("Fwd:", m->subject, C.subject, sizeof C.subject);
        C.reply_folder = m->folder;
        C.reply_uid = m->uid;
        C.forward = true;
        /* the attachments go along */
        for (int i = 0; i < m->natt && C.natt < ATT_MAX; i++) {
            if (m->att[i].truncated) continue;
            size_t n = 0;
            uint8_t *d = cr_att_bytes(m, i, &n);
            if (!d) continue;
            cr_out_att_t *a = &C.att[C.natt++];
            memset(a, 0, sizeof *a);
            cr_copy(a->name, sizeof a->name, m->att[i].name);
            cr_copy(a->type, sizeof a->type, m->att[i].type);
            a->data = d;
            a->data_n = n;
            a->size = (uint32_t)n;
        }
    }
    /* the body: room to write, the signature, then the original */
    cr_buf_t body = {0};
    cr_buf_str(&body, "\n");
    sig_body(&body);
    char d[80];
    cr_date_long(d, sizeof d, m->date);
    cr_addr_t fa[1];
    char who[120] = "";
    if (cr_addr_parse(m->from, fa, 1)) cr_addr_show(who, sizeof who, &fa[0]);
    if (mode == CR_FORWARD) {
        cr_buf_printf(&body, "\n\n---------- %s ----------\n%s: %s\n%s: %s\n%s: %s\n\n", _("Mensaje reenviado"), _("De"),
                      who, _("Fecha"), d, _("Asunto"), m->subject);
        char *plain = cr_msg_plain(m, NULL);
        if (plain) cr_buf_str(&body, plain);
        cr_free(plain);
    } else {
        cr_buf_printf(&body, "\n\n%s, %s %s:\n", d, who, _("escribió"));
        char *plain = cr_msg_plain(m, "> ");
        if (plain) cr_buf_str(&body, plain);
        cr_free(plain);
    }
    body_set(body.p);
    cr_buf_free(&body);
    C.start_hash = content_hash();
    ui_go(SCR_COMPOSE);
}

void cr_compose_draft(const cr_msg_t *m)
{
    open_fresh(m->acct);
    cr_buf_t b = {0};
    add_list(&b, m->to, NULL);
    cr_copy(C.to, sizeof C.to, b.p ? b.p : "");
    cr_buf_free(&b);
    add_list(&b, m->cc, NULL);
    cr_copy(C.cc, sizeof C.cc, b.p ? b.p : "");
    cr_buf_free(&b);
    C.show_cc = C.cc[0] != '\0';
    cr_copy(C.subject, sizeof C.subject, m->subject);
    cr_copy(C.irt, sizeof C.irt, m->irt);
    if (m->refs[0]) C.refs = cr_strdup(m->refs);
    char *plain = cr_msg_plain(m, NULL);
    /* cr_msg_plain ends blocks with blank lines: one is enough at the end */
    if (plain) {
        size_t n = strlen(plain);
        while (n && plain[n - 1] == '\n') plain[--n] = '\0';
    }
    body_set(plain);
    cr_free(plain);
    C.draft_folder = m->folder;
    C.draft_uid = m->uid;
    for (int i = 0; i < m->natt && C.natt < ATT_MAX; i++) {
        size_t n = 0;
        uint8_t *d = cr_att_bytes(m, i, &n);
        if (!d) continue;
        cr_out_att_t *a = &C.att[C.natt++];
        memset(a, 0, sizeof *a);
        cr_copy(a->name, sizeof a->name, m->att[i].name);
        cr_copy(a->type, sizeof a->type, m->att[i].type);
        a->data = d;
        a->data_n = n;
        a->size = (uint32_t)n;
    }
    C.start_hash = content_hash();
    ui_go(SCR_COMPOSE);
}

/* ---- sending ----------------------------------------------------------------------------------- */

static cr_out_t *make_out(bool draft)
{
    cr_out_t *o = cr_alloc(sizeof *o);
    if (!o) return NULL;
    o->acct = C.acct;
    cr_copy(o->to, sizeof o->to, C.to);
    cr_copy(o->cc, sizeof o->cc, C.cc);
    cr_copy(o->bcc, sizeof o->bcc, C.bcc);
    cr_copy(o->subject, sizeof o->subject, C.subject);
    o->body = cr_strdup(C.body ? C.body : "");
    cr_copy(o->irt, sizeof o->irt, C.irt);
    o->refs = C.refs ? cr_strdup(C.refs) : NULL;
    if (C.natt) {
        o->att = cr_alloc(sizeof(cr_out_att_t) * (size_t)C.natt);
        if (o->att) {
            for (int i = 0; i < C.natt; i++) {
                o->att[i] = C.att[i];
                if (C.att[i].data) {
                    o->att[i].data = cr_alloc(C.att[i].data_n);
                    if (o->att[i].data) memcpy(o->att[i].data, C.att[i].data, C.att[i].data_n);
                }
            }
            o->natt = C.natt;
        }
    }
    o->draft = draft;
    o->reply_folder = C.reply_folder;
    o->reply_uid = C.reply_uid;
    o->forward = C.forward;
    o->draft_folder = C.draft_folder;
    o->draft_uid = C.draft_uid;
    return o;
}

static int count_valid(const char *list)
{
    cr_addr_t ad[40];
    int n = cr_addr_parse(list, ad, 40), ok = 0;
    for (int i = 0; i < n; i++) {
        if (cr_addr_valid(ad[i].addr)) ok++;
        else return -1;
    }
    return ok;
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    if (C.sending) return;
    int a = count_valid(C.to), b = count_valid(C.cc), c = count_valid(C.bcc);
    if (a < 0 || b < 0 || c < 0) {
        aos_ui_toast(_("Hay una dirección que no está bien escrita"), 2500);
        return;
    }
    if (a + b + c == 0) {
        aos_ui_toast(_("Falta a quién mandarlo"), 2000);
        return;
    }
    cr_out_t *o = make_out(false);
    cr_job_t *j = o ? cr_job_new(CR_J_SEND) : NULL;
    if (!j) {
        cr_out_free(o);
        return;
    }
    j->acct = C.acct;
    j->out_msg = o;
    C.token = j->token = ui_token(OWN_COMPOSE);
    C.sending = true;
    cr_job_put(j);
    ui_kb_hide();
    if (C.send) {
        lv_obj_t *l = lv_obj_get_child(C.send, -1);
        if (l) lv_label_set_text(l, _("Enviando..."));
    }
}

static void save_draft(void)
{
    cr_out_t *o = make_out(true);
    cr_job_t *j = o ? cr_job_new(CR_J_DRAFT) : NULL;
    if (!j) {
        cr_out_free(o);
        return;
    }
    j->acct = C.acct;
    j->out_msg = o;
    j->token = ui_token(OWN_COMPOSE) | 0x800000;     /* a draft: answered even after the screen went */
    cr_job_put(j);
}

void cr_compose_job(cr_job_t *j)
{
    if (j->kind == CR_J_DRAFT) {
        if (j->ok) aos_ui_toast(_("Borrador guardado"), 1500);
        else {
            char t[260];
            snprintf(t, sizeof t, "%s: %s", _("No se pudo guardar el borrador"), j->why);
            aos_ui_toast(t, 3500);
        }
        return;
    }
    if (j->token != C.token) {
        if (j->kind == CR_J_SEND && !j->ok) {
            char t[260];
            snprintf(t, sizeof t, "%s: %s", _("No se pudo enviar"), j->why);
            aos_ui_toast(t, 4000);
        }
        return;
    }
    C.sending = false;
    if (j->ok) {
        aos_ui_toast(_("Enviado"), 1500);
        if (ui_top() == SCR_COMPOSE) ui_pop();
        else cr_compose_free();
        return;
    }
    char t[300];
    const char *kind = ui_err_text(j->err_kind);
    snprintf(t, sizeof t, "%s. %s %s", _("No se pudo enviar"), kind, j->why);
    aos_ui_toast(t, 5000);
    if (ui_top() == SCR_COMPOSE) ui_rebuild();
}

/* ---- leaving ----------------------------------------------------------------------------------- */

static void leave_pick(int k, void *ud)
{
    (void)ud;
    if (k == 0) save_draft();
    if (k == 0 || k == 1) ui_pop();
}

bool cr_compose_back(void)
{
    if (ui_sheet_open()) {
        ui_sheet_close();
        return true;
    }
    if (C.sending) {
        aos_ui_toast(_("Enviando..."), 1000);
        return true;
    }
    if (content_hash() == C.start_hash) {
        ui_pop();
        return true;
    }
    static const char *items[2];
    items[0] = _("Guardar como borrador");
    static char disc[64];
    snprintf(disc, sizeof disc, "!%s", _("Descartar"));
    items[1] = disc;
    ui_sheet(_("¿Qué hacemos con lo que escribiste?"), items, 2, leave_pick, NULL);
    return true;
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    cr_compose_back();
}

/* ---- the fields -------------------------------------------------------------------------------- */

static void sugg_hide(void)
{
    if (C.sugg) {
        lv_obj_delete(C.sugg);
        C.sugg = NULL;
    }
    C.sugg_for = NULL;
}

static void sugg_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *ta = C.sugg_for;
    if (!ta || i >= C.nsg) return;
    char *dst = ta == C.ta_to ? C.to : ta == C.ta_cc ? C.cc : C.bcc;
    size_t dn = ta == C.ta_bcc ? sizeof C.bcc : sizeof C.to;
    /* the token being typed is replaced */
    char *last = strrchr(dst, ',');
    size_t keep = last ? (size_t)(last - dst) + 1 : 0;
    char rep[220];
    if (C.sg[i].name[0]) snprintf(rep, sizeof rep, "%s%s <%s>, ", keep ? " " : "", C.sg[i].name, C.sg[i].addr);
    else snprintf(rep, sizeof rep, "%s%s, ", keep ? " " : "", C.sg[i].addr);
    dst[keep] = '\0';
    size_t l = strlen(dst);
    cr_copy(dst + l, dn - l, rep);
    lv_textarea_set_text(ta, dst);
    sugg_hide();
}

static void sugg_show(lv_obj_t *ta, const char *text)
{
    sugg_hide();
    const char *last = strrchr(text, ',');
    const char *tok = last ? last + 1 : text;
    while (*tok == ' ') tok++;
    if (strlen(tok) < 2 || strchr(tok, '<')) return;
    C.nsg = cr_contacts_find(tok, C.sg, 4);
    if (!C.nsg) return;
    C.sugg_for = ta;
    C.sugg = ui_box(C.col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(C.sugg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(C.sugg, 4, 0);
    lv_obj_move_to_index(C.sugg, lv_obj_get_index(ta) + 1);
    int32_t w = lv_obj_get_style_width(U.page, 0) - 2 * AOS_UI_PAD;
    for (int i = 0; i < C.nsg; i++) {
        lv_obj_t *row = ui_box(C.sugg, lv_pct(100), 76);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, sugg_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[200];
        if (C.sg[i].name[0]) snprintf(t, sizeof t, "%s  <%s>", C.sg[i].name, C.sg[i].addr);
        else cr_copy(t, sizeof t, C.sg[i].addr);
        char safe[220];
        aos_text_safe(safe, sizeof safe, t);
        lv_obj_t *l = ui_label_dots(row, safe, aos_font_small, AOS_C_TEXT, w - 40);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 18, 0);
    }
}

static void field_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    const char *t = lv_textarea_get_text(ta);
    if (ta == C.ta_to) cr_copy(C.to, sizeof C.to, t);
    else if (ta == C.ta_cc) cr_copy(C.cc, sizeof C.cc, t);
    else if (ta == C.ta_bcc) cr_copy(C.bcc, sizeof C.bcc, t);
    else if (ta == C.ta_subj) cr_copy(C.subject, sizeof C.subject, t);
    else if (ta == C.ta_body) body_set(t);
    if (ta == C.ta_to || ta == C.ta_cc || ta == C.ta_bcc) sugg_show(ta, t);
}

static void cc_cb(lv_event_t *e)
{
    (void)e;
    C.show_cc = true;
    ui_rebuild();
}

static void acct_cb(lv_event_t *e)
{
    C.acct = (int)(intptr_t)lv_event_get_user_data(e);
    /* the signature follows the account, if the body is only that */
    ui_rebuild();
}

static void att_remove_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= C.natt) return;
    cr_free(C.att[i].data);
    memmove(&C.att[i], &C.att[i + 1], sizeof C.att[0] * (size_t)(C.natt - i - 1));
    C.natt--;
    ui_rebuild();
}

static void picked(const char *path)
{
    if (C.natt >= ATT_MAX) {
        aos_ui_toast(_("No entran más adjuntos"), 2000);
        return;
    }
    uint32_t sz = cr_file_size(path);
    uint32_t total = sz;
    for (int i = 0; i < C.natt; i++) total += C.att[i].size;
    if (total > 20u * 1024 * 1024) {
        aos_ui_toast(_("Es demasiado para un correo (20 MB en total)"), 2500);
        return;
    }
    cr_out_att_t *a = &C.att[C.natt++];
    memset(a, 0, sizeof *a);
    cr_copy(a->path, sizeof a->path, path);
    const char *sl = strrchr(path, '/');
    cr_copy(a->name, sizeof a->name, sl ? sl + 1 : path);
    cr_copy(a->type, sizeof a->type, cr_mime_type_for(a->name));
    a->size = sz;
}

static void attach_cb(lv_event_t *e)
{
    (void)e;
    cr_pick_file(picked);
}

void cr_compose_build(lv_obj_t *page)
{
    if (!C.active) {
        ui_label(page, "", aos_font_body, AOS_C_DIM);
        return;
    }
    int32_t pw = lv_obj_get_style_width(page, 0);
    lv_obj_t *bar = ui_bar(page, _("Cancelar"), C.reply_uid && !C.forward ? _("Respuesta") :
                                                C.forward ? _("Reenviar") : _("Mensaje nuevo"), cancel_cb);
    lv_obj_t *r = ui_bar_right(bar);
    C.send = ui_btn(r, AOS_SYM_SEND, C.sending ? _("Enviando...") : _("Enviar"), AOS_C_ACCENT, send_cb, NULL);
    C.col = ui_col(page, pw, U.H - 96);
    lv_obj_set_y(C.col, 96);
    lv_obj_set_style_pad_row(C.col, 8, 0);
    C.sugg = NULL;
    if (CR.na > 1) {
        ui_label(C.col, _("De"), aos_font_small, AOS_C_DIM);
        lv_obj_t *row = ui_box(C.col, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(row, 10, 0);
        for (int i = 0; i < CR.na; i++) ui_chip(row, CR.a[i].cfg.email, i == C.acct, acct_cb, (void *)(intptr_t)i);
    }
    C.ta_to = ui_field(C.col, _("Para"), C.to, _("nombre o dirección"), false);
    lv_obj_add_event_cb(C.ta_to, field_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (C.show_cc || C.cc[0] || C.bcc[0]) {
        C.ta_cc = ui_field(C.col, _("Cc"), C.cc, NULL, false);
        lv_obj_add_event_cb(C.ta_cc, field_cb, LV_EVENT_VALUE_CHANGED, NULL);
        C.ta_bcc = ui_field(C.col, _("Cco (no lo ven los demás)"), C.bcc, NULL, false);
        lv_obj_add_event_cb(C.ta_bcc, field_cb, LV_EVENT_VALUE_CHANGED, NULL);
    } else {
        C.ta_cc = C.ta_bcc = NULL;
        lv_obj_t *b = ui_chip(C.col, _("Cc y Cco"), false, cc_cb, NULL);
        (void)b;
    }
    C.ta_subj = ui_field(C.col, _("Asunto"), C.subject, NULL, true);
    lv_obj_add_event_cb(C.ta_subj, field_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* the attachments */
    lv_obj_t *ar = ui_box(C.col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(ar, 10, 0);
    lv_obj_set_style_pad_top(ar, 6, 0);
    for (int i = 0; i < C.natt; i++) {
        char t[180], sz[24];
        ui_size(sz, sizeof sz, C.att[i].size);
        snprintf(t, sizeof t, "%.60s  %s  " , C.att[i].name, sz);
        lv_obj_t *chip = ui_chip(ar, t, false, att_remove_cb, (void *)(intptr_t)i);
        lv_obj_t *x = ui_label(chip, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(x, LV_ALIGN_RIGHT_MID, 8, 0);
    }
    ui_btn(ar, AOS_SYM_FILE_OUTLINE, _("Adjuntar"), AOS_C_CARD2, attach_cb, NULL);
    C.ta_body = ui_field(C.col, NULL, C.body ? C.body : "", _("Escribí acá"), false);
    lv_obj_set_height(C.ta_body, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(C.ta_body, U.land ? 260 : 460, 0);
    lv_obj_add_event_cb(C.ta_body, field_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (!C.reply_uid || C.forward) lv_textarea_set_cursor_pos(C.ta_body, 0);
    else lv_textarea_set_cursor_pos(C.ta_body, 0);
    ui_kb_attach(page, C.col);
}

/* ---- the file picker --------------------------------------------------------------------------- */

typedef struct {
    char     name[128];
    bool     dir;
    uint32_t size;
} pe_t;

static struct {
    cr_pick_file_cb_t cb;
    char              dir[256];
    char              root[128];
    pe_t             *e;
    int               n, cap;
} P;

void cr_pick_file(cr_pick_file_cb_t cb)
{
    P.cb = cb;
    const char *sd = aos_hal_path_sd_root();
    if (!sd) {
        aos_ui_toast(_("No hay tarjeta"), 2000);
        return;
    }
    cr_copy(P.root, sizeof P.root, sd);
    if (!P.dir[0] || strncmp(P.dir, P.root, strlen(P.root))) cr_copy(P.dir, sizeof P.dir, sd);
    ui_go(SCR_PICK);
}

static bool pick_scan(const aos_dir_entry_t *e, void *ctx)
{
    (void)ctx;
    if (e->name[0] == '.') return true;
    if (P.n == P.cap) {
        int c = P.cap ? P.cap * 2 : 64;
        pe_t *ne = cr_realloc(P.e, sizeof(pe_t) * (size_t)c);
        if (!ne) return false;
        P.e = ne;
        P.cap = c;
    }
    pe_t *p = &P.e[P.n++];
    cr_copy(p->name, sizeof p->name, e->name);
    p->dir = e->dir;
    p->size = e->size;
    return P.n < 2000;
}

static int cmp_pe(const void *a, const void *b)
{
    const pe_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool pick_up(void)
{
    if (!strcmp(P.dir, P.root)) return false;
    char *sl = strrchr(P.dir, '/');
    if (sl && sl > P.dir) *sl = '\0';
    ui_rebuild();
    return true;
}

bool cr_pick_back(void)
{
    return pick_up();
}

static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0) {
        pick_up();
        return;
    }
    if (i >= P.n) return;
    char path[400];
    snprintf(path, sizeof path, "%s/%s", P.dir, P.e[i].name);
    if (P.e[i].dir) {
        cr_copy(P.dir, sizeof P.dir, path);
        ui_rebuild();
        return;
    }
    cr_pick_file_cb_t cb = P.cb;
    ui_pop();
    if (cb) cb(path);
    if (ui_top() == SCR_COMPOSE) ui_rebuild();
}

static void pick_cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_pop();
}

void cr_pick_build(lv_obj_t *page)
{
    int32_t pw = lv_obj_get_style_width(page, 0);
    const char *base = strrchr(P.dir, '/');
    ui_bar(page, _("Cancelar"), strcmp(P.dir, P.root) && base ? base + 1 : _("La tarjeta"), pick_cancel_cb);
    lv_obj_t *col = ui_col(page, pw, U.H - 96);
    lv_obj_set_y(col, 96);
    lv_obj_set_style_pad_row(col, 4, 0);
    P.n = 0;
    aos_hal_dir_scan(P.dir, pick_scan, NULL);
    if (P.n > 1) qsort(P.e, (size_t)P.n, sizeof(pe_t), cmp_pe);
    int32_t w = pw - 2 * AOS_UI_PAD;
    if (strcmp(P.dir, P.root)) {
        lv_obj_t *row = ui_box(col, lv_pct(100), 80);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
        lv_obj_t *g = ui_label(row, AOS_SYM_ARROW_UP, &aos_sym_28, AOS_C_ACCENT);
        lv_obj_align(g, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_t *l = ui_label(row, _("Subir"), aos_font_body, AOS_C_ACCENT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 64, 0);
    }
    if (!P.n) ui_label(col, _("La carpeta está vacía"), aos_font_body, AOS_C_DIM);
    for (int i = 0; i < P.n && i < 400; i++) {
        lv_obj_t *row = ui_box(col, lv_pct(100), 80);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *g = ui_label(row, P.e[i].dir ? AOS_SYM_FOLDER : AOS_SYM_FILE_OUTLINE, &aos_sym_28,
                               P.e[i].dir ? AOS_C_ACCENT : AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_LEFT_MID, 16, 0);
        char safe[160];
        aos_text_safe(safe, sizeof safe, P.e[i].name);
        lv_obj_t *l = ui_label_dots(row, safe, aos_font_body, AOS_C_TEXT, w - 220);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 64, 0);
        if (!P.e[i].dir) {
            char sz[24];
            ui_size(sz, sizeof sz, P.e[i].size);
            lv_obj_t *s = ui_label(row, sz, aos_font_caption, AOS_C_DIM);
            lv_obj_align(s, LV_ALIGN_RIGHT_MID, -16, 0);
        }
    }
}
