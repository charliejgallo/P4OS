/*
 * P4OS - Pato goma: a card app (.so) that plays little keyboard-and-mouse
 * scripts out over USB (or the Bluetooth keyboard) onto the computer the
 * board is plugged into - a "rubber ducky" whose payloads the owner writes,
 * picks and confirms on the board. Nothing runs without a tap on the confirm
 * screen, so it is automation, not an attack: the script is only ever what
 * the user typed.
 *
 * Ported from AmoledOS (components/aos_apps/_pending/aos_app_pato.c). Two
 * things are new here:
 *   - it is a dynamic app: no firmware to build, it brings its own duck icon
 *     (PATO_ICON) and its own portal page (web/pato.js);
 *   - the 5" screen is big enough to EDIT a script's flow on the board, not
 *     only in the portal: a flow of steps you add, reorder, retype and save
 *     to the card, with an on-screen keyboard. The watch could only pick and
 *     run; here you can also write.
 *
 * DISCLAIMER. This app exists only to educate and to demonstrate the HID
 * capabilities of the board. We take no responsibility for the scripts third
 * parties run with it, nor for any misuse they may put it to.
 *
 * The scripts live on the card, one text file per script in <sd>/pato,
 * written here or from the #pato page of the portal (both sides read and
 * write the same .pato text). The script language:
 *
 *     # a comment, ignored
 *     STRING texto        types the rest of the line
 *     KEY nombre          a key or a combo: enter, esc, cmd+space, f5, up...
 *     DELAY ms            waits
 *     MOUSE dx dy         moves the pointer (relative)
 *     SCROLL n            the wheel (+ up, - down)
 *     CLICK [1|2]         click, 1 left (default) / 2 right
 *     REPEAT n            repeats the previous line n more times
 *
 * Why the runner plays out from an lv_timer one action at a time: every key
 * and every typed character blocks the HAL ~50 ms, so running a whole script
 * inside one callback would freeze the screen for seconds and the "Parar"
 * button with it. One character / one key per tick keeps each callback short,
 * the progress bar moving and Parar alive. A big MOUSE move is split at parse
 * time into chunks of <=120 (a report clamps at +-127).
 *
 * Running needs the keyboard: either the USB port in KEYS mode with the
 * computer listening, or the board taken as a Bluetooth keyboard
 * (aos_hal_usb_keys_ready() is true for both). If neither, the list offers
 * the switch to KEYS mode, as the Macro pad does.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* ========================================================================== */
/* Icon: a rubber duck. Yellow body and head, orange beak, a dark eye, drawn
 * over the tile's water-blue gradient. Coordinates are percent of the icon
 * size, so the one blob serves every launcher size. */
static const uint8_t PATO_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,  -4,  16, 58, 42, AIC_CIRCLE, AIC_C_YELLOW, 255),   /* body */
    AIC_RECT(AIC_CENTER,  20, -16, 36, 36, AIC_CIRCLE, AIC_C_YELLOW, 255),   /* head */
    AIC_RECT(AIC_CENTER,  42, -12, 22, 13,          6, AIC_C_ORANGE, 255),   /* beak */
    AIC_RECT(AIC_CENTER,  22, -22,  8,  8, AIC_CIRCLE, AIC_C_BG,     255),   /* eye  */
    AIC_END
};

/* ========================================================================== */
#define PATO_MAX_SCRIPTS   64      /* how many files the list shows            */
#define PATO_NAME_MAX      48      /* a script's file name, without .pato      */
#define PATO_MAX_STEPS     800     /* runner steps, after REPEAT expansion     */
#define PATO_MAX_FILE      16384   /* a script file we will read / write       */
#define PATO_MOUSE_CHUNK   120     /* a report clamps at +-127                 */
#define PATO_TICK_MS       10      /* the runner's period                      */
#define PATO_ED_MAX        200     /* editable lines in the on-board editor    */
#define PATO_VAL           256     /* a STRING line's length in the editor     */

/* -------------------------------------------------------------------------- */
/* The runner's program (expanded)                                             */
/* -------------------------------------------------------------------------- */
typedef enum {
    ST_STRING, ST_KEY, ST_DELAY, ST_MOUSE, ST_SCROLL, ST_CLICK,
} step_kind_t;

typedef struct {
    step_kind_t kind;
    char       *text;   /* STRING text or KEY name, malloc'd; NULL otherwise */
    int         a, b;   /* DELAY ms=a; MOUSE dx=a,dy=b; SCROLL n=a; CLICK btn=a */
} pato_step_t;

/* -------------------------------------------------------------------------- */
/* The editor's model (one entry per written line, comments kept)              */
/* -------------------------------------------------------------------------- */
typedef enum {
    ED_STRING, ED_KEY, ED_DELAY, ED_MOUSE, ED_SCROLL, ED_CLICK, ED_REPEAT,
    ED_COMMENT, ED_KIND_COUNT
} ed_kind_t;

typedef struct {
    ed_kind_t kind;
    char      a[PATO_VAL];   /* STRING/KEY/COMMENT text, or first number       */
    char      b[16];         /* MOUSE dy                                       */
} edit_step_t;

/* Row widgets of the flow list, so reorder/delete can find them. */
typedef struct {
    lv_obj_t *row;
    int       index;
} ed_rowref_t;

typedef enum { FACE_LIST, FACE_CONFIRM, FACE_RUN, FACE_EDIT, FACE_STEP } face_t;

typedef struct {
    int32_t   w, h;

    /* faces */
    lv_obj_t *list_box, *confirm_box, *run_box, *edit_box, *step_box;
    face_t    face;

    /* list face */
    lv_obj_t *status;
    lv_obj_t *switch_btn;
    lv_obj_t *list;
    lv_timer_t *poll;
    uint32_t  list_sig;
    int       list_tick;

    /* confirm face */
    lv_obj_t *cf_title, *cf_meta, *cf_preview, *cf_run_btn;

    /* run face */
    lv_obj_t *run_title, *run_bar, *run_step, *run_btn;
    lv_timer_t *run_timer;

    /* the loaded (expanded) program, run from the confirm face */
    pato_step_t *steps;
    int          nsteps;
    char         loaded[PATO_NAME_MAX];
    int          cur, char_pos;
    bool         waiting, done;
    uint32_t     wait_until;

    /* editor face */
    lv_obj_t *ed_name;          /* name textarea                               */
    lv_obj_t *ed_kb;            /* keyboard for the name field                 */
    lv_obj_t *ed_status;        /* small status line                           */
    lv_obj_t *ed_flow;          /* scrollable column of step rows              */
    char      ed_orig[PATO_NAME_MAX + 8];  /* file being edited, "" if new     */

    /* step-sheet face (edits one step) */
    lv_obj_t *sp_op;            /* op dropdown                                 */
    lv_obj_t *sp_body;          /* holds the kind's input(s)                   */
    lv_obj_t *sp_kb;            /* on-screen keyboard                          */
    int       sp_cur;           /* which edit step is open                     */

    /* the editable model */
    edit_step_t *ed;
    int          ed_count;
} pato_t;

static pato_t P;
static char   s_dir[160];      /* <sd>/pato                                    */

/* button user_data for the list rows and flow rows must outlive the event:
 * kept in static tables refilled on every rebuild. */
static char s_names[PATO_MAX_SCRIPTS][PATO_NAME_MAX];
static int  s_nnames;

/* ========================================================================== */
/* Small helpers                                                               */
/* ========================================================================== */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Read a script file whole into a caller buffer, NUL-terminated. */
static bool read_file(const char *name, char *buf, size_t cap)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%.*s.pato", s_dir,
             (int)(PATO_NAME_MAX - 1), name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return true;
}

static bool write_file(const char *name, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%.*s.pato", s_dir,
             (int)(PATO_NAME_MAX - 1), name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    size_t len = strlen(text);
    size_t w = fwrite(text, 1, len, f);
    fclose(f);
    return w == len;
}

static void delete_file(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%.*s.pato", s_dir,
             (int)(PATO_NAME_MAX - 1), name);
    remove(path);
}

/* A name the card takes: drop the awkward characters, trim, cap the length. */
static void clean_name(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap && j < PATO_NAME_MAX - 1; i++) {
        char c = in[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *t = trim(out);
    if (t != out) {
        memmove(out, t, strlen(t) + 1);
    }
}

/* ========================================================================== */
/* The runner's program                                                        */
/* ========================================================================== */

static void prog_free(void)
{
    for (int i = 0; i < P.nsteps; i++) {
        free(P.steps[i].text);
        P.steps[i].text = NULL;
    }
    P.nsteps = 0;
}

static bool step_add(step_kind_t kind, const char *text, int a, int b)
{
    if (!P.steps || P.nsteps >= PATO_MAX_STEPS) {
        return false;
    }
    pato_step_t *st = &P.steps[P.nsteps];
    st->kind = kind;
    st->a = a;
    st->b = b;
    st->text = NULL;
    if (text) {
        st->text = malloc(strlen(text) + 1);
        if (!st->text) {
            return false;
        }
        strcpy(st->text, text);
    }
    P.nsteps++;
    return true;
}

/* A MOUSE move larger than a report can carry becomes several steps, so the
 * runner never has to loop inside one tick. */
static bool step_add_mouse(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return step_add(ST_MOUSE, NULL, 0, 0);
    }
    while (dx != 0 || dy != 0) {
        int cx = clampi(dx, -PATO_MOUSE_CHUNK, PATO_MOUSE_CHUNK);
        int cy = clampi(dy, -PATO_MOUSE_CHUNK, PATO_MOUSE_CHUNK);
        if (!step_add(ST_MOUSE, NULL, cx, cy)) {
            return false;
        }
        dx -= cx;
        dy -= cy;
    }
    return true;
}

static bool prog_repeat(int n)
{
    if (P.nsteps == 0 || n <= 0) {
        return true;
    }
    pato_step_t src = P.steps[P.nsteps - 1];
    for (int i = 0; i < n; i++) {
        if (!step_add(src.kind, src.text, src.a, src.b)) {
            return false;
        }
    }
    return true;
}

/* Parse the whole text (chewed up in place) into P.steps. Unknown lines are
 * skipped, so a typo loses a line, never the script. */
static void prog_parse(char *text)
{
    prog_free();
    char *p = text;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        char *line = p;
        if (nl) {
            *nl = '\0';
        }
        p = nl ? nl + 1 : NULL;

        char *s = trim(line);
        if (s[0] == '\0' || s[0] == '#') {
            continue;
        }
        char *arg = s;
        while (*arg && *arg != ' ' && *arg != '\t') {
            arg++;
        }
        char verb[12];
        size_t vl = (size_t)(arg - s);
        if (vl >= sizeof(verb)) {
            continue;
        }
        for (size_t i = 0; i < vl; i++) {
            verb[i] = (char)toupper((unsigned char)s[i]);
        }
        verb[vl] = '\0';
        while (*arg == ' ' || *arg == '\t') {
            arg++;
        }

        if (!strcmp(verb, "STRING") || !strcmp(verb, "TYPE")) {
            if (*arg) {
                step_add(ST_STRING, arg, 0, 0);
            }
        } else if (!strcmp(verb, "KEY") || !strcmp(verb, "PRESS")) {
            if (*arg) {
                step_add(ST_KEY, arg, 0, 0);
            }
        } else if (!strcmp(verb, "DELAY") || !strcmp(verb, "WAIT")) {
            step_add(ST_DELAY, NULL, clampi(atoi(arg), 0, 60000), 0);
        } else if (!strcmp(verb, "MOUSE") || !strcmp(verb, "MOVE")) {
            int dx = 0, dy = 0;
            sscanf(arg, "%d %d", &dx, &dy);
            step_add_mouse(dx, dy);
        } else if (!strcmp(verb, "SCROLL")) {
            step_add(ST_SCROLL, NULL, clampi(atoi(arg), -127, 127), 0);
        } else if (!strcmp(verb, "CLICK")) {
            step_add(ST_CLICK, NULL, (atoi(arg) == 2) ? 2 : 1, 0);
        } else if (!strcmp(verb, "REPEAT")) {
            prog_repeat(atoi(arg));
        }
        if (P.nsteps >= PATO_MAX_STEPS) {
            break;
        }
    }
}

/* ========================================================================== */
/* USB / keyboard readiness                                                    */
/* ========================================================================== */

/* True when a key would actually reach a computer: the USB port is KEYS and a
 * host has it, OR the board is a Bluetooth keyboard with a host. Both make
 * aos_hal_usb_keys_ready() true. */
static bool keys_ready(void)
{
    return aos_hal_usb_keys_ready();
}

/* ========================================================================== */
/* Faces                                                                       */
/* ========================================================================== */

static void show_face(face_t face)
{
    P.face = face;
    lv_obj_add_flag(P.list_box,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(P.confirm_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(P.run_box,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(P.edit_box,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(P.step_box,    LV_OBJ_FLAG_HIDDEN);
    /* keyboards belong to one face each; never let one linger over another */
    if (P.sp_kb) {
        lv_keyboard_set_textarea(P.sp_kb, NULL);
        lv_obj_add_flag(P.sp_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (P.ed_kb) {
        lv_keyboard_set_textarea(P.ed_kb, NULL);
        lv_obj_add_flag(P.ed_kb, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *which = P.list_box;
    switch (face) {
    case FACE_LIST:    which = P.list_box;    break;
    case FACE_CONFIRM: which = P.confirm_box; break;
    case FACE_RUN:     which = P.run_box;     break;
    case FACE_EDIT:    which = P.edit_box;    break;
    case FACE_STEP:    which = P.step_box;    break;
    }
    lv_obj_remove_flag(which, LV_OBJ_FLAG_HIDDEN);
}

/* ========================================================================== */
/* The runner                                                                  */
/* ========================================================================== */

static const char *step_caption(const pato_step_t *st)
{
    static char buf[64];
    switch (st->kind) {
    case ST_STRING: snprintf(buf, sizeof(buf), "%s", _("Escribiendo")); break;
    case ST_KEY:    snprintf(buf, sizeof(buf), "%s %.40s", _("Tecla"), st->text ? st->text : ""); break;
    case ST_DELAY:  snprintf(buf, sizeof(buf), "%s %d ms", _("Espera"), st->a); break;
    case ST_MOUSE:  snprintf(buf, sizeof(buf), "%s", _("Mouse")); break;
    case ST_SCROLL: snprintf(buf, sizeof(buf), "%s", _("Rueda")); break;
    case ST_CLICK:  snprintf(buf, sizeof(buf), "%s", _("Clic")); break;
    default:        buf[0] = '\0'; break;
    }
    return buf;
}

static void run_stop(bool finished)
{
    if (P.run_timer) {
        lv_timer_delete(P.run_timer);
        P.run_timer = NULL;
    }
    P.done = finished;
    if (finished) {
        lv_bar_set_value(P.run_bar, 100, LV_ANIM_OFF);
        lv_label_set_text(P.run_step, _("Listo"));
        lv_label_set_text(lv_obj_get_child(P.run_btn, 0), _("Volver"));
        lv_obj_set_style_bg_color(P.run_btn, AOS_C_CARD2, 0);
        aos_hal_beep(1600, 20);
    }
}

static void run_tick(lv_timer_t *timer)
{
    (void)timer;

    if (!keys_ready()) {
        run_stop(false);
        show_face(FACE_LIST);
        aos_ui_toast(_("Se perdió el teclado"), 1600);
        return;
    }
    if (P.cur >= P.nsteps) {
        run_stop(true);
        return;
    }

    pato_step_t *st = &P.steps[P.cur];
    bool advance = true;

    switch (st->kind) {
    case ST_STRING: {
        int len = st->text ? (int)strlen(st->text) : 0;
        if (P.char_pos < len) {
            char one[2] = { st->text[P.char_pos], '\0' };
            aos_hal_usb_type(one);
            P.char_pos++;
        }
        advance = (P.char_pos >= len);
        break;
    }
    case ST_KEY:
        if (st->text) {
            aos_hal_usb_key(st->text);
        }
        break;
    case ST_DELAY:
        if (!P.waiting) {
            P.waiting = true;
            P.wait_until = (uint32_t)aos_hal_uptime_ms() + (uint32_t)st->a;
            advance = false;
        } else if ((int32_t)((uint32_t)aos_hal_uptime_ms() - P.wait_until) >= 0) {
            P.waiting = false;
        } else {
            advance = false;
        }
        break;
    case ST_MOUSE:
        aos_hal_usb_mouse(st->a, st->b, 0);
        break;
    case ST_SCROLL:
        aos_hal_usb_mouse(0, 0, st->a);
        break;
    case ST_CLICK:
        aos_hal_usb_click(st->a);
        break;
    }

    if (advance) {
        P.cur++;
        P.char_pos = 0;
        lv_label_set_text(P.run_step,
                          P.cur < P.nsteps ? step_caption(&P.steps[P.cur]) : "");
    }
    lv_bar_set_value(P.run_bar, P.nsteps ? P.cur * 100 / P.nsteps : 100,
                     LV_ANIM_OFF);
}

static void run_start(void)
{
    P.cur = 0;
    P.char_pos = 0;
    P.waiting = false;
    P.done = false;

    lv_label_set_text(P.run_title, P.loaded);
    lv_bar_set_value(P.run_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(P.run_step,
                      P.nsteps ? step_caption(&P.steps[0]) : "");
    lv_label_set_text(lv_obj_get_child(P.run_btn, 0), _("Parar"));
    lv_obj_set_style_bg_color(P.run_btn, AOS_C_RED, 0);
    show_face(FACE_RUN);

    if (!P.run_timer) {
        P.run_timer = lv_timer_create(run_tick, PATO_TICK_MS, NULL);
    }
}

/* ========================================================================== */
/* Editor model: parse / serialize                                             */
/* ========================================================================== */

static void ed_clear(void)
{
    P.ed_count = 0;
}

static void ed_add(ed_kind_t kind, const char *a, const char *b)
{
    if (P.ed_count >= PATO_ED_MAX) {
        return;
    }
    edit_step_t *e = &P.ed[P.ed_count++];
    e->kind = kind;
    snprintf(e->a, sizeof(e->a), "%s", a ? a : "");
    snprintf(e->b, sizeof(e->b), "%s", b ? b : "");
}

/* Parse a .pato text into the editor model, keeping comments. Blank lines are
 * dropped (they carry nothing). Unknown verbs are kept as a comment so the
 * line is not silently lost. */
static void ed_parse(const char *text)
{
    ed_clear();
    char *copy = malloc(strlen(text) + 1);
    if (!copy) {
        return;
    }
    strcpy(copy, text);
    char *p = copy;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        char *line = p;
        if (nl) {
            *nl = '\0';
        }
        p = nl ? nl + 1 : NULL;

        char *s = trim(line);
        if (s[0] == '\0') {
            continue;
        }
        if (s[0] == '#') {
            char *c = s + 1;
            if (*c == ' ') {
                c++;
            }
            ed_add(ED_COMMENT, c, NULL);
            continue;
        }
        char *arg = s;
        while (*arg && *arg != ' ' && *arg != '\t') {
            arg++;
        }
        char verb[12];
        size_t vl = (size_t)(arg - s);
        if (vl >= sizeof(verb)) {
            ed_add(ED_COMMENT, s, NULL);
            continue;
        }
        for (size_t i = 0; i < vl; i++) {
            verb[i] = (char)toupper((unsigned char)s[i]);
        }
        verb[vl] = '\0';
        while (*arg == ' ' || *arg == '\t') {
            arg++;
        }

        if (!strcmp(verb, "STRING") || !strcmp(verb, "TYPE")) {
            ed_add(ED_STRING, arg, NULL);
        } else if (!strcmp(verb, "KEY") || !strcmp(verb, "PRESS")) {
            ed_add(ED_KEY, arg, NULL);
        } else if (!strcmp(verb, "DELAY") || !strcmp(verb, "WAIT")) {
            char n[16];
            snprintf(n, sizeof(n), "%d", clampi(atoi(arg), 0, 60000));
            ed_add(ED_DELAY, n, NULL);
        } else if (!strcmp(verb, "MOUSE") || !strcmp(verb, "MOVE")) {
            int dx = 0, dy = 0;
            sscanf(arg, "%d %d", &dx, &dy);
            char sa[16], sb[16];
            snprintf(sa, sizeof(sa), "%d", dx);
            snprintf(sb, sizeof(sb), "%d", dy);
            ed_add(ED_MOUSE, sa, sb);
        } else if (!strcmp(verb, "SCROLL")) {
            char n[16];
            snprintf(n, sizeof(n), "%d", clampi(atoi(arg), -127, 127));
            ed_add(ED_SCROLL, n, NULL);
        } else if (!strcmp(verb, "CLICK")) {
            ed_add(ED_CLICK, (atoi(arg) == 2) ? "2" : "1", NULL);
        } else if (!strcmp(verb, "REPEAT")) {
            char n[16];
            snprintf(n, sizeof(n), "%d", clampi(atoi(arg), 1, 500));
            ed_add(ED_REPEAT, n, NULL);
        } else {
            ed_add(ED_COMMENT, s, NULL);
        }
        if (P.ed_count >= PATO_ED_MAX) {
            break;
        }
    }
    free(copy);
}

/* Serialize the editor model into 'out'. Returns the real step count (what the
 * computer executes: not comments). */
static int ed_serialize(char *out, size_t cap)
{
    size_t n = 0;
    int real = 0;
    out[0] = '\0';
    for (int i = 0; i < P.ed_count; i++) {
        const edit_step_t *e = &P.ed[i];
        char line[PATO_VAL + 32];
        switch (e->kind) {
        case ED_STRING:  snprintf(line, sizeof(line), "STRING %s", e->a); real++; break;
        case ED_KEY:     snprintf(line, sizeof(line), "KEY %s", e->a);    real++; break;
        case ED_DELAY:   snprintf(line, sizeof(line), "DELAY %d", clampi(atoi(e->a), 0, 60000)); real++; break;
        case ED_MOUSE:   snprintf(line, sizeof(line), "MOUSE %d %d", atoi(e->a), atoi(e->b)); real++; break;
        case ED_SCROLL:  snprintf(line, sizeof(line), "SCROLL %d", clampi(atoi(e->a), -127, 127)); real++; break;
        case ED_CLICK:   snprintf(line, sizeof(line), "CLICK %d", (atoi(e->a) == 2) ? 2 : 1); real++; break;
        case ED_REPEAT:  snprintf(line, sizeof(line), "REPEAT %d", clampi(atoi(e->a), 1, 500)); real += clampi(atoi(e->a), 1, 500); break;
        case ED_COMMENT: snprintf(line, sizeof(line), "# %s", e->a); break;
        default:         line[0] = '\0'; break;
        }
        size_t ll = strlen(line);
        if (n + ll + 2 >= cap) {
            break;
        }
        memcpy(out + n, line, ll);
        n += ll;
        out[n++] = '\n';
        out[n] = '\0';
    }
    return real;
}

/* ========================================================================== */
/* The step sheet (edits one editor step)                                      */
/* ========================================================================== */

/* op dropdown options, in ed_kind_t order. */
static const char *kind_option(ed_kind_t k)
{
    switch (k) {
    case ED_STRING:  return _("Escribir");
    case ED_KEY:     return _("Tecla");
    case ED_DELAY:   return _("Esperar");
    case ED_MOUSE:   return _("Mouse");
    case ED_SCROLL:  return _("Rueda");
    case ED_CLICK:   return _("Clic");
    case ED_REPEAT:  return _("Repetir");
    case ED_COMMENT: return _("Comentario");
    default:         return "";
    }
}

static void sp_keyboard(bool show, lv_obj_t *target, lv_keyboard_mode_t mode)
{
    if (show && target) {
        lv_keyboard_set_mode(P.sp_kb, mode);
        lv_keyboard_set_textarea(P.sp_kb, target);
        lv_obj_remove_flag(P.sp_kb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_keyboard_set_textarea(P.sp_kb, NULL);
        lv_obj_add_flag(P.sp_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* a value textarea inside the step sheet: wire it to show the keyboard. */
static lv_obj_t *sp_textarea(const char *text, bool numeric, const char *ph)
{
    lv_obj_t *ta = lv_textarea_create(P.sp_body);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, text ? text : "");
    if (ph) {
        lv_textarea_set_placeholder_text(ta, ph);
    }
    lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_user_data(ta, (void *)(uintptr_t)numeric);
    return ta;
}

static void sp_ta_click_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    bool numeric = (bool)(uintptr_t)lv_obj_get_user_data(ta);
    sp_keyboard(true, ta,
                numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
}

/* Read the step sheet's inputs back into the model. The widgets on screen
 * belong to the step's CURRENT kind (what sp_build_body drew), not to the
 * dropdown, which may already point at a new kind mid-change. */
static void sp_collect(void)
{
    if (P.sp_cur < 0 || P.sp_cur >= P.ed_count) {
        return;
    }
    edit_step_t *e = &P.ed[P.sp_cur];

    /* children of sp_body, in creation order */
    uint32_t n = lv_obj_get_child_count(P.sp_body);
    lv_obj_t *fields[3] = { NULL, NULL, NULL };
    int nf = 0;
    for (uint32_t i = 0; i < n && nf < 3; i++) {
        lv_obj_t *c = lv_obj_get_child(P.sp_body, i);
        if (lv_obj_check_type(c, &lv_textarea_class) ||
            lv_obj_check_type(c, &lv_dropdown_class)) {
            fields[nf++] = c;
        }
    }

    e->a[0] = '\0';
    e->b[0] = '\0';
    switch (e->kind) {
    case ED_STRING: case ED_KEY: case ED_COMMENT:
        if (fields[0]) {
            snprintf(e->a, sizeof(e->a), "%s", lv_textarea_get_text(fields[0]));
        }
        break;
    case ED_DELAY: case ED_SCROLL: case ED_REPEAT:
        if (fields[0]) {
            snprintf(e->a, sizeof(e->a), "%s", lv_textarea_get_text(fields[0]));
        }
        break;
    case ED_MOUSE:
        if (fields[0]) {
            snprintf(e->a, sizeof(e->a), "%s", lv_textarea_get_text(fields[0]));
        }
        if (fields[1]) {
            snprintf(e->b, sizeof(e->b), "%s", lv_textarea_get_text(fields[1]));
        }
        break;
    case ED_CLICK:
        if (fields[0]) {
            snprintf(e->a, sizeof(e->a), "%s",
                     lv_dropdown_get_selected(fields[0]) == 1 ? "2" : "1");
        }
        break;
    default:
        break;
    }
}

static void sp_build_body(void); /* fwd */

static void sp_op_cb(lv_event_t *e)
{
    (void)e;
    sp_collect();            /* read the fields under the kind still showing */
    P.ed[P.sp_cur].kind = (ed_kind_t)lv_dropdown_get_selected(P.sp_op);
    sp_build_body();
}

static void sp_build_body(void)
{
    lv_obj_clean(P.sp_body);
    sp_keyboard(false, NULL, 0);

    edit_step_t *st = &P.ed[P.sp_cur];
    ed_kind_t k = (ed_kind_t)lv_dropdown_get_selected(P.sp_op);
    st->kind = k;
    int fw = P.w - 2 * AOS_UI_PAD;

    lv_obj_t *hint = NULL;

    switch (k) {
    case ED_STRING: {
        lv_obj_t *ta = sp_textarea(st->a, false, _("texto a escribir"));
        lv_obj_set_size(ta, fw, 76);
        lv_obj_add_event_cb(ta, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        hint = aos_label(P.sp_body, _("Se escribe tal cual en la computadora."),
                         aos_font_small, AOS_C_DIM);
        break;
    }
    case ED_KEY: {
        lv_obj_t *ta = sp_textarea(st->a, false, "enter, cmd+space, f5...");
        lv_obj_set_size(ta, fw, 76);
        lv_obj_add_event_cb(ta, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        hint = aos_label(P.sp_body,
                         _("Una tecla o combo: enter, esc, up, cmd+space,\n"
                           "ctrl+alt+supr, f5. Prefijos: cmd+ ctrl+ alt+ shift+"),
                         aos_font_small, AOS_C_DIM);
        break;
    }
    case ED_DELAY: {
        lv_obj_t *ta = sp_textarea(st->a[0] ? st->a : "500", true, "500");
        lv_obj_set_size(ta, fw / 2, 76);
        lv_obj_add_event_cb(ta, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        hint = aos_label(P.sp_body, _("Espera, en milisegundos (0 a 60000)."),
                         aos_font_small, AOS_C_DIM);
        break;
    }
    case ED_MOUSE: {
        lv_obj_t *dx = sp_textarea(st->a[0] ? st->a : "0", true, "dx");
        lv_obj_set_size(dx, fw / 2 - 8, 76);
        lv_obj_add_event_cb(dx, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *dy = sp_textarea(st->b[0] ? st->b : "0", true, "dy");
        lv_obj_set_size(dy, fw / 2 - 8, 76);
        lv_obj_add_event_cb(dy, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        hint = aos_label(P.sp_body,
                         _("Mueve el puntero: dx a la derecha, dy hacia abajo."),
                         aos_font_small, AOS_C_DIM);
        break;
    }
    case ED_SCROLL: {
        lv_obj_t *ta = sp_textarea(st->a[0] ? st->a : "0", true, "0");
        lv_obj_set_size(ta, fw / 2, 76);
        lv_obj_add_event_cb(ta, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        hint = aos_label(P.sp_body, _("Rueda: positivo arriba, negativo abajo."),
                         aos_font_small, AOS_C_DIM);
        break;
    }
    case ED_CLICK: {
        lv_obj_t *dd = lv_dropdown_create(P.sp_body);
        char opts[32];
        snprintf(opts, sizeof(opts), "%s\n%s", _("izquierdo"), _("derecho"));
        lv_dropdown_set_options(dd, opts);
        lv_dropdown_set_selected(dd, (atoi(st->a) == 2) ? 1 : 0);
        lv_obj_set_size(dd, fw / 2, 64);
        lv_obj_set_style_text_font(dd, aos_font_body, 0);
        break;
    }
    case ED_REPEAT: {
        lv_obj_t *ta = sp_textarea(st->a[0] ? st->a : "2", true, "2");
        lv_obj_set_size(ta, fw / 2, 76);
        lv_obj_add_event_cb(ta, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        hint = aos_label(P.sp_body,
                         _("Repite el paso anterior esta cantidad de veces."),
                         aos_font_small, AOS_C_DIM);
        break;
    }
    case ED_COMMENT: {
        lv_obj_t *ta = sp_textarea(st->a, false, _("nota"));
        lv_obj_set_size(ta, fw, 76);
        lv_obj_add_event_cb(ta, sp_ta_click_cb, LV_EVENT_CLICKED, NULL);
        hint = aos_label(P.sp_body, _("No se ejecuta: sólo una nota para vos."),
                         aos_font_small, AOS_C_DIM);
        break;
    }
    default:
        break;
    }

    if (hint) {
        lv_obj_set_width(hint, fw);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    }
}

static void sp_open(int index)
{
    P.sp_cur = index;
    lv_dropdown_set_selected(P.sp_op, (uint32_t)P.ed[index].kind);
    sp_build_body();
    show_face(FACE_STEP);
}

/* ========================================================================== */
/* The flow (editor face)                                                      */
/* ========================================================================== */

/* one-line summary of a step for the flow list. */
static void ed_summary(const edit_step_t *e, char *out, size_t cap)
{
    switch (e->kind) {
    case ED_STRING:  snprintf(out, cap, "%s  %.40s", _("Escribir"), e->a); break;
    case ED_KEY:     snprintf(out, cap, "%s  %.40s", _("Tecla"), e->a); break;
    case ED_DELAY:   snprintf(out, cap, "%s  %d ms", _("Esperar"), clampi(atoi(e->a), 0, 60000)); break;
    case ED_MOUSE:   snprintf(out, cap, "%s  %d, %d", _("Mouse"), atoi(e->a), atoi(e->b)); break;
    case ED_SCROLL:  snprintf(out, cap, "%s  %d", _("Rueda"), clampi(atoi(e->a), -127, 127)); break;
    case ED_CLICK:   snprintf(out, cap, "%s  %s", _("Clic"), (atoi(e->a) == 2) ? _("derecho") : _("izquierdo")); break;
    case ED_REPEAT:  snprintf(out, cap, "%s  x%d", _("Repetir"), clampi(atoi(e->a), 1, 500)); break;
    case ED_COMMENT: snprintf(out, cap, "#  %.40s", e->a); break;
    default:         out[0] = '\0'; break;
    }
}

static void ed_rebuild_flow(void); /* fwd */

static void flow_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sp_open(i);
}

static void flow_up_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i > 0) {
        edit_step_t tmp = P.ed[i];
        P.ed[i] = P.ed[i - 1];
        P.ed[i - 1] = tmp;
        ed_rebuild_flow();
    }
}

static void flow_down_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < P.ed_count - 1) {
        edit_step_t tmp = P.ed[i];
        P.ed[i] = P.ed[i + 1];
        P.ed[i + 1] = tmp;
        ed_rebuild_flow();
    }
}

static void flow_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < P.ed_count) {
        for (int j = i; j < P.ed_count - 1; j++) {
            P.ed[j] = P.ed[j + 1];
        }
        P.ed_count--;
        ed_rebuild_flow();
    }
}

static void ed_rebuild_flow(void)
{
    lv_obj_clean(P.ed_flow);
    int fw = P.w - 2 * AOS_UI_PAD;

    if (P.ed_count == 0) {
        lv_obj_t *empty = aos_label(P.ed_flow,
            _("Flujo vacío. Agregá el primer paso."),
            aos_font_small, AOS_C_DIM);
        lv_obj_set_width(empty, lv_pct(100));
        return;
    }

    for (int i = 0; i < P.ed_count; i++) {
        lv_obj_t *row = lv_obj_create(P.ed_flow);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 64);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);

        /* the summary, clickable to edit the step */
        lv_obj_t *txtbox = lv_obj_create(row);
        lv_obj_remove_style_all(txtbox);
        lv_obj_set_flex_grow(txtbox, 1);
        lv_obj_set_height(txtbox, lv_pct(100));
        lv_obj_remove_flag(txtbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(txtbox, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(txtbox, flow_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        char sum[96];
        ed_summary(&P.ed[i], sum, sizeof(sum));
        lv_obj_t *lbl = aos_label(txtbox, sum, aos_font_body,
                                  P.ed[i].kind == ED_COMMENT ? AOS_C_DIM : AOS_C_TEXT);
        lv_obj_set_width(lbl, fw - 230);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_center(lbl);

        lv_obj_t *up = aos_button(row, LV_SYMBOL_UP, AOS_C_CARD2, flow_up_cb,
                                  (void *)(intptr_t)i);
        lv_obj_set_size(up, 48, 48);
        lv_obj_t *dn = aos_button(row, LV_SYMBOL_DOWN, AOS_C_CARD2, flow_down_cb,
                                  (void *)(intptr_t)i);
        lv_obj_set_size(dn, 48, 48);
        lv_obj_t *del = aos_button(row, LV_SYMBOL_TRASH, AOS_C_CARD2, flow_del_cb,
                                   (void *)(intptr_t)i);
        lv_obj_set_size(del, 48, 48);
        lv_obj_set_style_text_color(lv_obj_get_child(del, 0), AOS_C_RED, 0);
    }
}

static void ed_set_status(const char *msg, lv_color_t color)
{
    lv_label_set_text(P.ed_status, msg ? msg : "");
    lv_obj_set_style_text_color(P.ed_status, color, 0);
}

/* open the editor on a file (name without .pato), or NULL for a new script. */
static void ed_open(const char *name)
{
    if (name) {
        char *raw = malloc(PATO_MAX_FILE);
        if (raw && read_file(name, raw, PATO_MAX_FILE)) {
            ed_parse(raw);
        } else {
            ed_clear();
        }
        free(raw);
        snprintf(P.ed_orig, sizeof(P.ed_orig), "%s.pato", name);
        lv_textarea_set_text(P.ed_name, name);
    } else {
        ed_clear();
        ed_add(ED_STRING, "", NULL);
        P.ed_orig[0] = '\0';
        lv_textarea_set_text(P.ed_name, "");
    }
    ed_set_status("", AOS_C_DIM);
    ed_rebuild_flow();
    show_face(FACE_EDIT);
}

static void refresh_list(void); /* fwd */

/* the name field's keyboard (its own, since the step sheet's lives in another
 * face) */
static void ed_name_click_cb(lv_event_t *e)
{
    (void)e;
    lv_keyboard_set_mode(P.ed_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(P.ed_kb, P.ed_name);
    lv_obj_remove_flag(P.ed_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ed_kb_done_cb(lv_event_t *e)
{
    (void)e;
    lv_keyboard_set_textarea(P.ed_kb, NULL);
    lv_obj_add_flag(P.ed_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ed_save_cb(lv_event_t *e)
{
    (void)e;
    char name[PATO_NAME_MAX];
    clean_name(lv_textarea_get_text(P.ed_name), name, sizeof(name));
    if (!name[0]) {
        ed_set_status(_("Ponele un nombre"), AOS_C_RED);
        return;
    }

    char *text = malloc(PATO_MAX_FILE);
    if (!text) {
        return;
    }
    int real = ed_serialize(text, PATO_MAX_FILE);
    bool ok = write_file(name, text);
    free(text);
    if (!ok) {
        ed_set_status(_("No se pudo guardar"), AOS_C_RED);
        return;
    }

    /* renamed: drop the old file */
    char newfile[PATO_NAME_MAX + 8];
    snprintf(newfile, sizeof(newfile), "%s.pato", name);
    if (P.ed_orig[0] && strcasecmp(P.ed_orig, newfile) != 0) {
        char old[PATO_NAME_MAX];
        size_t ol = strlen(P.ed_orig);
        snprintf(old, sizeof(old), "%.*s", (int)(ol > 5 ? ol - 5 : ol), P.ed_orig);
        delete_file(old);
    }
    snprintf(P.ed_orig, sizeof(P.ed_orig), "%s", newfile);

    char msg[48];
    snprintf(msg, sizeof(msg), "%s · %d %s", _("Guardado"), real, _("pasos"));
    ed_set_status(msg, AOS_C_GREEN);
    P.list_sig = 0;          /* force the list to refresh on return */
}

static void ed_add_cb(lv_event_t *e)
{
    (void)e;
    if (P.ed_count >= PATO_ED_MAX) {
        aos_ui_toast(_("Demasiados pasos"), 1400);
        return;
    }
    ed_add(ED_STRING, "", NULL);
    sp_open(P.ed_count - 1);
}

static void ed_back_cb(lv_event_t *e)
{
    (void)e;
    show_face(FACE_LIST);
    refresh_list();
}

static void ed_delfile_cb(lv_event_t *e)
{
    (void)e;
    if (P.ed_orig[0]) {
        char old[PATO_NAME_MAX];
        size_t ol = strlen(P.ed_orig);
        snprintf(old, sizeof(old), "%.*s", (int)(ol > 5 ? ol - 5 : ol), P.ed_orig);
        delete_file(old);
        aos_ui_toast(_("Borrado"), 1200);
    }
    P.list_sig = 0;
    show_face(FACE_LIST);
    refresh_list();
}

/* step sheet "Listo": collect and return to the flow. */
static void sp_done_cb(lv_event_t *e)
{
    (void)e;
    sp_collect();
    sp_keyboard(false, NULL, 0);
    ed_rebuild_flow();
    show_face(FACE_EDIT);
}

/* ========================================================================== */
/* Confirm face                                                                */
/* ========================================================================== */

static void confirm_run_cb(lv_event_t *e)
{
    (void)e;
    if (!keys_ready()) {
        aos_ui_toast(_("Pasá el USB a Teclado, o emparejá por Bluetooth"), 2000);
        return;
    }
    if (P.nsteps == 0) {
        aos_ui_toast(_("El script está vacío"), 1400);
        return;
    }
    run_start();
}

static void confirm_edit_cb(lv_event_t *e)
{
    (void)e;
    ed_open(P.loaded);
}

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    show_face(FACE_LIST);
}

/* Load a script into the confirm face (expanded program + preview). */
static void load_confirm(const char *name)
{
    char *raw = malloc(PATO_MAX_FILE);
    if (!raw) {
        return;
    }
    if (!read_file(name, raw, PATO_MAX_FILE)) {
        free(raw);
        aos_ui_toast(_("No se pudo leer el script"), 1600);
        return;
    }
    snprintf(P.loaded, sizeof(P.loaded), "%s", name);

    static char preview[640];
    size_t plen = strlen(raw);
    if (plen >= sizeof(preview)) {
        plen = sizeof(preview) - 1;
    }
    memcpy(preview, raw, plen);
    preview[plen] = '\0';

    prog_parse(raw);
    free(raw);

    lv_label_set_text(P.cf_title, P.loaded);
    char meta[48];
    snprintf(meta, sizeof(meta), "%d %s", P.nsteps, _("pasos"));
    lv_label_set_text(P.cf_meta, meta);
    lv_label_set_text(P.cf_preview, preview);
    show_face(FACE_CONFIRM);
}

/* ========================================================================== */
/* List face                                                                   */
/* ========================================================================== */

static void run_btn_cb(lv_event_t *e)
{
    (void)e;
    if (P.done) {
        show_face(FACE_LIST);
        return;
    }
    run_stop(false);
    show_face(FACE_LIST);
    aos_ui_toast(_("Detenido"), 1200);
}

static void list_pick_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    load_confirm(name);
}

static void list_edit_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    ed_open(name);
}

static void list_new_cb(lv_event_t *e)
{
    (void)e;
    ed_open(NULL);
}

static void switch_cb(lv_event_t *e)
{
    (void)e;
    if (!aos_hal_usb_mode_set(AOS_HAL_USB_KEYS)) {
        aos_ui_toast(_("El USB está cambiando de modo"), 1400);
    }
}

static void refresh_list(void)
{
    lv_obj_clean(P.list);
    s_nnames = 0;
    int fw = P.w - 2 * AOS_UI_PAD;

    DIR *d = opendir(s_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) && s_nnames < PATO_MAX_SCRIPTS) {
            if (ent->d_name[0] == '.') {
                continue;        /* skip ._ files macOS leaves behind */
            }
            size_t len = strlen(ent->d_name);
            if (len < 6 || strcasecmp(ent->d_name + len - 5, ".pato") != 0) {
                continue;
            }
            size_t base = len - 5;
            if (base >= PATO_NAME_MAX) {
                base = PATO_NAME_MAX - 1;
            }
            memcpy(s_names[s_nnames], ent->d_name, base);
            s_names[s_nnames][base] = '\0';
            s_nnames++;
        }
        closedir(d);
    }

    if (s_nnames == 0) {
        lv_obj_t *empty = aos_label(P.list,
            _("No hay scripts todavía.\nCreá uno con «Nuevo script»,\n"
              "o desde el portal (página Pato goma)."),
            aos_font_body, AOS_C_DIM);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
        return;
    }

    for (int i = 0; i < s_nnames; i++) {
        lv_obj_t *row = lv_obj_create(P.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 70);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        lv_obj_t *namebox = lv_obj_create(row);
        lv_obj_remove_style_all(namebox);
        lv_obj_set_flex_grow(namebox, 1);
        lv_obj_set_height(namebox, lv_pct(100));
        lv_obj_remove_flag(namebox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(namebox, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(namebox, list_pick_cb, LV_EVENT_CLICKED, s_names[i]);
        lv_obj_t *nm = aos_label(namebox, s_names[i], aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(nm, fw - 200);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_center(nm);

        lv_obj_t *edit = aos_button(row, LV_SYMBOL_EDIT, AOS_C_CARD2,
                                    list_edit_cb, s_names[i]);
        lv_obj_set_size(edit, 54, 54);
        lv_obj_t *play = aos_button(row, LV_SYMBOL_PLAY, AOS_C_ACCENT,
                                    list_pick_cb, s_names[i]);
        lv_obj_set_size(play, 54, 54);
    }
}

/* A cheap fingerprint of the folder (FNV-1a over name/size/mtime), so the list
 * refreshes itself when a script is added, removed or edited from the portal. */
static uint32_t list_signature(void)
{
    uint32_t h = 2166136261u;
    DIR *d = opendir(s_dir);
    if (!d) {
        return 0;
    }
    struct dirent *ent;
    char path[512];      /* s_dir + '/' + d_name (up to 255): avoid truncation */
    while ((ent = readdir(d))) {
        size_t len = strlen(ent->d_name);
        if (ent->d_name[0] == '.' || len < 6 ||
            strcasecmp(ent->d_name + len - 5, ".pato") != 0) {
            continue;
        }
        for (const char *p = ent->d_name; *p; p++) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        struct stat stt;
        snprintf(path, sizeof(path), "%s/%s", s_dir, ent->d_name);
        if (stat(path, &stt) == 0) {
            h = (h ^ (uint32_t)stt.st_size) * 16777619u;
            h = (h ^ (uint32_t)stt.st_mtime) * 16777619u;
        }
    }
    closedir(d);
    return h;
}

static void poll_cb(lv_timer_t *timer)
{
    (void)timer;
    bool ready = keys_ready();
    lv_label_set_text(P.status,
                      ready ? _("Teclado listo (USB o Bluetooth)")
                            : _("Pasalo a «Teclado» o emparejá Bluetooth"));
    lv_obj_set_style_text_color(P.status, ready ? AOS_C_GREEN : AOS_C_ORANGE, 0);
    if (ready) {
        lv_obj_add_flag(P.switch_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(P.switch_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* refresh the list about every two seconds, only on the list face, and
     * only when the folder actually changed. */
    if (P.face == FACE_LIST && ++P.list_tick >= 4) {
        P.list_tick = 0;
        uint32_t sig = list_signature();
        if (sig != P.list_sig) {
            P.list_sig = sig;
            refresh_list();
        }
    }
}

/* ========================================================================== */
/* Building the faces                                                          */
/* ========================================================================== */

static lv_obj_t *make_box(lv_obj_t *root)
{
    lv_obj_t *box = lv_obj_create(root);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(box, AOS_UI_PAD, 0);
    lv_obj_set_style_bg_color(box, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    return t;
}

static void build_list(lv_obj_t *root)
{
    P.list_box = make_box(root);

    make_title(P.list_box, "Pato goma");

    P.status = aos_label(P.list_box, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(P.status, LV_ALIGN_TOP_LEFT, 0, 48);

    lv_obj_t *nuevo = aos_button(P.list_box, _("Nuevo script"), AOS_C_GREEN,
                                 list_new_cb, NULL);
    lv_obj_set_size(nuevo, lv_pct(100), 60);
    lv_obj_align(nuevo, LV_ALIGN_TOP_MID, 0, 86);

    P.switch_btn = aos_button(P.list_box, _("Pasar el USB a Teclado"),
                              AOS_C_ACCENT, switch_cb, NULL);
    lv_obj_set_size(P.switch_btn, lv_pct(100), 56);
    lv_obj_align(P.switch_btn, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_add_flag(P.switch_btn, LV_OBJ_FLAG_HIDDEN);

    P.list = lv_obj_create(P.list_box);
    lv_obj_remove_style_all(P.list);
    lv_obj_set_style_bg_opa(P.list, LV_OPA_TRANSP, 0);
    lv_obj_set_size(P.list, lv_pct(100), P.h - 2 * AOS_UI_PAD - 230);
    lv_obj_align(P.list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(P.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(P.list, 10, 0);
    lv_obj_set_scroll_dir(P.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(P.list, LV_SCROLLBAR_MODE_OFF);
}

static void build_confirm(lv_obj_t *root)
{
    P.confirm_box = make_box(root);
    int fw = P.w - 2 * AOS_UI_PAD;

    P.cf_title = aos_label(P.confirm_box, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(P.cf_title, fw);
    lv_label_set_long_mode(P.cf_title, LV_LABEL_LONG_DOT);
    lv_obj_align(P.cf_title, LV_ALIGN_TOP_LEFT, 0, 0);

    P.cf_meta = aos_label(P.confirm_box, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(P.cf_meta, LV_ALIGN_TOP_LEFT, 0, 48);

    lv_obj_t *pv = lv_obj_create(P.confirm_box);
    lv_obj_set_style_bg_color(pv, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(pv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pv, 0, 0);
    lv_obj_set_style_radius(pv, 12, 0);
    lv_obj_set_style_pad_all(pv, 14, 0);
    lv_obj_set_size(pv, fw, P.h - 2 * AOS_UI_PAD - 260);
    lv_obj_align(pv, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_scroll_dir(pv, LV_DIR_VER);
    P.cf_preview = aos_label(pv, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(P.cf_preview, lv_pct(100));
    lv_label_set_long_mode(P.cf_preview, LV_LABEL_LONG_WRAP);

    lv_obj_t *warn = aos_label(P.confirm_box,
        _("Se enviará al teclado y mouse de la computadora."),
        aos_font_small, AOS_C_ORANGE);
    lv_obj_set_width(warn, fw);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_align(warn, LV_ALIGN_BOTTOM_MID, 0, -78);

    lv_obj_t *cancel = aos_button(P.confirm_box, _("Volver"), AOS_C_CARD2,
                                  confirm_cancel_cb, NULL);
    lv_obj_set_size(cancel, (fw - 20) / 3, 60);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *edit = aos_button(P.confirm_box, _("Editar"), AOS_C_CARD2,
                                confirm_edit_cb, NULL);
    lv_obj_set_size(edit, (fw - 20) / 3, 60);
    lv_obj_align(edit, LV_ALIGN_BOTTOM_MID, 0, 0);

    P.cf_run_btn = aos_button(P.confirm_box, _("Ejecutar"), AOS_C_GREEN,
                              confirm_run_cb, NULL);
    lv_obj_set_size(P.cf_run_btn, (fw - 20) / 3, 60);
    lv_obj_align(P.cf_run_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void build_run(lv_obj_t *root)
{
    P.run_box = make_box(root);
    int fw = P.w - 2 * AOS_UI_PAD;

    P.run_title = aos_label(P.run_box, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(P.run_title, fw);
    lv_label_set_long_mode(P.run_title, LV_LABEL_LONG_DOT);
    lv_obj_align(P.run_title, LV_ALIGN_TOP_LEFT, 0, 10);

    P.run_bar = lv_bar_create(P.run_box);
    lv_obj_set_size(P.run_bar, fw, 18);
    lv_obj_align(P.run_bar, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(P.run_bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(P.run_bar, AOS_C_GREEN, LV_PART_INDICATOR);
    lv_bar_set_value(P.run_bar, 0, LV_ANIM_OFF);

    P.run_step = aos_label(P.run_box, "", aos_font_body, AOS_C_DIM);
    lv_obj_align(P.run_step, LV_ALIGN_TOP_MID, 0, 130);

    P.run_btn = aos_button(P.run_box, _("Parar"), AOS_C_RED, run_btn_cb, NULL);
    lv_obj_set_size(P.run_btn, 260, 64);
    lv_obj_align(P.run_btn, LV_ALIGN_TOP_MID, 0, 210);
}

static void build_edit(lv_obj_t *root)
{
    P.edit_box = make_box(root);
    int fw = P.w - 2 * AOS_UI_PAD;

    lv_obj_t *back = aos_button(P.edit_box, LV_SYMBOL_LEFT, AOS_C_CARD2,
                                ed_back_cb, NULL);
    lv_obj_set_size(back, 60, 56);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);

    P.ed_name = lv_textarea_create(P.edit_box);
    lv_textarea_set_one_line(P.ed_name, true);
    lv_textarea_set_placeholder_text(P.ed_name, _("Nombre del script"));
    lv_textarea_set_max_length(P.ed_name, PATO_NAME_MAX - 1);
    lv_obj_set_size(P.ed_name, fw - 70, 56);
    lv_obj_align(P.ed_name, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(P.ed_name, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(P.ed_name, 0, 0);
    lv_obj_set_style_radius(P.ed_name, 12, 0);
    lv_obj_set_style_text_color(P.ed_name, AOS_C_TEXT, 0);
    lv_obj_set_style_text_font(P.ed_name, aos_font_body, 0);
    lv_obj_add_event_cb(P.ed_name, ed_name_click_cb, LV_EVENT_CLICKED, NULL);

    P.ed_status = aos_label(P.edit_box, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(P.ed_status, LV_ALIGN_TOP_LEFT, 0, 66);

    P.ed_flow = lv_obj_create(P.edit_box);
    lv_obj_remove_style_all(P.ed_flow);
    lv_obj_set_style_bg_opa(P.ed_flow, LV_OPA_TRANSP, 0);
    lv_obj_set_size(P.ed_flow, lv_pct(100), P.h - 2 * AOS_UI_PAD - 100 - 76);
    lv_obj_align(P.ed_flow, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_flex_flow(P.ed_flow, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(P.ed_flow, 8, 0);
    lv_obj_set_scroll_dir(P.ed_flow, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(P.ed_flow, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *add = aos_button(P.edit_box, _("Agregar paso"), AOS_C_ACCENT,
                               ed_add_cb, NULL);
    lv_obj_set_size(add, (fw - 20) / 3, 60);
    lv_obj_align(add, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *save = aos_button(P.edit_box, _("Guardar"), AOS_C_GREEN,
                                ed_save_cb, NULL);
    lv_obj_set_size(save, (fw - 20) / 3, 60);
    lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *del = aos_button(P.edit_box, _("Borrar"), AOS_C_CARD2,
                               ed_delfile_cb, NULL);
    lv_obj_set_size(del, (fw - 20) / 3, 60);
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(del, 0), AOS_C_RED, 0);

    P.ed_kb = lv_keyboard_create(P.edit_box);
    lv_obj_set_size(P.ed_kb, P.w, P.h * 2 / 5);
    lv_obj_align(P.ed_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(P.ed_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(P.ed_kb, aos_font_body, 0);
    lv_obj_set_style_bg_color(P.ed_kb, AOS_C_BG, 0);
    lv_obj_set_style_bg_color(P.ed_kb, AOS_C_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(P.ed_kb, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_width(P.ed_kb, 0, 0);
    lv_obj_set_style_border_width(P.ed_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(P.ed_kb, 8, LV_PART_ITEMS);
    lv_obj_add_flag(P.ed_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(P.ed_kb, ed_kb_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(P.ed_kb, ed_kb_done_cb, LV_EVENT_CANCEL, NULL);
}

static void build_step(lv_obj_t *root)
{
    P.step_box = make_box(root);
    int fw = P.w - 2 * AOS_UI_PAD;

    make_title(P.step_box, _("Paso"));

    lv_obj_t *done = aos_button(P.step_box, _("Listo"), AOS_C_GREEN,
                                sp_done_cb, NULL);
    lv_obj_set_size(done, 140, 56);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, 0);

    P.sp_op = lv_dropdown_create(P.step_box);
    char opts[160];
    int o = 0;
    for (int k = 0; k < ED_KIND_COUNT; k++) {
        o += snprintf(opts + o, sizeof(opts) - o, "%s%s",
                      k ? "\n" : "", kind_option((ed_kind_t)k));
    }
    lv_dropdown_set_options(P.sp_op, opts);
    lv_obj_set_size(P.sp_op, fw, 64);
    lv_obj_align(P.sp_op, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_text_font(P.sp_op, aos_font_body, 0);
    lv_obj_add_event_cb(P.sp_op, sp_op_cb, LV_EVENT_VALUE_CHANGED, NULL);

    P.sp_body = lv_obj_create(P.step_box);
    lv_obj_remove_style_all(P.sp_body);
    lv_obj_set_style_bg_opa(P.sp_body, LV_OPA_TRANSP, 0);
    lv_obj_set_size(P.sp_body, lv_pct(100), P.h - 2 * AOS_UI_PAD - 150 - (P.h * 2 / 5));
    lv_obj_align(P.sp_body, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_flex_flow(P.sp_body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(P.sp_body, 0, 0);
    lv_obj_set_style_pad_column(P.sp_body, 12, 0);
    lv_obj_set_style_pad_row(P.sp_body, 12, 0);

    P.sp_kb = lv_keyboard_create(P.step_box);
    lv_obj_set_size(P.sp_kb, P.w, P.h * 2 / 5);
    lv_obj_align(P.sp_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(P.sp_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(P.sp_kb, aos_font_body, 0);
    lv_obj_set_style_bg_color(P.sp_kb, AOS_C_BG, 0);
    lv_obj_set_style_bg_color(P.sp_kb, AOS_C_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(P.sp_kb, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_width(P.sp_kb, 0, 0);
    lv_obj_set_style_border_width(P.sp_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(P.sp_kb, 8, LV_PART_ITEMS);
    lv_obj_add_flag(P.sp_kb, LV_OBJ_FLAG_HIDDEN);
    /* the keyboard's own checkmark hides it */
    lv_obj_add_event_cb(P.sp_kb, sp_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(P.sp_kb, sp_done_cb, LV_EVENT_CANCEL, NULL);
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

static void *pato_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&P, 0, sizeof(P));

    P.w = lv_obj_get_width(root);
    P.h = lv_obj_get_height(root);
    if (P.w <= 0) {
        P.w = 720;
    }
    if (P.h <= 0) {
        P.h = 1280;
    }

    P.steps = malloc(sizeof(pato_step_t) * PATO_MAX_STEPS);   /* PSRAM */
    P.ed    = malloc(sizeof(edit_step_t) * PATO_ED_MAX);      /* PSRAM */
    if (!P.steps || !P.ed) {
        free(P.steps);
        free(P.ed);
        return NULL;
    }

    const char *sd = aos_hal_path_sd_root();
    snprintf(s_dir, sizeof(s_dir), "%s/pato", sd ? sd : "/sdcard");
    mkdir(s_dir, 0775);

    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);

    build_list(root);
    build_confirm(root);
    build_run(root);
    build_edit(root);
    build_step(root);

    refresh_list();
    P.list_sig = list_signature();
    poll_cb(NULL);
    P.poll = lv_timer_create(poll_cb, 500, NULL);
    show_face(FACE_LIST);
    return &P;
}

static void pato_destroy(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (P.run_timer) {
        lv_timer_delete(P.run_timer);
        P.run_timer = NULL;
    }
    if (P.poll) {
        lv_timer_delete(P.poll);
        P.poll = NULL;
    }
    prog_free();
    free(P.steps);
    P.steps = NULL;
    free(P.ed);
    P.ed = NULL;
    /* the LVGL objects under root are freed by the runtime */
}

static bool pato_back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    switch (P.face) {
    case FACE_RUN:
        if (!P.done) {
            run_stop(false);
        }
        show_face(FACE_LIST);
        return true;
    case FACE_CONFIRM:
        show_face(FACE_LIST);
        return true;
    case FACE_EDIT:
        show_face(FACE_LIST);
        refresh_list();
        return true;
    case FACE_STEP:
        sp_collect();
        sp_keyboard(false, NULL, 0);
        ed_rebuild_flow();
        show_face(FACE_EDIT);
        return true;
    default:
        return false;
    }
}

static bool pato_init(aos_app_t *app)
{
    app->desc.id       = "aos.pato";
    app->desc.name     = "Pato goma";
    app->desc.icon     = LV_SYMBOL_USB;       /* fallback if the blob is refused */
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x0A84FF;            /* water blue, so the yellow pops  */
    app->desc.color_b  = 0x0050A0;
    app->desc.order    = 210;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE;   /* a run must not dim out */
    aos_icon_set_ops(app, PATO_ICON, sizeof PATO_ICON);

    app->create  = pato_create;
    app->destroy = pato_destroy;
    app->back    = pato_back;
    return true;
}

AOS_APP_ENTRY(pato_init);
