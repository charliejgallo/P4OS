/*
 * P4OS - Dados (from AmoledOS)
 *
 * Rolls one to six dice of four, six, eight, ten, twelve or twenty faces. You
 * roll by touching the big button, by touching the dice themselves, by
 * pressing the physical button or -where there is an accelerometer- by
 * shaking the board.
 *
 * Decisions worth writing down:
 *
 *   - On the watch every die was a NUMBER, because pips only work for the d6
 *     and cost seven objects per die. On the 5" panel there is room and
 *     memory for them, so the d6 has pips (7 per die, 42 in total, created
 *     once) and the other five dice keep the number, which works just as
 *     well for a d20.
 *   - The six squares exist from the start and the spare ones are hidden with
 *     LV_OBJ_FLAG_HIDDEN. Rebuilding them on every roll would be clima's
 *     hourly-strip mistake: creating and destroying objects costs 20-30 times
 *     more on the board than on the Mac.
 *   - During the animation each die's text is rewritten every 60 ms, but the
 *     square's COLOUR and the pips' visibility are only written when they
 *     change. LVGL does not compare: an lv_obj_set_style_* with the same
 *     value invalidates all the same.
 *   - This board has no IMU: the shake is kept only if aos_hal_has() says
 *     there is one, and touching the dice rolls them instead.
 *   - The size of the dice follows how many there are: one die is 300 px,
 *     six are 200. Everything is placed from the root's size in layout(),
 *     which resize() calls again on a turn of the screen, so a roll and its
 *     history survive it.
 *   - A USB gamepad walks the controls with aos_pad_menu (the die chips,
 *     minus, plus and TIRAR, where the outline starts, so A rolls). START
 *     rolls from wherever the outline is and L / R step through the dice.
 */
#include "aos_app.h"
#include "aos_theme.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_ui.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_DICE        6
#define HISTORY         6
#define PIPS            7

#define MARGIN          24
#define DIE_GAP         24
#define DIE_MAX         300
#define TOTAL_H         156         /* the total's number plus its caption */
#define CHIP_H          88
#define CHIP_AIR        12          /* around the chips, for the pad's outline */
#define COUNT_H         104
#define ROLL_H          128
#define HIST_H          48
#define GAP             24

/* Shaking: the magnitude of the difference between two consecutive readings,
 * in g. A flick of the wrist passes 1 g with room to spare; resting the board
 * on the table does not reach it. */
#define SHAKE_G         0.90f
#define SHAKE_COOLDOWN  900         /* ms between two shake rolls */
#define IMU_PERIOD_MS   100

#define ROLL_MS         700         /* how long the tumble lasts */
#define ROLL_FRAME_MS   60

#define C_DIE           0x2C2C2E
#define C_DIE_HIGH      0x1E8E3E
#define C_DIE_ONE       0x8E1F18

static const uint8_t SIDES[] = { 4, 6, 8, 10, 12, 20 };
#define N_SIDES         (int)(sizeof(SIDES) / sizeof(SIDES[0]))

/* Pips: top-left, top-right, middle-left, centre, middle-right, bottom-left,
 * bottom-right. One bit per pip, per face. */
static const int8_t PIP_POS[PIPS][2] = {
    { 1, 1 }, { 3, 1 }, { 1, 2 }, { 2, 2 }, { 3, 2 }, { 1, 3 }, { 3, 3 },
};
static const uint8_t PIP_MASK[7] = {
    0x00,
    0x08,                   /* 1: centre                  */
    0x41,                   /* 2: TL BR                   */
    0x49,                   /* 3: TL C BR                 */
    0x63,                   /* 4: TL TR BL BR             */
    0x6B,                   /* 5: 4 + centre              */
    0x77,                   /* 6: the two columns         */
};

typedef struct {
    lv_obj_t *tray;                     /* the dice area, touching it rolls */
    lv_obj_t *box[MAX_DICE];
    lv_obj_t *num[MAX_DICE];
    lv_obj_t *pip[MAX_DICE][PIPS];
    uint32_t  box_color[MAX_DICE];      /* the last thing written, so as not to repeat */
    int       pip_mask[MAX_DICE];       /* -1 = pips hidden, the number shows */

    lv_obj_t *lbl_total;
    lv_obj_t *lbl_kind;
    lv_obj_t *chip_row;
    lv_obj_t *chip[N_SIDES];
    lv_obj_t *count_card;
    lv_obj_t *btn_minus, *btn_plus;
    lv_obj_t *lbl_count;
    lv_obj_t *btn_roll;
    lv_obj_t *hist_row;
    lv_obj_t *hist_cap;
    lv_obj_t *hist_pill[HISTORY];
    lv_obj_t *hist_lbl[HISTORY];
    lv_obj_t *lbl_hint;

    lv_timer_t *timer;

    int32_t tray_w, tray_h;             /* the dice area, set by layout() */
    bool    land;

    bool     rolling;
    uint32_t roll_end_ms;
    uint32_t last_frame_ms;

    /* shaking */
    bool     has_imu;
    uint32_t last_imu_ms;
    uint32_t last_shake_ms;
    float    ax, ay, az;
    bool     imu_primed;

    bool pending_roll;                  /* left by the physical button */

    aos_pad_t      pad;
    aos_pad_menu_t pmenu;               /* chips, minus, plus and TIRAR */
} dice_ui_t;

/* The game itself outlives the UI: leaving the app and coming back, or a
 * turn of the screen that rebuilds it, keeps the last roll and the history. */
static struct {
    int  sides_idx;
    int  count;
    int  value[MAX_DICE];
    int  hist[HISTORY];
    int  hist_n;
    uint32_t rng;
    bool loaded;
} s_game;

static dice_ui_t s_dice;

/* -------------------------------------------------------------------------- */

static uint32_t rnd(void)
{
    uint32_t x = s_game.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_game.rng = x;
    return x;
}

static int die_sides(void)
{
    return SIDES[s_game.sides_idx];
}

/* -------------------------------------------------------------------------- */
/* Drawing a die                                                               */

static void set_pips(int i, int mask)
{
    if (mask == s_dice.pip_mask[i]) {
        return;
    }
    int old = s_dice.pip_mask[i] < 0 ? 0 : s_dice.pip_mask[i];
    int now = mask < 0 ? 0 : mask;
    for (int p = 0; p < PIPS; p++) {
        bool was = (old >> p) & 1;
        bool is  = (now >> p) & 1;
        if (was != is) {
            if (is) {
                lv_obj_remove_flag(s_dice.pip[i][p], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_dice.pip[i][p], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    /* the number shows only while the pips do not */
    if ((mask < 0) != (s_dice.pip_mask[i] < 0)) {
        if (mask < 0) {
            lv_obj_remove_flag(s_dice.num[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dice.num[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_dice.pip_mask[i] = mask;
}

static void paint_die(int i, bool settled)
{
    int v = s_game.value[i];
    int n = die_sides();

    uint32_t color = C_DIE;
    if (settled && v > 0) {
        if (v == n && n > 4) {
            color = C_DIE_HIGH;     /* the highest face */
        } else if (v == 1) {
            color = C_DIE_ONE;      /* the one */
        }
    }
    if (color != s_dice.box_color[i]) {
        lv_obj_set_style_bg_color(s_dice.box[i], lv_color_hex(color), 0);
        s_dice.box_color[i] = color;
    }

    if (n == 6 && v >= 1 && v <= 6) {
        set_pips(i, PIP_MASK[v]);
        return;
    }
    set_pips(i, -1);

    /* v == 0 is "not rolled yet": the square stays grey with a dash, so the
     * first screen does not show six red ones as if they were a catastrophic
     * roll. */
    /* 16 and not 8: for GCC a %d takes up to 11 characters, it does not care
     * that the number is a die face, and with -Werror=format-truncation that
     * stops the firmware's build even though the simulator says nothing. */
    char buf[16];
    if (v <= 0) {
        snprintf(buf, sizeof(buf), "%s", "-");
    } else {
        snprintf(buf, sizeof(buf), "%d", v);
    }
    lv_label_set_text(s_dice.num[i], buf);
}

static void paint_all(bool settled)
{
    for (int i = 0; i < s_game.count; i++) {
        paint_die(i, settled);
    }
}

static void refresh_total(bool settled)
{
    int total = 0;
    for (int i = 0; i < s_game.count; i++) {
        total += s_game.value[i];
    }

    char buf[32];
    if (total == 0) {                   /* freshly opened or freshly changed */
        snprintf(buf, sizeof(buf), "%s", "-");
    } else {
        snprintf(buf, sizeof(buf), "%d", total);
    }
    lv_label_set_text(s_dice.lbl_total, buf);
    lv_obj_set_style_text_color(s_dice.lbl_total,
                                settled ? AOS_C_TEXT : AOS_C_DIM, 0);

    if (s_game.count == 1) {
        snprintf(buf, sizeof(buf), "d%d", die_sides());
    } else {
        snprintf(buf, sizeof(buf), "%dd%d", s_game.count, die_sides());
    }
    lv_label_set_text(s_dice.lbl_kind, buf);
}

/* The history is a short strip read left to right: the latest first, in a
 * pill of its own colour. With nothing rolled yet the strip says how to
 * roll. */
static void refresh_history(void)
{
    bool empty = s_game.hist_n == 0;
    if (empty) {
        lv_obj_remove_flag(s_dice.lbl_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dice.hist_cap, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dice.lbl_hint, LV_OBJ_FLAG_HIDDEN);
        /* six pills plus the caption do not fit in the landscape column */
        if (s_dice.land) {
            lv_obj_add_flag(s_dice.hist_cap, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_dice.hist_cap, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < HISTORY; i++) {
        if (i >= s_game.hist_n) {
            lv_obj_add_flag(s_dice.hist_pill[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_game.hist[i]);
        lv_label_set_text(s_dice.hist_lbl[i], buf);
        lv_obj_remove_flag(s_dice.hist_pill[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------------------------------------------------------------------------- */
/* Placing the dice                                                            */

/* The six squares always exist; what changes is which are visible, where and
 * how big. Two dice go side by side, four in a square, three, five and six
 * in rows of three. The GROUP (the squares plus the total) is centred in the
 * tray: centring only the squares leaves the total hanging far away. */
static void show_dice(void)
{
    int n    = s_game.count;
    int cols = n <= 2 ? n : (n == 4 ? 2 : 3);
    int rows = (n + cols - 1) / cols;

    int32_t aw = s_dice.tray_w;
    int32_t ah = s_dice.tray_h - TOTAL_H - DIE_GAP;
    int32_t size = (aw - (cols - 1) * DIE_GAP) / cols;
    int32_t by   = (ah - (rows - 1) * DIE_GAP) / rows;
    if (by < size) {
        size = by;
    }
    if (size > DIE_MAX) {
        size = DIE_MAX;
    }

    int32_t block_h = rows * size + (rows - 1) * DIE_GAP;
    int32_t group_h = block_h + DIE_GAP + TOTAL_H;
    int32_t y0 = (s_dice.tray_h - group_h) / 2;
    if (y0 < 0) {
        y0 = 0;
    }

    lv_obj_set_width(s_dice.lbl_total, aw);
    lv_obj_set_pos(s_dice.lbl_total, 0, y0 + block_h + DIE_GAP - 8);
    lv_obj_set_width(s_dice.lbl_kind, aw);
    lv_obj_set_pos(s_dice.lbl_kind, 0, y0 + block_h + DIE_GAP + 112);

    bool big = size >= 240;
    int32_t pip = size * 17 / 100;

    for (int i = 0; i < MAX_DICE; i++) {
        lv_obj_t *box = s_dice.box[i];
        if (i >= n) {
            lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int r  = i / cols;
        int rc = n - r * cols;          /* dice on this row */
        if (rc > cols) {
            rc = cols;
        }
        int32_t x0 = (aw - (rc * size + (rc - 1) * DIE_GAP)) / 2;
        lv_obj_set_size(box, size, size);
        lv_obj_set_style_radius(box, size / 5, 0);
        lv_obj_set_pos(box, x0 + (i % cols) * (size + DIE_GAP),
                            y0 + r * (size + DIE_GAP));

        for (int p = 0; p < PIPS; p++) {
            lv_obj_set_size(s_dice.pip[i][p], pip, pip);
            lv_obj_set_pos(s_dice.pip[i][p],
                           size * PIP_POS[p][0] / 4 - pip / 2,
                           size * PIP_POS[p][1] / 4 - pip / 2);
        }
        lv_obj_set_style_text_font(s_dice.num[i],
                                   big ? &aos_inter_num_144 : &aos_inter_num_96, 0);
        lv_obj_set_width(s_dice.num[i], size);
        lv_obj_center(s_dice.num[i]);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------------------------------------------------------------------------- */
/* Roll                                                                        */

static void settle(void)
{
    int total = 0;
    for (int i = 0; i < s_game.count; i++) {
        s_game.value[i] = (int)(rnd() % (uint32_t)die_sides()) + 1;
        total += s_game.value[i];
    }
    paint_all(true);
    refresh_total(true);

    for (int i = HISTORY - 1; i > 0; i--) {
        s_game.hist[i] = s_game.hist[i - 1];
    }
    s_game.hist[0] = total;
    if (s_game.hist_n < HISTORY) {
        s_game.hist_n++;
    }
    refresh_history();

    /* Two short notes: the second higher the better the roll came out. */
    int max = s_game.count * die_sides();
    int min = s_game.count;
    int f   = 700 + (max > min ? (total - min) * 900 / (max - min) : 450);
    aos_hal_beep(f, 40);
}

static void start_roll(void)
{
    if (s_dice.rolling) {
        return;
    }
    s_dice.rolling       = true;
    s_dice.roll_end_ms   = lv_tick_get() + ROLL_MS;
    s_dice.last_frame_ms = 0;
    refresh_total(false);
    aos_hal_beep(420, 30);
}

/* -------------------------------------------------------------------------- */
/* Controls                                                                    */

static void roll_cb(lv_event_t *event)
{
    (void)event;
    start_roll();
}

static void paint_chips(void)
{
    for (int i = 0; i < N_SIDES; i++) {
        bool on = (i == s_game.sides_idx);
        lv_obj_set_style_bg_color(s_dice.chip[i], on ? AOS_C_ACCENT : AOS_C_CARD, 0);
    }
}

static void paint_count(void)
{
    char buf[24];
    if (s_game.count == 1) {
        snprintf(buf, sizeof(buf), "%s", _("1 dado"));
    } else {
        snprintf(buf, sizeof(buf), _("%d dados"), s_game.count);
    }
    lv_label_set_text(s_dice.lbl_count, buf);

    lv_obj_t *minus = lv_obj_get_child(s_dice.btn_minus, 0);
    lv_obj_t *plus  = lv_obj_get_child(s_dice.btn_plus, 0);
    lv_obj_set_style_text_color(minus, s_game.count > 1 ? AOS_C_TEXT : lv_color_hex(0x5A5A5E), 0);
    lv_obj_set_style_text_color(plus, s_game.count < MAX_DICE ? AOS_C_TEXT : lv_color_hex(0x5A5A5E), 0);
}

static void chip_cb(lv_event_t *event)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(event);
    if (idx == s_game.sides_idx || s_dice.rolling) {
        return;
    }
    s_game.sides_idx = idx;
    aos_hal_pref_set_i32("dice_sides", idx);
    paint_chips();

    /* Changing die invalidates what was on screen: the old values may not
     * exist on the new face. */
    s_game.hist_n = 0;
    for (int i = 0; i < MAX_DICE; i++) {
        s_game.value[i] = 0;
    }
    refresh_history();
    start_roll();
}

static void count_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int next  = s_game.count + delta;
    if (next < 1 || next > MAX_DICE || s_dice.rolling) {
        return;
    }
    s_game.count = next;
    aos_hal_pref_set_i32("dice_count", next);
    paint_count();

    show_dice();
    s_game.hist_n = 0;
    refresh_history();
    start_roll();
}

/* -------------------------------------------------------------------------- */
/* Shaking (only where there is an accelerometer)                              */

static void check_shake(uint32_t now)
{
    if (!s_dice.has_imu || (uint32_t)(now - s_dice.last_imu_ms) < IMU_PERIOD_MS) {
        return;
    }
    s_dice.last_imu_ms = now;

    aos_imu_t imu;
    if (!aos_hal_imu_read(&imu)) {
        return;
    }
    if (!s_dice.imu_primed) {
        s_dice.ax = imu.ax;
        s_dice.ay = imu.ay;
        s_dice.az = imu.az;
        s_dice.imu_primed = true;
        return;
    }

    float dx = imu.ax - s_dice.ax;
    float dy = imu.ay - s_dice.ay;
    float dz = imu.az - s_dice.az;
    s_dice.ax = imu.ax;
    s_dice.ay = imu.ay;
    s_dice.az = imu.az;

    float jerk = sqrtf(dx * dx + dy * dy + dz * dz);
    if (jerk < SHAKE_G) {
        return;
    }
    if ((uint32_t)(now - s_dice.last_shake_ms) < SHAKE_COOLDOWN) {
        return;
    }
    s_dice.last_shake_ms = now;
    start_roll();
}

/* -------------------------------------------------------------------------- */

static void frame_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();

    if (s_dice.pending_roll) {
        s_dice.pending_roll = false;
        start_roll();
    }

    /* the gamepad */
    aos_pad_update(&s_dice.pad, now);
    if (aos_pad_pressed(&s_dice.pad, AOS_PAD_START)) {
        start_roll();
    } else if (aos_pad_pressed(&s_dice.pad, AOS_PAD_L | AOS_PAD_R)) {
        int next = s_game.sides_idx + (aos_pad_pressed(&s_dice.pad, AOS_PAD_R) ? 1 : -1);
        if (next >= 0 && next < N_SIDES) {
            lv_obj_send_event(s_dice.chip[next], LV_EVENT_CLICKED, NULL);
        }
    } else {
        aos_pad_menu_step(&s_dice.pmenu, &s_dice.pad);
    }

    if (s_dice.rolling) {
        if ((int32_t)(now - s_dice.roll_end_ms) >= 0) {
            s_dice.rolling = false;
            settle();
        } else if ((uint32_t)(now - s_dice.last_frame_ms) >= ROLL_FRAME_MS) {
            s_dice.last_frame_ms = now;
            for (int i = 0; i < s_game.count; i++) {
                s_game.value[i] = (int)(rnd() % (uint32_t)die_sides()) + 1;
            }
            paint_all(false);
        }
        return;         /* while it tumbles the accelerometer is not watched */
    }

    check_shake(now);
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */

static lv_obj_t *flat_button(lv_obj_t *parent, const char *text,
                             lv_color_t color, const lv_font_t *font,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = aos_label(btn, text, font, AOS_C_TEXT);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    return btn;
}

static void build(lv_obj_t *root)
{
    lv_obj_t *page = aos_page(root);

    /* ---- the tray and its six squares ---- */
    s_dice.tray = lv_obj_create(page);
    lv_obj_remove_style_all(s_dice.tray);
    lv_obj_remove_flag(s_dice.tray, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dice.tray, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dice.tray, roll_cb, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < MAX_DICE; i++) {
        lv_obj_t *box = lv_obj_create(s_dice.tray);
        lv_obj_remove_style_all(box);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_DIE), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        s_dice.box[i]       = box;
        s_dice.box_color[i] = C_DIE;
        s_dice.pip_mask[i]  = -1;

        s_dice.num[i] = aos_label(box, "-", &aos_inter_num_96, AOS_C_TEXT);
        lv_obj_set_style_text_align(s_dice.num[i], LV_TEXT_ALIGN_CENTER, 0);

        for (int p = 0; p < PIPS; p++) {
            lv_obj_t *pip = lv_obj_create(box);
            lv_obj_remove_style_all(pip);
            lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(pip, AOS_C_TEXT, 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_add_flag(pip, LV_OBJ_FLAG_HIDDEN);
            s_dice.pip[i][p] = pip;
        }
        /* decoration: a touch on a die goes to the tray, which rolls */
        aos_make_decorative(box);
    }

    /* ---- total ---- */
    s_dice.lbl_total = aos_label(s_dice.tray, "-", &aos_inter_num_96, AOS_C_TEXT);
    lv_obj_set_style_text_align(s_dice.lbl_total, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(s_dice.lbl_total, LV_OBJ_FLAG_CLICKABLE);
    s_dice.lbl_kind = aos_label(s_dice.tray, "", aos_font_title, AOS_C_DIM);
    lv_obj_set_style_text_align(s_dice.lbl_kind, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(s_dice.lbl_kind, LV_OBJ_FLAG_CLICKABLE);

    /* ---- choosing the die ---- */
    s_dice.chip_row = lv_obj_create(page);
    lv_obj_remove_style_all(s_dice.chip_row);
    lv_obj_remove_flag(s_dice.chip_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_dice.chip_row, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < N_SIDES; i++) {
        char name[8];
        snprintf(name, sizeof(name), "d%d", SIDES[i]);
        s_dice.chip[i] = flat_button(s_dice.chip_row, name, AOS_C_CARD, aos_font_body,
                                     chip_cb, (void *)(intptr_t)i);
        lv_obj_set_style_radius(s_dice.chip[i], 24, 0);
    }
    paint_chips();

    /* ---- how many ---- */
    s_dice.count_card = lv_obj_create(page);
    lv_obj_remove_style_all(s_dice.count_card);
    lv_obj_set_style_bg_color(s_dice.count_card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(s_dice.count_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dice.count_card, AOS_UI_RADIUS, 0);
    lv_obj_remove_flag(s_dice.count_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_dice.count_card, LV_OBJ_FLAG_CLICKABLE);

    s_dice.btn_minus = flat_button(s_dice.count_card, AOS_SYM_MINUS, AOS_C_CARD2,
                                   &aos_sym_44, count_cb, (void *)(intptr_t)-1);
    s_dice.btn_plus  = flat_button(s_dice.count_card, AOS_SYM_PLUS, AOS_C_CARD2,
                                   &aos_sym_44, count_cb, (void *)(intptr_t)1);
    for (int k = 0; k < 2; k++) {
        lv_obj_t *b = k ? s_dice.btn_plus : s_dice.btn_minus;
        lv_obj_set_size(b, 120, 88);
        lv_obj_set_style_radius(b, 22, 0);
        lv_obj_align(b, k ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, k ? -8 : 8, 0);
    }
    s_dice.lbl_count = aos_label(s_dice.count_card, "", aos_font_title, AOS_C_TEXT);
    lv_obj_center(s_dice.lbl_count);
    lv_obj_remove_flag(s_dice.lbl_count, LV_OBJ_FLAG_CLICKABLE);
    paint_count();

    /* ---- roll: the dice glyph and the word, side by side ---- */
    s_dice.btn_roll = lv_obj_create(page);
    lv_obj_remove_style_all(s_dice.btn_roll);
    lv_obj_set_style_bg_color(s_dice.btn_roll, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_dice.btn_roll, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_dice.btn_roll, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_dice.btn_roll, 40, 0);
    lv_obj_remove_flag(s_dice.btn_roll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dice.btn_roll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dice.btn_roll, roll_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(s_dice.btn_roll, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dice.btn_roll, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dice.btn_roll, 20, 0);
    aos_label(s_dice.btn_roll, AOS_SYM_DICE_5, &aos_sym_44, AOS_C_TEXT);
    aos_label(s_dice.btn_roll, _("TIRAR"), aos_font_large, AOS_C_TEXT);
    for (uint32_t k = 0; k < lv_obj_get_child_count(s_dice.btn_roll); k++) {
        lv_obj_remove_flag(lv_obj_get_child(s_dice.btn_roll, (int32_t)k),
                           LV_OBJ_FLAG_CLICKABLE);
    }

    /* ---- history ---- */
    s_dice.hist_row = lv_obj_create(page);
    lv_obj_remove_style_all(s_dice.hist_row);
    lv_obj_remove_flag(s_dice.hist_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_dice.hist_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dice.hist_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dice.hist_row, 10, 0);

    s_dice.hist_cap = aos_label(s_dice.hist_row, _("Últimas"), aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_right(s_dice.hist_cap, 6, 0);
    for (int i = 0; i < HISTORY; i++) {
        lv_obj_t *pill = lv_obj_create(s_dice.hist_row);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, 72, HIST_H);
        lv_obj_set_style_radius(pill, HIST_H / 2, 0);
        lv_obj_set_style_bg_color(pill, i == 0 ? AOS_C_CARD2 : AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        s_dice.hist_pill[i] = pill;
        s_dice.hist_lbl[i]  = aos_label(pill, "", aos_font_small,
                                        i == 0 ? AOS_C_TEXT : AOS_C_DIM);
        lv_obj_center(s_dice.hist_lbl[i]);
    }
    s_dice.lbl_hint = aos_label(s_dice.hist_row,
                                s_dice.has_imu ? _("Agitá la placa o tocá los dados")
                                               : _("Tocá los dados para tirar"),
                                aos_font_small, AOS_C_DIM);
    aos_make_decorative(s_dice.hist_row);
}

/* Portrait: the tray on top, and under it, from the bottom up, the history,
 * TIRAR, how many and which die - the controls where the thumb is.
 * Landscape: the tray on the left and the same controls stacked in a column
 * on the right, the chips in two rows of three. */
static void layout(int32_t w, int32_t h)
{
    s_dice.land = w > h;

    int32_t cx, cw, y;
    int chip_cols;

    if (s_dice.land) {
        cw = 480;
        cx = w - MARGIN - cw;
        chip_cols = 3;
        int32_t col_h = 2 * CHIP_H + 12 + GAP + COUNT_H + GAP + ROLL_H + GAP + HIST_H;
        y = (h - col_h) / 2;

        s_dice.tray_w = cx - 2 * MARGIN - 24;
        s_dice.tray_h = h - 2 * MARGIN;
        lv_obj_set_pos(s_dice.tray, MARGIN + 12, MARGIN);
    } else {
        cw = w - 2 * MARGIN;
        cx = MARGIN;
        chip_cols = N_SIDES;
        int32_t col_h = CHIP_H + GAP + COUNT_H + GAP + ROLL_H + GAP + HIST_H;
        y = h - MARGIN - col_h;

        s_dice.tray_w = cw;
        s_dice.tray_h = y - GAP - MARGIN;
        lv_obj_set_pos(s_dice.tray, MARGIN, MARGIN);
    }
    lv_obj_set_size(s_dice.tray, s_dice.tray_w, s_dice.tray_h);

    /* chips */
    int chip_rows = (N_SIDES + chip_cols - 1) / chip_cols;
    int32_t chip_w = (cw - (chip_cols - 1) * 12) / chip_cols;
    int32_t rows_h = chip_rows * CHIP_H + (chip_rows - 1) * 12;
    /* the row reaches CHIP_AIR beyond the chips: room for the gamepad's
     * outline around the end ones, which the row would otherwise clip */
    lv_obj_set_size(s_dice.chip_row, cw + 2 * CHIP_AIR, rows_h + 2 * CHIP_AIR);
    lv_obj_set_pos(s_dice.chip_row, cx - CHIP_AIR, y - CHIP_AIR);
    for (int i = 0; i < N_SIDES; i++) {
        lv_obj_set_size(s_dice.chip[i], chip_w, CHIP_H);
        lv_obj_set_pos(s_dice.chip[i], CHIP_AIR + (i % chip_cols) * (chip_w + 12),
                                       CHIP_AIR + (i / chip_cols) * (CHIP_H + 12));
    }
    y += rows_h + GAP;

    lv_obj_set_size(s_dice.count_card, cw, COUNT_H);
    lv_obj_set_pos(s_dice.count_card, cx, y);
    y += COUNT_H + GAP;

    lv_obj_set_size(s_dice.btn_roll, cw, ROLL_H);
    lv_obj_set_pos(s_dice.btn_roll, cx, y);
    y += ROLL_H + GAP;

    lv_obj_set_size(s_dice.hist_row, cw, HIST_H);
    lv_obj_set_pos(s_dice.hist_row, cx, y);
    for (int i = 0; i < HISTORY; i++) {
        lv_obj_set_width(s_dice.hist_pill[i], s_dice.land ? 64 : 72);
    }

    show_dice();
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

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&s_dice, 0, sizeof(s_dice));
    s_dice.has_imu = aos_hal_has(AOS_CAP_IMU);

    if (!s_game.loaded) {
        s_game.rng = (uint32_t)aos_hal_uptime_ms() * 2654435761u | 1u;
        int32_t v = 0;
        if (aos_hal_pref_get_i32("dice_sides", &v) && v >= 0 && v < N_SIDES) {
            s_game.sides_idx = (int)v;
        } else {
            s_game.sides_idx = 1;       /* d6 */
        }
        if (aos_hal_pref_get_i32("dice_count", &v) && v >= 1 && v <= MAX_DICE) {
            s_game.count = (int)v;
        } else {
            s_game.count = 2;
        }
        s_game.loaded = true;
    }

    build(root);

    int32_t w, h;
    root_size(root, &w, &h);
    layout(w, h);

    refresh_history();
    refresh_total(true);
    paint_all(true);

    lv_obj_t *items[N_SIDES + 3];
    for (int i = 0; i < N_SIDES; i++) {
        items[i] = s_dice.chip[i];
    }
    items[N_SIDES]     = s_dice.btn_minus;
    items[N_SIDES + 1] = s_dice.btn_plus;
    items[N_SIDES + 2] = s_dice.btn_roll;
    aos_pad_menu_set(&s_dice.pmenu, items, N_SIDES + 3, N_SIDES + 2);
    aos_pad_reset(&s_dice.pad, lv_tick_get());

    s_dice.last_imu_ms   = lv_tick_get();
    s_dice.last_shake_ms = lv_tick_get();
    s_dice.timer = lv_timer_create(frame_cb, 30, NULL);
    return &s_dice;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    int32_t w, h;
    root_size(root, &w, &h);
    layout(w, h);
    refresh_history();
    return true;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (s_dice.timer) {
        lv_timer_delete(s_dice.timer);
        s_dice.timer = NULL;
    }
    /* a tumble cut short leaves random faces: settle them quietly so the
     * next visit does not open on a half-rolled screen */
    if (s_dice.rolling) {
        for (int i = 0; i < s_game.count; i++) {
            s_game.value[i] = 0;
        }
    }
    if (self && self->root) {
        lv_obj_clean(self->root);
    }
    memset(&s_dice, 0, sizeof(s_dice));
}

/* The physical button rolls. It runs from the HAL's task, so it only leaves a
 * note and the next frame picks it up. The long press is left to the system,
 * which is the way out to the clock. */
static bool button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    (void)inst;
    if (action == AOS_BUTTON_CLICK) {
        s_dice.pending_roll = true;
        return true;
    }
    return false;
}

static bool dados_init(aos_app_t *app)
{
    app->desc.id       = "aos.dice";
    app->desc.name     = "Dados";
    app->desc.icon     = "d6";
    app->desc.icon_vec = AOS_ICON_DICE;
    app->desc.color_a  = 0xBF5AF2;
    app->desc.color_b  = 0x5B2078;
    app->desc.order    = 75;

    app->create        = create;
    app->destroy       = destroy;
    app->button        = button;
    app->resize        = resize;
    return true;
}

AOS_APP_ENTRY(dados_init);
