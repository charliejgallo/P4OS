/*
 * P4OS - Simon (from AmoledOS)
 *
 * The memory game of 1978: the machine plays a sequence of colours and you
 * have to repeat it. Each success adds a step and takes a few milliseconds
 * off.
 *
 * The whole game is a state machine driven by a single 40 ms lv_timer. There
 * are deliberately no LVGL animations: an opacity animation per panel would do
 * the job just as well, but the sound and the light have to land TOGETHER and
 * with a timer of our own both come out of the same frame. The note's duration
 * is the same as the light's, and the silence between notes is a third of
 * that.
 *
 * P4OS: on the 5" panel the four pads are 320 px each (they were 156), the
 * eye in the middle carries the step in 96 px digits, and two cards on top
 * show the current step and the record. It lives in both orientations: lying
 * down the board goes left and the cards to a column on the right. A turn of
 * the screen goes through resize(), which only moves the objects, so a game
 * in progress survives it - even mid-sequence, because the timer never
 * stops.
 */
#include "aos_app.h"
#include "aos_theme.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

#define MAX_STEPS       99

#define PAD_GAP         24
#define PAD_RADIUS      64
#define CARD_H          132

/* The four notes of the original Simon (Milton Bradley, 1978): an E major
 * chord in second inversion, chosen so any combination sounds good. The fifth
 * -the low one- is the error note. */
static const uint16_t TONE[4]  = { 415, 310, 252, 209 };
static const uint32_t BRIGHT[4] = { 0x30D158, 0xFF453A, 0xFFD60A, 0x0A84FF };
static const uint32_t DIM[4]    = { 0x0F4A20, 0x581712, 0x584A04, 0x06325F };

typedef enum {
    ST_IDLE = 0,        /* waiting for a touch to start      */
    ST_SHOW,            /* the machine plays the sequence    */
    ST_WAIT,            /* the player's turn                 */
    ST_OVER,            /* failed; a touch starts over       */
} state_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *title;
    lv_obj_t *card[2];
    lv_obj_t *card_cap[2];
    lv_obj_t *card_val[2];
    lv_obj_t *pad[4];
    lv_obj_t *eye;
    lv_obj_t *lbl_eye;
    lv_obj_t *lbl_foot;
    lv_timer_t *timer;

    uint32_t pad_color[4];      /* the last thing written, so as not to invalidate for nothing */
    bool     eye_digits;        /* the eye's label is on the digits-only font */

    uint8_t seq[MAX_STEPS];
    int     len;                /* steps of the current sequence */
    int     pos;               /* in ST_SHOW: which one is playing; in ST_WAIT: how many have gone */

    state_t  state;
    uint32_t next_ms;           /* when the current step expires */
    bool     lit;               /* in ST_SHOW: the panel is lit */
    int      flash;             /* panel lit by the player, -1 if none */
    uint32_t flash_end_ms;

    int  best;
    bool pending_start;         /* left by the physical button */

    uint32_t rng;
} simon_t;

static simon_t s_simon;

/* -------------------------------------------------------------------------- */

static uint32_t rnd(void)
{
    uint32_t x = s_simon.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_simon.rng = x;
    return x;
}

/* The sequence speeds up with the level and bottoms out at 220 ms, which is
 * already the limit of what can be followed. */
static uint32_t note_ms(void)
{
    int ms = 520 - s_simon.len * 16;
    return (uint32_t)(ms < 220 ? 220 : ms);
}

/* A lit pad also glows: the shadow is the pad's own colour. It only changes
 * at note boundaries, so the cost of drawing a blurred shadow is paid a few
 * times a second at most. */
static void set_pad(int i, bool on)
{
    uint32_t color = on ? BRIGHT[i] : DIM[i];
    if (color == s_simon.pad_color[i]) {
        return;
    }
    lv_obj_set_style_bg_color(s_simon.pad[i], lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(s_simon.pad[i], on ? LV_OPA_70 : LV_OPA_TRANSP, 0);
    s_simon.pad_color[i] = color;
}

static void all_pads_off(void)
{
    for (int i = 0; i < 4; i++) {
        set_pad(i, false);
    }
}

/* Numbers go on the 96 px digits font; words (TOCA) on the large one, which
 * has letters and accents. */
static void set_eye(const char *text, lv_color_t color)
{
    bool digits = text[0] >= '0' && text[0] <= '9';
    if (digits != s_simon.eye_digits) {
        lv_obj_set_style_text_font(s_simon.lbl_eye,
                                   digits ? &aos_inter_num_96 : aos_font_large, 0);
        s_simon.eye_digits = digits;
    }
    lv_label_set_text(s_simon.lbl_eye, text);
    lv_obj_set_style_text_color(s_simon.lbl_eye, color, 0);
}

static void set_foot(const char *text, lv_color_t color)
{
    lv_label_set_text(s_simon.lbl_foot, text);
    lv_obj_set_style_text_color(s_simon.lbl_foot, color, 0);
}

static void refresh_cards(void)
{
    char buf[16];
    int step = s_simon.state == ST_IDLE ? 0 : s_simon.len;
    if (step > 0) {
        snprintf(buf, sizeof(buf), "%d", step);
    } else {
        snprintf(buf, sizeof(buf), "%s", "-");
    }
    lv_label_set_text(s_simon.card_val[0], buf);

    if (s_simon.best > 0) {
        snprintf(buf, sizeof(buf), "%d", s_simon.best);
    } else {
        snprintf(buf, sizeof(buf), "%s", "-");
    }
    lv_label_set_text(s_simon.card_val[1], buf);
}

/* -------------------------------------------------------------------------- */
/* Transitions                                                                 */

static void go_idle(void)
{
    s_simon.state = ST_IDLE;
    s_simon.len   = 0;
    s_simon.pos   = 0;
    all_pads_off();
    set_eye(_("TOCA"), AOS_C_TEXT);
    set_foot(_("Tocá el centro para empezar"), AOS_C_DIM);
    refresh_cards();
}

/* Starts a new game. It is separate from grow_and_show() because the bug the
 * first test uncovered was precisely that: touching after losing called
 * grow_and_show() without zeroing the sequence, so the "new" game started with
 * the old one inside it and with the inherited length. */
static void restart(void)
{
    s_simon.len = 0;
    s_simon.pos = 0;
    all_pads_off();
    s_simon.flash = -1;
}

/* One more step and the machine plays it from the beginning. */
static void grow_and_show(void)
{
    if (s_simon.len < MAX_STEPS) {
        s_simon.seq[s_simon.len++] = (uint8_t)(rnd() & 3u);
    }
    s_simon.state   = ST_SHOW;
    s_simon.pos     = 0;
    s_simon.lit     = false;
    /* Half a second of air before starting: without that the first colour
     * collides with the player's touch and it looks as if the machine had
     * jumped the gun. */
    s_simon.next_ms = lv_tick_get() + 500;

    /* 16 and not 8: for GCC a %d takes up to 11 characters and it does not
     * care that the sequence is capped at 99. See HANDOFF-APPS.md. */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_simon.len);
    set_eye(buf, AOS_C_DIM);
    set_foot(_("Mirá y escuchá"), AOS_C_DIM);
    refresh_cards();
}

static void game_over(void)
{
    s_simon.state = ST_OVER;
    all_pads_off();
    s_simon.flash = -1;

    int score = s_simon.len - 1;        /* the step that failed does not count */
    bool record = score > s_simon.best;
    if (record) {
        s_simon.best = score;
        aos_hal_pref_set_i32("simon_best", score);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", score);
    set_eye(buf, AOS_C_RED);

    char foot[64];
    if (record && score > 0) {
        snprintf(foot, sizeof(foot), _("¡Nuevo récord! %d pasos"), score);
        set_foot(foot, AOS_C_YELLOW);
    } else {
        snprintf(foot, sizeof(foot), _("Fallaste en el paso %d"), score + 1);
        set_foot(foot, AOS_C_RED);
    }
    refresh_cards();

    aos_hal_beep(110, 420);
    s_simon.next_ms = lv_tick_get() + 900;
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */

/* Start, if it is time to start. Called by the eye in the middle, the physical
 * button and any panel when a game is not under way. */
static void try_start(void)
{
    if (s_simon.state != ST_IDLE && s_simon.state != ST_OVER) {
        return;
    }
    /* After losing, the board stays deaf for a moment: otherwise the same
     * clumsy finger that failed starts the next game and you do not even get
     * to see what you finished on. */
    if (s_simon.state == ST_OVER &&
        (int32_t)(lv_tick_get() - s_simon.next_ms) < 0) {
        return;
    }
    restart();
    grow_and_show();
}

static void press_pad(int i)
{
    if (s_simon.state == ST_IDLE || s_simon.state == ST_OVER) {
        try_start();
        return;
    }
    if (s_simon.state != ST_WAIT) {
        return;         /* while the machine plays, the board does not listen */
    }

    uint32_t dur = note_ms();
    set_pad(i, true);
    s_simon.flash        = i;
    s_simon.flash_end_ms = lv_tick_get() + dur;
    aos_hal_beep(TONE[i], (int)dur);

    if (s_simon.seq[s_simon.pos] != (uint8_t)i) {
        game_over();
        /* the wrong panel stays lit for as long as the note lasts: you can see
         * which the mistake was */
        set_pad(i, true);
        s_simon.flash        = i;
        s_simon.flash_end_ms = lv_tick_get() + dur;
        return;
    }

    s_simon.pos++;
    if (s_simon.pos >= s_simon.len) {
        /* the round is complete: it waits for the note to finish and carries
         * on */
        s_simon.state   = ST_SHOW;
        s_simon.pos     = -1;       /* -1 = the sequence has not grown yet */
        s_simon.next_ms = lv_tick_get() + dur + 320;
        s_simon.lit     = false;
        set_foot(_("¡Bien!"), AOS_C_GREEN);
    }
}

/* PRESSED and not CLICKED: the pad answers the moment the finger lands, as
 * the real one does, and a finger that slides a little off does not lose the
 * note. */
static void pad_cb(lv_event_t *event)
{
    press_pad((int)(intptr_t)lv_event_get_user_data(event));
}

static void eye_cb(lv_event_t *event)
{
    (void)event;
    try_start();
}

/* -------------------------------------------------------------------------- */

static void frame_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();

    if (s_simon.pending_start) {
        s_simon.pending_start = false;
        try_start();
    }

    /* switch off the panel the player lit */
    if (s_simon.flash >= 0 && (int32_t)(now - s_simon.flash_end_ms) >= 0) {
        set_pad(s_simon.flash, false);
        s_simon.flash = -1;
    }

    if (s_simon.state != ST_SHOW || (int32_t)(now - s_simon.next_ms) < 0) {
        return;
    }

    /* pos == -1 is the pause that follows completing a round */
    if (s_simon.pos < 0) {
        grow_and_show();
        return;
    }

    uint32_t dur = note_ms();
    if (!s_simon.lit) {
        int i = s_simon.seq[s_simon.pos];
        set_pad(i, true);
        aos_hal_beep(TONE[i], (int)dur);
        s_simon.lit     = true;
        s_simon.next_ms = now + dur;
        return;
    }

    set_pad(s_simon.seq[s_simon.pos], false);
    s_simon.lit = false;
    s_simon.pos++;

    if (s_simon.pos >= s_simon.len) {
        s_simon.state = ST_WAIT;
        s_simon.pos   = 0;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_simon.len);
        set_eye(buf, AOS_C_TEXT);
        set_foot(_("Tu turno"), AOS_C_TEXT);
        return;
    }
    s_simon.next_ms = now + dur / 3;
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */

/* Everything is positioned here from the size of the root, so the same code
 * serves create() and a turn of the screen.
 *
 *   portrait   title, the two cards, the board, the message under it
 *   landscape  the board filling the height on the left; title, cards and
 *              message in a column on the right */
static void layout(int32_t w, int32_t h)
{
    int32_t pad, bx, by;                /* pad side, board origin */
    int32_t cx, cy, cw;                 /* the column of text: x, y, width */

    if (w > h) {
        pad = (h - 2 * 40 - PAD_GAP) / 2;
        int32_t board = 2 * pad + PAD_GAP;
        bx = 64;
        by = (h - board) / 2;
        cx = bx + board + 64;
        cw = w - cx - 64;
        /* title, cards and message: ~340 px, centred on the height */
        cy = (h - 340) / 2;

        lv_obj_set_width(s_simon.title, cw);
        lv_obj_set_pos(s_simon.title, cx, cy);

        int32_t card_w = (cw - PAD_GAP) / 2;
        for (int i = 0; i < 2; i++) {
            lv_obj_set_size(s_simon.card[i], card_w, CARD_H);
            lv_obj_set_pos(s_simon.card[i], cx + i * (card_w + PAD_GAP), cy + 80);
        }
        lv_obj_set_width(s_simon.lbl_foot, cw);
        lv_obj_set_pos(s_simon.lbl_foot, cx, cy + 80 + CARD_H + 48);
    } else {
        pad = (w - 2 * 28 - PAD_GAP) / 2;
        if (pad > 330) {
            pad = 330;
        }
        int32_t board = 2 * pad + PAD_GAP;
        bx = (w - board) / 2;
        cw = board;
        cx = bx;

        /* title + cards take 262 px from the top; the board and the message
         * under it are centred in what is left */
        int32_t top  = 64 + 56 + CARD_H;
        int32_t foot = 40 + 80;
        by = top + 40 + (h - top - 40 - board - foot) / 2;
        if (by < top + 32) {
            by = top + 32;
        }

        lv_obj_set_width(s_simon.title, cw);
        lv_obj_set_pos(s_simon.title, cx, 56);

        int32_t card_w = (cw - PAD_GAP) / 2;
        for (int i = 0; i < 2; i++) {
            lv_obj_set_size(s_simon.card[i], card_w, CARD_H);
            lv_obj_set_pos(s_simon.card[i], cx + i * (card_w + PAD_GAP), 120);
        }
        lv_obj_set_width(s_simon.lbl_foot, cw);
        lv_obj_set_pos(s_simon.lbl_foot, cx, by + board + 44);
    }

    for (int i = 0; i < 4; i++) {
        lv_obj_set_size(s_simon.pad[i], pad, pad);
        lv_obj_set_pos(s_simon.pad[i], bx + (i & 1) * (pad + PAD_GAP),
                                       by + (i >> 1) * (pad + PAD_GAP));
    }

    int32_t eye = pad * 3 / 4;
    lv_obj_set_size(s_simon.eye, eye, eye);
    lv_obj_set_pos(s_simon.eye, bx + pad + PAD_GAP / 2 - eye / 2,
                                by + pad + PAD_GAP / 2 - eye / 2);
    lv_obj_set_size(s_simon.lbl_eye, eye - 16, 120);
    lv_obj_center(s_simon.lbl_eye);
}

static void root_size(lv_obj_t *root, int32_t *w, int32_t *h)
{
    lv_obj_update_layout(root);
    *w = lv_obj_get_width(root);
    *h = lv_obj_get_height(root);
    if (*w <= 0 || *h <= 0) {
        aos_ui_app_area(w, h);
    }
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */

static lv_obj_t *make_card(lv_obj_t *parent, const char *caption, int idx)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

    s_simon.card_cap[idx] = aos_label(card, caption, aos_font_small, AOS_C_DIM);
    lv_obj_align(s_simon.card_cap[idx], LV_ALIGN_TOP_MID, 0, 16);

    s_simon.card_val[idx] = aos_label(card, "-", aos_font_large, AOS_C_TEXT);
    lv_obj_align(s_simon.card_val[idx], LV_ALIGN_BOTTOM_MID, 0, -14);
    aos_make_decorative(card);
    return card;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&s_simon, 0, sizeof(s_simon));
    s_simon.rng   = (uint32_t)aos_hal_uptime_ms() * 2654435761u | 1u;
    s_simon.flash = -1;

    int32_t v = 0;
    if (aos_hal_pref_get_i32("simon_best", &v) && v > 0 && v <= MAX_STEPS) {
        s_simon.best = (int)v;
    }

    lv_obj_t *page = aos_page(root);
    s_simon.page = page;

    s_simon.title = aos_label(page, _("SIMON"), aos_font_title, AOS_C_DIM);
    lv_obj_set_style_text_letter_space(s_simon.title, 10, 0);
    lv_obj_set_style_text_align(s_simon.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(s_simon.title, LV_OBJ_FLAG_CLICKABLE);

    s_simon.card[0] = make_card(page, _("Paso"), 0);
    s_simon.card[1] = make_card(page, _("Récord"), 1);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *pad = lv_obj_create(page);
        lv_obj_remove_style_all(pad);
        lv_obj_set_style_radius(pad, PAD_RADIUS, 0);
        lv_obj_set_style_bg_color(pad, lv_color_hex(DIM[i]), 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(pad, lv_color_hex(BRIGHT[i]), 0);
        lv_obj_set_style_shadow_width(pad, 56, 0);
        lv_obj_set_style_shadow_opa(pad, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        /* PRESSED only: registering LV_EVENT_ALL also brings the deletion's
         * events, which arrive with the context already freed. */
        lv_obj_add_event_cb(pad, pad_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        s_simon.pad[i]       = pad;
        s_simon.pad_color[i] = DIM[i];
    }

    /* The eye in the middle covers the gap between the four panels and is also
     * the start button. It goes AFTER the panels so it ends up on top. */
    s_simon.eye = lv_obj_create(page);
    lv_obj_remove_style_all(s_simon.eye);
    lv_obj_set_style_radius(s_simon.eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_simon.eye, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(s_simon.eye, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_simon.eye, AOS_C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_simon.eye, 10, 0);
    lv_obj_set_style_border_color(s_simon.eye, AOS_C_CARD2, 0);
    lv_obj_remove_flag(s_simon.eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_simon.eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_simon.eye, eye_cb, LV_EVENT_CLICKED, NULL);

    s_simon.lbl_eye = aos_label_boxed(s_simon.eye, _("TOCA"), aos_font_large,
                                      AOS_C_TEXT, 200, 120);
    lv_label_set_long_mode(s_simon.lbl_eye, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(s_simon.lbl_eye, LV_OBJ_FLAG_CLICKABLE);

    /* two lines tall: in landscape the column is narrow and a long
     * translation may wrap */
    s_simon.lbl_foot = aos_label_boxed(page, "", aos_font_body, AOS_C_DIM, 600, 80);
    lv_label_set_long_mode(s_simon.lbl_foot, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(s_simon.lbl_foot, LV_OBJ_FLAG_CLICKABLE);

    int32_t w, h;
    root_size(root, &w, &h);
    layout(w, h);

    go_idle();
    s_simon.timer = lv_timer_create(frame_cb, 40, NULL);
    return &s_simon;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    int32_t w, h;
    root_size(root, &w, &h);
    layout(w, h);
    return true;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (s_simon.timer) {
        lv_timer_delete(s_simon.timer);
        s_simon.timer = NULL;
    }
    if (self && self->root) {
        lv_obj_clean(self->root);
    }
    memset(&s_simon, 0, sizeof(s_simon));
}

static bool button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    (void)inst;
    if (action == AOS_BUTTON_CLICK) {
        s_simon.pending_start = true;
        return true;
    }
    return false;
}

static bool simon_init(aos_app_t *app)
{
    app->desc.id       = "aos.simon";
    app->desc.name     = "Simon";
    app->desc.icon     = "Si";
    app->desc.icon_vec = AOS_ICON_SIMON;
    app->desc.color_a  = 0x2C2C2E;
    app->desc.color_b  = 0x000000;
    app->desc.flags    = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 153;

    app->create        = create;
    app->destroy       = destroy;
    app->button        = button;
    app->resize        = resize;
    return true;
}

AOS_APP_ENTRY(simon_init);
