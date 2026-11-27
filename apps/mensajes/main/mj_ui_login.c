/*
 * P4OS - Mensajes: signing in.
 *
 * One screen that changes with mj_S.auth: the app's keys (api_id and
 * api_hash, which each person gets from my.telegram.org and which can also
 * come from the portal's page), the number, the code, the two-step
 * password. The network thread does the talking (mj_api.c); this only asks
 * and shows the state and the errors in words.
 */
#include "mj_ui.h"

static int s_state;

/* what the screen shows of the store, copied under its lock */
static struct {
    int auth;
    char err[96], how[24], phone[24], hint[64];
} L;

static void copy_store(void)
{
    mj_store_lock();
    L.auth = mj_S.auth;
    snprintf(L.err, sizeof L.err, "%s", mj_S.auth_err);
    snprintf(L.how, sizeof L.how, "%s", mj_S.code_how);
    snprintf(L.phone, sizeof L.phone, "%s", mj_S.phone);
    snprintf(L.hint, sizeof L.hint, "%s", mj_S.pw_hint);
    mj_store_unlock();
}

static const char *err_words(const char *e)
{
    if (!e || !*e) return "";
    if (!strcmp(e, "PHONE_NUMBER_INVALID")) return _("Ese número no es válido: va con el código de país.");
    if (!strcmp(e, "API_ID_INVALID") || !strcmp(e, "API_ID_PUBLISHED_FLOOD"))
        return _("El api_id o el api_hash no son válidos.");
    if (!strcmp(e, "PHONE_CODE_INVALID")) return _("El código no es el correcto.");
    if (!strcmp(e, "PHONE_CODE_EXPIRED")) return _("El código venció: pedí otro.");
    if (!strcmp(e, "PHONE_CODE_EMPTY")) return _("Falta el código.");
    if (!strcmp(e, "PASSWORD_HASH_INVALID")) return _("La contraseña no es la correcta.");
    if (!strcmp(e, "PHONE_NUMBER_BANNED")) return _("Telegram bloqueó ese número.");
    if (!strcmp(e, "PHONE_NUMBER_FLOOD") || !strncmp(e, "FLOOD_WAIT_", 11))
        return _("Demasiados intentos: esperá un rato y probá de nuevo.");
    if (!strcmp(e, "EMAIL_SETUP_REQUIRED"))
        return _("Telegram pide configurar un correo desde la app oficial antes de entrar acá.");
    if (!strcmp(e, "UNKNOWN_ALGO")) return _("La contraseña usa un método que la placa no conoce.");
    if (!strcmp(e, "TIMEOUT")) return _("Telegram no contestó. ¿Hay red?");
    return e;
}

static void field(lv_obj_t *col, lv_obj_t **ta, const char *placeholder, bool password)
{
    *ta = lv_textarea_create(col);
    lv_textarea_set_one_line(*ta, true);
    lv_textarea_set_placeholder_text(*ta, placeholder);
    lv_obj_set_width(*ta, lv_pct(100));
    lv_obj_set_style_text_font(*ta, aos_font_body, 0);
    lv_obj_set_style_text_color(*ta, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_color(*ta, AOS_C_CARD2, 0);
    lv_obj_set_style_border_width(*ta, 0, 0);
    lv_obj_set_style_radius(*ta, 18, 0);
    lv_obj_set_style_pad_all(*ta, 18, 0);
    if (password) lv_textarea_set_password_mode(*ta, true);
}

static void go(void);

static void ready_cb(lv_event_t *e)
{
    (void)e;
    if (U.kb_ta == U.ta1 && U.ta2) {
        /* the first of two fields: on to the second */
        mj_kb_show(U.ta2, LV_KEYBOARD_MODE_TEXT_LOWER, ready_cb);
        return;
    }
    go();
}

static void ta_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_mode_t mode = (s_state == MJ_AUTH_PHONE || s_state == MJ_AUTH_CODE || (s_state == MJ_AUTH_NEED_API && ta == U.ta1))
                                  ? LV_KEYBOARD_MODE_NUMBER
                                  : LV_KEYBOARD_MODE_TEXT_LOWER;
    mj_kb_show(ta, mode, ready_cb);
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    go();
}

static void digits(const char *in, char *out, size_t n, bool plus)
{
    size_t o = 0;
    if (plus && o + 1 < n) out[o++] = '+';
    for (; *in && o + 1 < n; in++)
        if (*in >= '0' && *in <= '9') out[o++] = *in;
    out[o] = 0;
}

static void go(void)
{
    const char *a = U.ta1 ? lv_textarea_get_text(U.ta1) : "";
    switch (s_state) {
    case MJ_AUTH_NEED_API: {
        const char *h = U.ta2 ? lv_textarea_get_text(U.ta2) : "";
        int id = atoi(a);
        size_t hl = strlen(h);
        bool hex = hl == 32;
        for (size_t i = 0; i < hl && hex; i++)
            hex = (h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f');
        if (id <= 0 || !hex) {
            lv_label_set_text(U.err_lbl, _("El api_id es un número y el api_hash tiene 32 letras y números (0-9, a-f)."));
            return;
        }
        mj_kb_hide();
        mj_api_set_app(id, h);
        break;
    }
    case MJ_AUTH_PHONE: {
        char p[24];
        digits(a, p, sizeof p, true);
        if (strlen(p) < 8) {
            lv_label_set_text(U.err_lbl, _("Ese número no es válido: va con el código de país."));
            return;
        }
        mj_kb_hide();
        mj_api_login_phone(p);
        break;
    }
    case MJ_AUTH_CODE: {
        char c[16];
        digits(a, c, sizeof c, false);
        if (!c[0]) return;
        mj_kb_hide();
        mj_api_login_code(c);
        break;
    }
    case MJ_AUTH_PASSWORD:
        if (!a[0]) return;
        mj_kb_hide();
        mj_api_login_password(a);
        lv_textarea_set_text(U.ta1, "");
        break;
    case MJ_AUTH_SIGNUP:
        mj_api_login_restart();
        break;
    }
}

static void other_cb(lv_event_t *e)
{
    (void)e;
    mj_kb_hide();
    if (s_state == MJ_AUTH_PHONE) {
        /* back to the keys */
        L.err[0] = 0;
        mj_login_build(U.root, MJ_AUTH_NEED_API);
        return;
    }
    mj_api_login_restart();
}

void mj_login_build(lv_obj_t *parent, int state)
{
    mj_kb_hide();
    mj_sheet_close();
    if (U.login) lv_obj_delete(U.login);
    U.ta1 = U.ta2 = U.err_lbl = U.go_btn = NULL;
    s_state = state;
    U.login_state = state;
    lv_obj_t *o = mj_box(parent, U.W, U.H);
    lv_obj_set_style_bg_color(o, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    U.login = o;
    int32_t cw = U.land ? 640 : U.W - 2 * 48;
    lv_obj_t *col = mj_box(o, cw, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, U.land ? 24 : 56);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 18, 0);

    if (!U.land || state == MJ_AUTH_BUSY || state == MJ_AUTH_STARTING) {
        lv_obj_t *c = mj_box(col, 132, 132);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c, AOS_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_t *g = mj_label(c, AOS_SYM_SEND, &aos_sym_72, AOS_C_TEXT);
        lv_obj_center(g);
    }
    const char *title = "", *text = "", *ph1 = NULL, *ph2 = NULL, *btn = NULL, *other = NULL;
    char textbuf[300];
    switch (state) {
    case MJ_AUTH_STARTING:
    case MJ_AUTH_BUSY:
        title = _("Mensajes");
        text = _("Hablando con Telegram…");
        break;
    case MJ_AUTH_NEED_API:
        title = _("Mensajes para Telegram");
        text = _("Para entrar con tu cuenta, la placa necesita un api_id y un api_hash tuyos. Se sacan gratis en "
                 "my.telegram.org, en «API development tools». También se cargan desde la página Mensajes del "
                 "portal.");
        ph1 = _("api_id");
        ph2 = _("api_hash");
        btn = _("Guardar");
        break;
    case MJ_AUTH_PHONE:
        title = _("Tu número");
        text = _("Con el código de país: +54 9 11 y el resto, por ejemplo. Te va a llegar un código.");
        ph1 = "+54 9 11 ...";
        btn = _("Seguir");
        other = _("Cambiar el api_id");
        break;
    case MJ_AUTH_CODE: {
        title = _("El código");
        const char *how = L.how;
        const char *w = !strcmp(how, "app")     ? _("Telegram te lo mandó a la app, en tu otro dispositivo.")
                        : !strcmp(how, "sms")   ? _("Te llega por SMS.")
                        : !strcmp(how, "call")  ? _("Te lo dicen en una llamada.")
                        : !strcmp(how, "email") ? _("Te llega a tu correo.")
                                                : _("Telegram te mandó un código.");
        snprintf(textbuf, sizeof textbuf, "%s\n%s", L.phone, w);
        text = textbuf;
        ph1 = "12345";
        btn = _("Entrar");
        other = _("Otro número");
        break;
    }
    case MJ_AUTH_PASSWORD:
        title = _("Contraseña en dos pasos");
        if (L.hint[0]) {
            snprintf(textbuf, sizeof textbuf, _("Tu cuenta tiene una contraseña además del código. Pista: %s"),
                     L.hint);
            text = textbuf;
        } else {
            text = _("Tu cuenta tiene una contraseña además del código.");
        }
        ph1 = _("Contraseña");
        btn = _("Entrar");
        other = _("Otro número");
        break;
    case MJ_AUTH_SIGNUP:
        title = _("Ese número no tiene cuenta");
        text = _("Creala desde la app oficial de Telegram y volvé: la placa entra a cuentas que ya existen.");
        btn = _("Otro número");
        break;
    }
    lv_obj_t *t = mj_label(col, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *d = mj_label(col, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(d, lv_pct(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    if (state == MJ_AUTH_BUSY || state == MJ_AUTH_STARTING) {
        lv_obj_t *sp = lv_spinner_create(col);
        lv_obj_set_size(sp, 64, 64);
        lv_obj_set_style_arc_color(sp, AOS_C_ACCENT, LV_PART_INDICATOR);
        return;
    }
    if (ph1) {
        field(col, &U.ta1, ph1, state == MJ_AUTH_PASSWORD);
        lv_obj_add_event_cb(U.ta1, ta_cb, LV_EVENT_CLICKED, NULL);
    }
    if (ph2) {
        field(col, &U.ta2, ph2, false);
        lv_obj_add_event_cb(U.ta2, ta_cb, LV_EVENT_CLICKED, NULL);
    }
    if (state == MJ_AUTH_PHONE && L.phone[0]) lv_textarea_set_text(U.ta1, L.phone);
    U.err_lbl = mj_label(col, err_words(L.err), aos_font_small, AOS_C_RED);
    lv_obj_set_width(U.err_lbl, lv_pct(100));
    lv_label_set_long_mode(U.err_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(U.err_lbl, LV_TEXT_ALIGN_CENTER, 0);
    if (btn) U.go_btn = mj_pill_btn(col, btn, AOS_C_ACCENT, go_cb, NULL);
    if (other) {
        lv_obj_t *l = mj_label(col, other, aos_font_small, AOS_C_ACCENT);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(l, 16);
        lv_obj_add_event_cb(l, other_cb, LV_EVENT_CLICKED, NULL);
    }
    /* the keyboard up at once on the first field (not lying down: no room) */
    if (U.ta1 && !U.land) {
        lv_keyboard_mode_t mode = state == MJ_AUTH_PASSWORD || state == MJ_AUTH_NEED_API ? LV_KEYBOARD_MODE_TEXT_LOWER
                                                                                          : LV_KEYBOARD_MODE_NUMBER;
        if (state == MJ_AUTH_NEED_API) mode = LV_KEYBOARD_MODE_NUMBER;
        mj_kb_show(U.ta1, mode, ready_cb);
    }
}

void mj_login_update(void)
{
    copy_store();
    if (L.auth != s_state || !U.login) {
        mj_login_build(U.root, L.auth);
    } else if (U.err_lbl) {
        lv_label_set_text(U.err_lbl, err_words(L.err));
    }
}
