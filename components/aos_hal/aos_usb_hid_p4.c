/*
 * P4OS - HID devices on the USB host: keyboards, mice, gamepads and media
 * keys (aos_hal.h, docs/USB.md).
 *
 * A client of ESP-IDF's USB Host Library, beside usb_host_msc. Every new
 * device is opened, and each of its HID interfaces (class 3, any subclass)
 * is claimed and set up in three steps, each the completion of the one
 * before (they run inside the client's event handling, where nothing may
 * wait for a transfer):
 *
 *   1. GET_DESCRIPTOR(Report): the report descriptor, parsed here into the
 *      fields of each input report - which usage, where in the report, how
 *      many bits, signed or not, relative or absolute - under the
 *      application collection they belong to (keyboard, mouse, joystick,
 *      gamepad, consumer control). An interface can carry several, told
 *      apart by report IDs (a wireless receiver: keyboard, mouse and media
 *      keys on one interface).
 *   2. SET_IDLE(0): reports only when something changes.
 *   3. The interrupt IN endpoint read with one transfer that resubmits
 *      itself.
 *
 * A keyboard whose descriptor cannot be read or parsed falls back to the
 * boot protocol (SET_PROTOCOL 0), whose layout is fixed: eight bytes,
 * modifiers and up to six keys. A boot mouse likewise: buttons, X, Y.
 *
 * What comes out:
 *   - keys, as characters through a layout (US, or Latin American with ñ,
 *     ¿¡, AltGr and the acute and diaeresis as dead keys), into the queue
 *     aos_hal_usb_kbd_read() empties; a key held repeats (500 ms, then
 *     25 a second). Caps Lock lights the keyboard's LED (SET_REPORT).
 *   - media keys (volume, play/pause, next, previous, mute), into the same
 *     queue as AOS_KEY_CONSUMER + their usage;
 *   - mouse motion, buttons and wheel, summed until aos_hal_hid_mouse_read()
 *     takes them; an absolute pointer (a touch screen, a tablet) as a
 *     position;
 *   - gamepads and joysticks as a state, read whenever wanted
 *     (aos_hal_hid_gamepad_get): buttons, axes scaled to +-32767, the hat.
 *
 * The devices list (aos_usb_devs_p4.c) learns what each device is used for.
 */
#include "aos_hal.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";
#define PREF_LAYOUT "usb_kbdlay"
#define IF_MAX      6           /* HID interfaces at once */
#define FIELD_MAX   96          /* input fields of one interface */
#define DESC_MAX    1024        /* the largest report descriptor read */
#define KEYS_MAX    16          /* keys held at once that are tracked */

void aos_p4_usb_dev_use(uint8_t addr, const char *what);

/* the application collections the fields belong to */
enum { APP_NONE = 0, APP_KEYBOARD, APP_MOUSE, APP_JOYSTICK, APP_CONSUMER, APP_SYSTEM };

typedef struct {
    uint16_t page;
    uint16_t usage, usage_max;  /* a range for arrays and runs of buttons; usage alone for one value */
    uint16_t bit;               /* where in the report, after the report ID byte */
    uint8_t  size, count;
    uint8_t  report_id;
    uint8_t  app;
    uint8_t  flags;             /* the Input item's: bit 0 constant, 1 variable, 2 relative */
    int32_t  lmin, lmax;
} field_t;

typedef struct {
    usb_device_handle_t dev;
    uint8_t addr, intf, ep, sub, proto;
    uint16_t mps, desc_len;
    usb_transfer_t *in, *ctrl, *led;
    volatile bool gone, in_busy, ctrl_busy, led_busy;
    bool halted;
    int step;                   /* 0 report descriptor, 1 idle, 2 running */
    bool boot;                  /* the boot protocol's fixed layout */
    bool ids;                   /* the reports start with a report ID */
    field_t *f;
    int nf;
    uint8_t apps;               /* bit per APP_* found */
    /* keyboard */
    uint8_t keys[KEYS_MAX], nkeys, mods;
    uint8_t led_id, led_bytes;  /* the output report with the LEDs, and its length */
    int16_t led_caps_bit;       /* -1 none */
    /* consumer: the usages held */
    uint16_t cons[4];
    /* gamepad slot, -1 none */
    int pad;
    char name[48];
} hid_t;

static struct {
    usb_host_client_handle_t client;
    volatile bool stop, done;
    QueueHandle_t q;
    SemaphoreHandle_t mx;
    hid_t h[IF_MAX];
    /* keyboard state shared by all keyboards */
    uint8_t held_code, held_mods;
    TickType_t held_next;
    uint32_t dead;
    bool caps;
    /* mouse: summed until read */
    aos_mouse_event_t mouse;
    bool mouse_new;
    int mice;
    /* gamepads */
    aos_gamepad_t pad[AOS_GAMEPAD_MAX];
} K;

/* ---- layouts ---------------------------------------------------------- */

/* HID usages 0x04..0x38 and 0x64: plain, with Shift, with AltGr (0: none) */
typedef struct { uint8_t usage; uint32_t plain, shift, altgr; } keymap_t;

#define DEAD_ACUTE 0x00B4   /* ´ */
#define DEAD_DIAER 0x00A8   /* ¨ */

static const keymap_t US[] = {
    { 0x1E, '1', '!', 0 }, { 0x1F, '2', '@', 0 }, { 0x20, '3', '#', 0 }, { 0x21, '4', '$', 0 },
    { 0x22, '5', '%', 0 }, { 0x23, '6', '^', 0 }, { 0x24, '7', '&', 0 }, { 0x25, '8', '*', 0 },
    { 0x26, '9', '(', 0 }, { 0x27, '0', ')', 0 }, { 0x2C, ' ', ' ', 0 }, { 0x2D, '-', '_', 0 },
    { 0x2E, '=', '+', 0 }, { 0x2F, '[', '{', 0 }, { 0x30, ']', '}', 0 }, { 0x31, '\\', '|', 0 },
    { 0x32, '#', '~', 0 }, { 0x33, ';', ':', 0 }, { 0x34, '\'', '"', 0 }, { 0x35, '`', '~', 0 },
    { 0x36, ',', '<', 0 }, { 0x37, '.', '>', 0 }, { 0x38, '/', '?', 0 }, { 0x64, '\\', '|', 0 },
};

static const keymap_t LATAM[] = {
    { 0x14, 'q', 'Q', '@' },
    { 0x1E, '1', '!', '|' }, { 0x1F, '2', '"', '@' }, { 0x20, '3', '#', 0 }, { 0x21, '4', '$', '~' },
    { 0x22, '5', '%', 0 }, { 0x23, '6', '&', 0 }, { 0x24, '7', '/', 0 }, { 0x25, '8', '(', 0 },
    { 0x26, '9', ')', 0 }, { 0x27, '0', '=', 0 }, { 0x2C, ' ', ' ', 0 },
    { 0x2D, '\'', '?', '\\' }, { 0x2E, 0x00BF, 0x00A1, 0 },             /* ¿ ¡ */
    { 0x2F, DEAD_ACUTE, DEAD_DIAER, 0 },                                /* dead ´ ¨ */
    { 0x30, '+', '*', '~' }, { 0x31, '}', ']', '`' }, { 0x32, '}', ']', '`' },
    { 0x33, 0x00F1, 0x00D1, 0 },                                        /* ñ Ñ */
    { 0x34, '{', '[', '^' }, { 0x35, '|', 0x00B0, 0x00AC },             /* | ° ¬ */
    { 0x36, ',', ';', 0 }, { 0x37, '.', ':', 0 }, { 0x38, '-', '_', 0 }, { 0x64, '<', '>', 0 },
};

/* the numeric keypad, the same on both */
static const char KEYPAD[] = "/*-+\n1234567890.";      /* usages 0x54..0x63 */

int aos_hal_usb_kbd_layout(void)
{
    int32_t v = AOS_KBD_LATAM;
    aos_hal_pref_get_i32(PREF_LAYOUT, &v);
    return v == AOS_KBD_US ? AOS_KBD_US : AOS_KBD_LATAM;
}

void aos_hal_usb_kbd_layout_set(int layout)
{
    aos_hal_pref_set_i32(PREF_LAYOUT, layout == AOS_KBD_US ? AOS_KBD_US : AOS_KBD_LATAM);
}

/* a dead key and the letter after it: á é í ó ú ü, and their capitals */
static uint32_t compose(uint32_t dead, uint32_t c)
{
    static const char V[] = "aeiouAEIOU";
    static const uint32_t ACUTE[] = { 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA };
    static const uint32_t DIAER[] = { 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xC4, 0xCB, 0xCF, 0xD6, 0xDC };
    for (int i = 0; i < 10; i++)
        if (c == (uint32_t)V[i]) return dead == DEAD_ACUTE ? ACUTE[i] : DIAER[i];
    return 0;
}

static void push(uint32_t key, uint8_t mods)
{
    aos_kbd_event_t ev = { .key = key, .mods = mods };
    xQueueSend(K.q, &ev, 0);
}

static void caps_led(void);

/* A HID usage of the keyboard page pressed, with the modifiers of its
 * report: an event, or a dead key remembered for the next one. */
static void key_down(uint8_t u, uint8_t mods)
{
    bool shift = mods & 0x22, ctrl = mods & 0x11, altgr = mods & 0x40, gui = mods & 0x88;
    uint32_t key = 0;
    switch (u) {
    case 0x28: case 0x58: key = AOS_KEY_ENTER; break;
    case 0x29: key = AOS_KEY_ESC; break;
    case 0x2A: key = AOS_KEY_BACKSPACE; break;
    case 0x2B: key = shift ? AOS_KEY_PREV : AOS_KEY_NEXT; break;
    case 0x4C: key = AOS_KEY_DEL; break;
    case 0x4A: key = AOS_KEY_HOME; break;
    case 0x4D: key = AOS_KEY_END; break;
    case 0x4F: key = AOS_KEY_RIGHT; break;
    case 0x50: key = AOS_KEY_LEFT; break;
    case 0x51: key = AOS_KEY_DOWN; break;
    case 0x52: key = AOS_KEY_UP; break;
    default: break;
    }
    if (key) {
        K.dead = 0;
        push(key, mods);
        return;
    }
    if (u == 0x39) {                    /* Caps Lock, and its LED */
        K.caps = !K.caps;
        caps_led();
        return;
    }
    /* letters, upper case with Shift or Caps Lock; with AltGr only what the
     * layout's table says (q -> @ on the Latin American one) */
    if (u >= 0x04 && u <= 0x1D && !altgr) key = (shift != K.caps ? 'A' : 'a') + (u - 0x04);
    if (u >= 0x54 && u <= 0x63) key = (uint8_t)KEYPAD[u - 0x54];
    if (key == '\n') key = AOS_KEY_ENTER;
    const keymap_t *map = aos_hal_usb_kbd_layout() == AOS_KBD_US ? US : LATAM;
    size_t n = map == US ? sizeof US / sizeof US[0] : sizeof LATAM / sizeof LATAM[0];
    for (size_t i = 0; i < n; i++)
        if (map[i].usage == u) {
            key = altgr ? map[i].altgr : shift ? map[i].shift : map[i].plain;
            break;
        }
    /* Ctrl or Cmd with a key is a shortcut, not text: it goes with its mods */
    if (!key || key == AOS_KEY_ENTER || (ctrl && !altgr) || gui) {
        if (key) push(key, mods);
        return;
    }
    if (key == DEAD_ACUTE || key == DEAD_DIAER) {
        if (K.dead) {                   /* twice: the accent itself */
            push(K.dead, 0);
            K.dead = 0;
        } else K.dead = key;
        return;
    }
    if (K.dead) {
        uint32_t c = compose(K.dead, key);
        if (c) key = c;
        else {
            push(K.dead, 0);            /* no such letter: the accent, then the key */
            if (key == ' ') key = 0;    /* ´ and space: just the accent */
        }
        K.dead = 0;
        if (!key) return;
    }
    push(key, mods);
}

/* ---- the report descriptor -------------------------------------------- */

static uint8_t app_of(uint16_t page, uint16_t usage)
{
    if (page == 0x01) {
        switch (usage) {
        case 0x06: case 0x07: return APP_KEYBOARD;
        case 0x01: case 0x02: return APP_MOUSE;
        case 0x04: case 0x05: case 0x08: return APP_JOYSTICK;
        case 0x80: return APP_SYSTEM;
        }
    }
    if (page == 0x0C && usage == 0x01) return APP_CONSUMER;
    return APP_NONE;
}

static int32_t sext(uint32_t v, int bits)
{
    if (bits >= 32) return (int32_t)v;
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

/* Parses a report descriptor into the input fields (and finds the LED
 * output report of a keyboard). */
static void parse(hid_t *h, const uint8_t *d, int len)
{
    struct glob { uint16_t page; int32_t lmin, lmax; uint8_t size, count, id; } g = { 0 }, stack[4];
    int sp = 0;
    uint32_t usages[16];
    int nu = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false;
    uint8_t app = APP_NONE;
    int depth = 0;
    struct { uint8_t id; uint16_t in_bits, out_bits; } off[16];
    int noff = 0;
    h->nf = 0;
    h->ids = false;
    h->led_caps_bit = -1;

    for (int i = 0; i < len;) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                /* long item: skipped */
            if (i + 1 >= len) break;
            i += 2 + d[i];
            continue;
        }
        int sz = (b & 3) == 3 ? 4 : (b & 3);
        if (i + sz > len) break;
        uint32_t uv = 0;
        for (int k = 0; k < sz; k++) uv |= (uint32_t)d[i + k] << (8 * k);
        int32_t sv = sz ? sext(uv, 8 * sz) : 0;
        i += sz;
        uint8_t tag = b & 0xFC;
        switch (tag) {
        /* global */
        case 0x04: g.page = uv; break;
        case 0x14: g.lmin = sv; break;
        case 0x24: g.lmax = sz && g.lmin >= 0 && sv < 0 ? (int32_t)uv : sv; break;
        case 0x74: g.size = uv; break;
        case 0x94: g.count = uv; break;
        case 0x84: g.id = uv; h->ids = true; break;
        case 0xA4: if (sp < 4) stack[sp++] = g; break;
        case 0xB4: if (sp) g = stack[--sp]; break;
        /* local: a usage of 4 bytes carries its own page */
        case 0x08: if (nu < 16) usages[nu++] = sz == 4 ? uv : ((uint32_t)g.page << 16 | uv); break;
        case 0x18: umin = sz == 4 ? uv : ((uint32_t)g.page << 16 | uv); range = true; break;
        case 0x28: umax = sz == 4 ? uv : ((uint32_t)g.page << 16 | uv); range = true; break;
        /* main */
        case 0xA0:                      /* collection */
            if (uv == 1 && depth == 0) {
                uint32_t u = nu ? usages[0] : umin;
                app = app_of(u >> 16, u & 0xFFFF);
            }
            depth++;
            nu = 0;
            range = false;
            break;
        case 0xC0:                      /* end collection */
            if (depth) depth--;
            break;
        case 0x80:                      /* input */
        case 0x90: {                    /* output */
            int o = 0;
            while (o < noff && off[o].id != g.id) o++;
            if (o == noff && noff < 16) { off[noff].id = g.id; off[noff].in_bits = off[noff].out_bits = 0; noff++; }
            if (o >= 16) break;
            uint16_t *pos = tag == 0x80 ? &off[o].in_bits : &off[o].out_bits;
            if (tag == 0x90) {
                /* the LEDs of a keyboard: Caps Lock is usage 2 of page 8 */
                uint32_t first = range ? umin : nu ? usages[0] : 0;
                if ((first >> 16) == 0x08) {
                    for (int k = 0; k < g.count; k++) {
                        uint32_t u = range ? umin + k : nu ? usages[k < nu ? k : nu - 1] : 0;
                        if ((u & 0xFFFF) == 2) { h->led_id = g.id; h->led_caps_bit = *pos + k * g.size; }
                    }
                }
                *pos += g.size * g.count;
                for (int k = 0; k < noff; k++)
                    if (off[k].id == h->led_id && h->led_caps_bit >= 0) h->led_bytes = (off[k].out_bits + 7) / 8;
                nu = 0;
                range = false;
                break;
            }
            bool constant = uv & 1, var = uv & 2;
            if (!constant && g.size && g.size <= 32 && h->nf < FIELD_MAX) {
                /* variables are split into a field each, with its own usage
                 * (X, Y, the wheel), but for runs of buttons and keys, kept
                 * as one field over a range */
                uint16_t pg = range ? umin >> 16 : nu ? usages[0] >> 16 : g.page;
                if (var && (nu || range) && pg != 0x07 && pg != 0x09 && g.count <= 16) {
                    for (int k = 0; k < g.count && h->nf < FIELD_MAX; k++) {
                        uint32_t u = range ? (umin + k > umax ? umax : umin + k) : usages[k < nu ? k : nu - 1];
                        h->f[h->nf++] = (field_t){ .page = u >> 16, .usage = u, .usage_max = u,
                            .bit = *pos + k * g.size, .size = g.size, .count = 1, .report_id = g.id,
                            .app = app, .flags = uv, .lmin = g.lmin, .lmax = g.lmax };
                    }
                } else {
                    uint32_t lo = range ? umin : nu ? usages[0] : 0, hi = range ? umax : nu ? usages[nu - 1] : 0;
                    if (!range && !nu) lo = hi = (uint32_t)g.page << 16;
                    h->f[h->nf++] = (field_t){ .page = lo >> 16, .usage = lo, .usage_max = hi, .bit = *pos,
                        .size = g.size, .count = g.count, .report_id = g.id, .app = app, .flags = uv,
                        .lmin = g.lmin, .lmax = g.lmax };
                }
                h->apps |= 1 << app;
            }
            *pos += g.size * g.count;
            nu = 0;
            range = false;
            break;
        }
        case 0xB0:                      /* feature: not read */
            nu = 0;
            range = false;
            break;
        default: break;
        }
    }
}

/* the boot protocol's layouts, as fields */
static void boot_fields(hid_t *h)
{
    h->nf = 0;
    h->ids = false;
    if (h->proto == 1) {
        h->f[h->nf++] = (field_t){ 0x07, 0xE0, 0xE7, 0, 1, 8, 0, APP_KEYBOARD, 2, 0, 1 };
        h->f[h->nf++] = (field_t){ 0x07, 0x00, 0xFF, 16, 8, 6, 0, APP_KEYBOARD, 0, 0, 255 };
        h->apps = 1 << APP_KEYBOARD;
        h->led_id = 0;
        h->led_bytes = 1;
        h->led_caps_bit = 1;
    } else {
        h->f[h->nf++] = (field_t){ 0x09, 1, 3, 0, 1, 3, 0, APP_MOUSE, 2, 0, 1 };
        h->f[h->nf++] = (field_t){ 0x01, 0x30, 0x30, 8, 8, 1, 0, APP_MOUSE, 6, -127, 127 };
        h->f[h->nf++] = (field_t){ 0x01, 0x31, 0x31, 16, 8, 1, 0, APP_MOUSE, 6, -127, 127 };
        h->apps = 1 << APP_MOUSE;
    }
}

/* ---- the reports ------------------------------------------------------ */

static uint32_t bits(const uint8_t *r, int len, int bit, int size)
{
    uint32_t v = 0;
    for (int k = 0; k < size; k++) {
        int b = bit + k;
        if (b / 8 >= len) break;
        if (r[b / 8] >> (b % 8) & 1) v |= 1u << k;
    }
    return v;
}

static int32_t value(const field_t *f, const uint8_t *r, int len, int k)
{
    uint32_t v = bits(r, len, f->bit + k * f->size, f->size);
    return f->lmin < 0 ? sext(v, f->size) : (int32_t)v;
}

/* an axis from its logical range to -32767..32767 */
static int16_t scale(const field_t *f, int32_t v)
{
    if (f->lmax <= f->lmin) return 0;
    int64_t c = (int64_t)(v - f->lmin) * 65534 / (f->lmax - f->lmin) - 32767;
    return c < -32767 ? -32767 : c > 32767 ? 32767 : (int16_t)c;
}

/* whether a report ID carries fields of an application (a receiver sends
 * its keyboard, mouse and media keys as different reports: one of the mouse
 * must not read as the keyboard with every key up) */
static bool carries(const hid_t *h, uint8_t id, uint8_t app, uint16_t page)
{
    for (int i = 0; i < h->nf; i++)
        if (h->f[i].report_id == id && h->f[i].app == app && (!page || h->f[i].page == page)) return true;
    return false;
}

static void report_keyboard(hid_t *h, const uint8_t *r, int len, uint8_t id)
{
    if (!carries(h, id, APP_KEYBOARD, 0x07)) return;
    uint8_t now[KEYS_MAX], n = 0, mods = 0;
    for (int i = 0; i < h->nf; i++) {
        const field_t *f = &h->f[i];
        if (f->app != APP_KEYBOARD || f->report_id != id || f->page != 0x07) continue;
        if (f->flags & 2) {
            for (int k = 0; k < f->count; k++) {
                if (!bits(r, len, f->bit + k * f->size, f->size)) continue;
                uint16_t u = f->usage + (f->usage_max > f->usage ? k : 0);
                if (u >= 0xE0 && u <= 0xE7) mods |= 1 << (u - 0xE0);
                else if (n < KEYS_MAX && u >= 4) now[n++] = u;
            }
        } else {
            for (int k = 0; k < f->count; k++) {
                int32_t v = value(f, r, len, k);
                if (v < f->lmin || v > f->lmax) continue;
                uint32_t u = (f->usage & 0xFFFF) + (v - f->lmin);
                if (u == 1) return;                     /* rollover: too many keys, nothing to read */
                if (u >= 0xE0 && u <= 0xE7) mods |= 1 << (u - 0xE0);
                else if (n < KEYS_MAX && u >= 4) now[n++] = u;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (memchr(h->keys, now[i], h->nkeys)) continue;
        key_down(now[i], mods);
        K.held_code = now[i];
        K.held_next = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    }
    if (K.held_code && !memchr(now, K.held_code, n)) K.held_code = 0;
    K.held_mods = mods;
    memcpy(h->keys, now, n);
    h->nkeys = n;
    h->mods = mods;
}

static void report_consumer(hid_t *h, const uint8_t *r, int len, uint8_t id)
{
    if (!carries(h, id, APP_CONSUMER, 0x0C) && !carries(h, id, APP_KEYBOARD, 0x0C)) return;
    uint16_t now[4];
    int n = 0;
    for (int i = 0; i < h->nf; i++) {
        const field_t *f = &h->f[i];
        if (f->report_id != id || f->page != 0x0C || (f->app != APP_CONSUMER && f->app != APP_KEYBOARD)) continue;
        for (int k = 0; k < f->count && n < 4; k++) {
            if (f->flags & 2) {
                if (bits(r, len, f->bit + k * f->size, f->size)) now[n++] = f->usage + (f->usage_max > f->usage ? k : 0);
            } else {
                int32_t v = value(f, r, len, k);
                if (v > 0 && v >= f->lmin && v <= f->lmax) now[n++] = (f->usage & 0xFFFF) + (v - f->lmin);
            }
        }
    }
    for (int i = 0; i < n; i++) {
        bool was = false;
        for (int k = 0; k < 4; k++) was |= h->cons[k] == now[i];
        if (!was && now[i]) push(AOS_KEY_CONSUMER + now[i], 0);
    }
    memset(h->cons, 0, sizeof h->cons);
    memcpy(h->cons, now, n * sizeof now[0]);
}

static void report_mouse(hid_t *h, const uint8_t *r, int len, uint8_t id)
{
    aos_mouse_event_t m = { 0 };
    bool any = false;
    for (int i = 0; i < h->nf; i++) {
        const field_t *f = &h->f[i];
        if (f->app != APP_MOUSE || f->report_id != id) continue;
        any = true;
        if (f->page == 0x09) {
            for (int k = 0; k < f->count; k++)
                if (bits(r, len, f->bit + k * f->size, f->size)) {
                    int btn = f->usage + (f->usage_max > f->usage ? k : 0);
                    if (btn >= 1 && btn <= 8) m.buttons |= 1 << (btn - 1);
                }
            continue;
        }
        int32_t v = value(f, r, len, 0);
        bool rel = f->flags & 4;
        uint16_t u = f->usage & 0xFFFF;
        if (f->page == 0x01 && (u == 0x30 || u == 0x31)) {
            if (rel) {
                if (u == 0x30) m.dx += v; else m.dy += v;
            } else {
                m.absolute = true;
                uint16_t a = (uint16_t)((int64_t)(v - f->lmin) * 65535 / (f->lmax > f->lmin ? f->lmax - f->lmin : 1));
                if (u == 0x30) m.x = a; else m.y = a;
            }
        } else if (f->page == 0x01 && u == 0x38) m.wheel += v;
        else if (f->page == 0x0C && u == 0x238) m.pan += v;
    }
    if (!any) return;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    K.mouse.dx += m.dx;
    K.mouse.dy += m.dy;
    K.mouse.wheel += m.wheel;
    K.mouse.pan += m.pan;
    K.mouse.buttons = m.buttons;
    if (m.absolute) {
        K.mouse.absolute = true;
        K.mouse.x = m.x;
        K.mouse.y = m.y;
    }
    K.mouse_new = true;
    xSemaphoreGive(K.mx);
}

static void report_pad(hid_t *h, const uint8_t *r, int len, uint8_t id)
{
    if (h->pad < 0) return;
    aos_gamepad_t p;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    p = K.pad[h->pad];
    xSemaphoreGive(K.mx);
    bool any = false;
    for (int i = 0; i < h->nf; i++) {
        const field_t *f = &h->f[i];
        if (f->app != APP_JOYSTICK || f->report_id != id) continue;
        any = true;
        uint16_t u = f->usage & 0xFFFF;
        if (f->page == 0x09) {
            for (int k = 0; k < f->count; k++) {
                int btn = f->usage + (f->usage_max > f->usage ? k : 0);
                if (btn < 1 || btn > 32) continue;
                if (bits(r, len, f->bit + k * f->size, f->size)) p.buttons |= 1u << (btn - 1);
                else p.buttons &= ~(1u << (btn - 1));
            }
        } else if (f->page == 0x01 && u >= 0x30 && u <= 0x37) {
            p.axis[u - 0x30] = scale(f, value(f, r, len, 0));
        } else if (f->page == 0x01 && u == 0x39) {
            int32_t v = value(f, r, len, 0);
            int span = f->lmax - f->lmin;
            if (v < f->lmin || v > f->lmax) p.hat = -1;
            else p.hat = span == 3 ? (int8_t)((v - f->lmin) * 2) : (int8_t)(v - f->lmin);
        } else if (f->page == 0x02 && (u == 0xC4 || u == 0xC5)) {     /* accelerator, brake */
            p.axis[u == 0xC4 ? 6 : 7] = scale(f, value(f, r, len, 0));
        }
    }
    if (!any) return;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    K.pad[h->pad].buttons = p.buttons;
    memcpy(K.pad[h->pad].axis, p.axis, sizeof p.axis);
    K.pad[h->pad].hat = p.hat;
    K.pad[h->pad].reports++;
    xSemaphoreGive(K.mx);
}

static void in_done(usb_transfer_t *t)
{
    hid_t *h = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
        const uint8_t *r = t->data_buffer;
        int len = t->actual_num_bytes;
        uint8_t id = 0;
        if (h->ids) {
            id = r[0];
            r++;
            len--;
        }
        if (h->apps & (1 << APP_KEYBOARD)) report_keyboard(h, r, len, id);
        if (h->apps & (1 << APP_CONSUMER | 1 << APP_KEYBOARD)) report_consumer(h, r, len, id);
        if (h->apps & (1 << APP_MOUSE)) report_mouse(h, r, len, id);
        if (h->apps & (1 << APP_JOYSTICK)) report_pad(h, r, len, id);
    }
    bool again = t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED &&
                 !h->gone && !K.stop;
    h->in_busy = again && usb_host_transfer_submit(t) == ESP_OK;
}

/* ---- the steps -------------------------------------------------------- */

static void ctrl_done(usb_transfer_t *t);

static bool control(hid_t *h, usb_transfer_t *t, uint8_t type, uint8_t req, uint16_t value, uint16_t length)
{
    uint8_t *b = t->data_buffer;
    b[0] = type;
    b[1] = req;
    b[2] = value & 0xFF;
    b[3] = value >> 8;
    b[4] = h->intf;
    b[5] = 0;
    b[6] = length & 0xFF;
    b[7] = length >> 8;
    t->num_bytes = 8 + length;
    t->device_handle = h->dev;
    t->bEndpointAddress = 0;
    t->context = h;
    return usb_host_transfer_submit_control(K.client, t) == ESP_OK;
}

static const char *const APP_NAME[] = { "", "keyboard", "mouse", "gamepad", "media keys", "system keys" };

static void start_reading(hid_t *h)
{
    h->step = 2;
    if (h->apps & (1 << APP_JOYSTICK)) {
        xSemaphoreTake(K.mx, portMAX_DELAY);
        for (int i = 0; i < AOS_GAMEPAD_MAX && h->pad < 0; i++)
            if (!K.pad[i].connected) {
                memset(&K.pad[i], 0, sizeof K.pad[i]);
                K.pad[i].connected = true;
                K.pad[i].hat = -1;
                snprintf(K.pad[i].name, sizeof K.pad[i].name, "%s", h->name);
                h->pad = i;
            }
        /* what the pad has, from its fields */
        if (h->pad >= 0)
            for (int i = 0; i < h->nf; i++) {
                const field_t *f = &h->f[i];
                if (f->app != APP_JOYSTICK) continue;
                uint16_t u = f->usage & 0xFFFF;
                if (f->page == 0x01 && u >= 0x30 && u <= 0x37) K.pad[h->pad].axes |= 1 << (u - 0x30);
                if (f->page == 0x02 && (u == 0xC4 || u == 0xC5)) K.pad[h->pad].axes |= 1 << (u == 0xC4 ? 6 : 7);
                if (f->page == 0x01 && u == 0x39) K.pad[h->pad].has_hat = true;
                if (f->page == 0x09) {
                    int top = f->usage_max > f->usage ? f->usage_max : f->usage + f->count - 1;
                    if (top > K.pad[h->pad].nbuttons) K.pad[h->pad].nbuttons = top > 32 ? 32 : top;
                }
            }
        xSemaphoreGive(K.mx);
    }
    if (h->apps & (1 << APP_MOUSE)) K.mice++;
    char what[48] = "";
    size_t o = 0;
    for (int a = APP_KEYBOARD; a <= APP_SYSTEM; a++)
        if (h->apps & (1 << a)) {
            o += snprintf(what + o, sizeof what - o, "%s%s", o ? ", " : "", APP_NAME[a]);
            if (o >= sizeof what) break;
        }
    aos_p4_usb_dev_use(h->addr, what[0] ? what : "HID");
    h->in->num_bytes = h->mps;
    h->in->device_handle = h->dev;
    h->in->bEndpointAddress = h->ep;
    h->in->callback = in_done;
    h->in->context = h;
    esp_err_t e = usb_host_transfer_submit(h->in);
    h->in_busy = e == ESP_OK;
    ESP_LOGI(TAG, "HID \"%s\" interface %u: %s%s, %d fields%s: %s", h->name, h->intf, what[0] ? what : "nothing known",
             h->boot ? " (boot protocol)" : "", h->nf, h->ids ? ", report IDs" : "", e == ESP_OK ? "reading" : esp_err_to_name(e));
}

static void ctrl_done(usb_transfer_t *t)
{
    hid_t *h = t->context;
    h->ctrl_busy = false;
    if (h->gone || K.stop) return;
    bool ok = t->status == USB_TRANSFER_STATUS_COMPLETED;
    if (h->step == 0) {
        int got = t->actual_num_bytes - 8;
        if (ok && got > 0) parse(h, t->data_buffer + 8, got);
        if (!h->nf && h->sub == 1 && (h->proto == 1 || h->proto == 2)) {
            /* no descriptor to go by: the boot protocol's fixed reports */
            h->boot = true;
            boot_fields(h);
            h->step = 1;
            h->ctrl_busy = control(h, h->ctrl, 0x21, 0x0B, 0, 0);       /* SET_PROTOCOL: boot */
            return;
        }
        if (!h->nf) {
            ESP_LOGW(TAG, "HID \"%s\" interface %u: no report descriptor (%d bytes, status %d)", h->name, h->intf, got,
                     t->status);
            return;
        }
        h->step = 1;
        h->ctrl_busy = control(h, h->ctrl, 0x21, 0x0A, 0, 0);           /* SET_IDLE: only on change */
        if (!h->ctrl_busy) start_reading(h);
        return;
    }
    if (h->step == 1) start_reading(h);         /* a stall on SET_IDLE is fine: many devices refuse it */
}

/* Caps Lock's LED on every keyboard that has one (SET_REPORT, output) */
static void led_done(usb_transfer_t *t)
{
    ((hid_t *)t->context)->led_busy = false;
}

static void caps_led(void)
{
    for (int i = 0; i < IF_MAX; i++) {
        hid_t *h = &K.h[i];
        if (!h->dev || h->gone || h->led_caps_bit < 0 || !h->led || h->led_busy || h->step != 2) continue;
        int n = h->led_bytes + (h->led_id ? 1 : 0);
        if (n < 1 || n > 8) continue;
        uint8_t *p = h->led->data_buffer + 8;
        memset(p, 0, n);
        int at = 0;
        if (h->led_id) p[at++] = h->led_id;
        if (K.caps) p[at + h->led_caps_bit / 8] |= 1 << (h->led_caps_bit % 8);
        h->led->callback = led_done;
        h->led_busy = control(h, h->led, 0x21, 0x09, 0x0200 | h->led_id, n);
    }
}

/* ---- devices ---------------------------------------------------------- */

static void wide_to_str(const usb_str_desc_t *d, char *out, size_t n)
{
    size_t i = 0;
    if (d)
        for (int k = 0; k < (d->bLength - 2) / 2 && i + 1 < n; k++) {
            uint16_t w = d->wData[k];
            out[i++] = w >= 32 && w < 127 ? (char)w : '?';
        }
    out[i] = 0;
    /* some pad their names with spaces (" USB gamepad           ") */
    while (i && out[i - 1] == ' ') out[--i] = 0;
    size_t lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead) memmove(out, out + lead, i - lead + 1);
}

static void hid_free(hid_t *h)
{
    if (h->in) usb_host_transfer_free(h->in);
    if (h->ctrl) usb_host_transfer_free(h->ctrl);
    if (h->led) usb_host_transfer_free(h->led);
    if (h->dev) {
        usb_host_interface_release(K.client, h->dev, h->intf);
        /* the device is closed once, with its last interface */
        bool other = false;
        for (int i = 0; i < IF_MAX; i++) other |= &K.h[i] != h && K.h[i].dev == h->dev;
        if (!other) usb_host_device_close(K.client, h->dev);
    }
    if (h->apps & (1 << APP_MOUSE) && h->step == 2 && K.mice) K.mice--;
    field_t *f = h->f;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    if (h->pad >= 0) K.pad[h->pad].connected = false;
    memset(h, 0, sizeof *h);
    h->f = f;
    h->pad = -1;
    xSemaphoreGive(K.mx);
}

/* A new device: each of its HID interfaces is taken. */
static void dev_new(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(K.client, addr, &dev) != ESP_OK) return;
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) { usb_host_device_close(K.client, dev); return; }
    usb_device_info_t di;
    char name[48] = "", product[40] = "", vendor[24] = "";
    if (usb_host_device_info(dev, &di) == ESP_OK) {
        wide_to_str(di.str_desc_manufacturer, vendor, sizeof vendor);
        wide_to_str(di.str_desc_product, product, sizeof product);
    }
    snprintf(name, sizeof name, "%s%s%s", vendor, *vendor && *product ? " " : "", product);
    if (!name[0]) snprintf(name, sizeof name, "USB device %u", addr);
    int taken = 0, off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    hid_t *h = NULL;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *it = (const usb_intf_desc_t *)d;
            h = NULL;
            if (it->bInterfaceClass != USB_CLASS_HID || it->bAlternateSetting != 0) continue;
            for (int i = 0; i < IF_MAX && !h; i++) if (!K.h[i].dev) h = &K.h[i];
            if (!h) { ESP_LOGW(TAG, "HID: more than %d interfaces, left alone", IF_MAX); break; }
            field_t *f = h->f;
            memset(h, 0, sizeof *h);
            h->f = f;
            h->pad = -1;
            h->led_caps_bit = -1;
            h->dev = dev;
            h->addr = addr;
            h->intf = it->bInterfaceNumber;
            h->sub = it->bInterfaceSubClass;
            h->proto = it->bInterfaceProtocol;
            snprintf(h->name, sizeof h->name, "%s", name);
        } else if (d->bDescriptorType == 0x21 && h && d->bLength >= 9) {          /* the HID descriptor */
            const uint8_t *b = (const uint8_t *)d;
            h->desc_len = b[7] | b[8] << 8;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && h && !h->ep) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if (USB_EP_DESC_GET_XFERTYPE(e) != USB_TRANSFER_TYPE_INTR || !USB_EP_DESC_GET_EP_DIR(e)) continue;
            h->ep = e->bEndpointAddress;
            h->mps = USB_EP_DESC_GET_MPS(e);
            if (!h->f) h->f = heap_caps_malloc(FIELD_MAX * sizeof(field_t), MALLOC_CAP_SPIRAM);
            int dl = h->desc_len && h->desc_len <= DESC_MAX ? h->desc_len : DESC_MAX;
            if (!h->f || usb_host_interface_claim(K.client, dev, h->intf, 0) != ESP_OK ||
                usb_host_transfer_alloc(8 + DESC_MAX, 0, &h->ctrl) != ESP_OK ||
                usb_host_transfer_alloc(16, 0, &h->led) != ESP_OK ||
                usb_host_transfer_alloc(h->mps < 8 ? 8 : h->mps, 0, &h->in) != ESP_OK) {
                ESP_LOGW(TAG, "HID \"%s\" interface %u: could not claim it", name, h->intf);
                if (h->in) usb_host_transfer_free(h->in);
                if (h->ctrl) usb_host_transfer_free(h->ctrl);
                if (h->led) usb_host_transfer_free(h->led);
                usb_host_interface_release(K.client, dev, h->intf);
                field_t *f = h->f;
                memset(h, 0, sizeof *h);
                h->f = f;
                h->pad = -1;
                h = NULL;
                continue;
            }
            taken++;
            h->ctrl->callback = ctrl_done;
            h->step = 0;
            h->ctrl_busy = control(h, h->ctrl, 0x81, 0x06, 0x2200, dl);       /* GET_DESCRIPTOR: report */
            h = NULL;
        }
    }
    if (!taken) usb_host_device_close(K.client, dev);
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        dev_new(msg->new_dev.address);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (int i = 0; i < IF_MAX; i++)
            if (K.h[i].dev == msg->dev_gone.dev_hdl && !K.h[i].gone) {
                ESP_LOGI(TAG, "HID \"%s\" interface %u gone", K.h[i].name, K.h[i].intf);
                K.h[i].gone = true;
                K.held_code = 0;
            }
    }
}

/* An interface that is gone (or stopping) is freed here, outside the
 * client's callback: its transfers halted and flushed first, and only once
 * their callbacks have run. */
static bool reap(void)
{
    bool left = false;
    for (int i = 0; i < IF_MAX; i++) {
        hid_t *h = &K.h[i];
        if (!h->dev || !h->gone) continue;
        if ((h->in_busy || h->ctrl_busy || h->led_busy) && !h->halted) {
            h->halted = true;
            if (h->ep && usb_host_endpoint_halt(h->dev, h->ep) == ESP_OK) usb_host_endpoint_flush(h->dev, h->ep);
        }
        if (h->in_busy || h->ctrl_busy || h->led_busy) {
            left = true;
            continue;
        }
        hid_free(h);
    }
    return left;
}

static void hid_task(void *arg)
{
    (void)arg;
    while (!K.stop) {
        usb_host_client_handle_events(K.client, pdMS_TO_TICKS(20));
        reap();
        /* the key held repeats */
        if (K.held_code && (int32_t)(xTaskGetTickCount() - K.held_next) >= 0) {
            key_down(K.held_code, K.held_mods);
            K.held_next = xTaskGetTickCount() + pdMS_TO_TICKS(40);
        }
    }
    for (int i = 0; i < IF_MAX; i++) if (K.h[i].dev) K.h[i].gone = true;
    for (int i = 0; i < 50 && reap(); i++) usb_host_client_handle_events(K.client, pdMS_TO_TICKS(20));
    usb_host_client_deregister(K.client);
    K.client = NULL;
    K.done = true;
    vTaskDelete(NULL);
}

/* aos_usb_p4.c: with the host library up, and before it goes down */
bool aos_p4_usb_hid_start(void)
{
    if (!K.q) K.q = xQueueCreate(32, sizeof(aos_kbd_event_t));
    if (!K.mx) K.mx = xSemaphoreCreateMutex();
    K.stop = K.done = false;
    K.held_code = 0;
    K.dead = 0;
    K.caps = false;
    K.mice = 0;
    memset(&K.mouse, 0, sizeof K.mouse);
    memset(K.pad, 0, sizeof K.pad);
    for (int i = 0; i < IF_MAX; i++) {
        field_t *f = K.h[i].f;
        memset(&K.h[i], 0, sizeof K.h[i]);
        K.h[i].f = f;
        K.h[i].pad = -1;
    }
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 8,
                                          .async = { .client_event_callback = client_event } };
    if (usb_host_client_register(&cc, &K.client) != ESP_OK) return false;
    if (xTaskCreatePinnedToCore(hid_task, "usb_hid", 4096, NULL, 4, NULL, 0) != pdPASS) {
        usb_host_client_deregister(K.client);
        K.client = NULL;
        return false;
    }
    return true;
}

void aos_p4_usb_hid_stop(void)
{
    if (!K.client) return;
    K.stop = true;
    for (int i = 0; i < 100 && !K.done; i++) {
        usb_host_client_unblock(K.client);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---- the API ---------------------------------------------------------- */

bool aos_hal_usb_kbd_read(aos_kbd_event_t *ev) { return K.q && xQueueReceive(K.q, ev, 0) == pdTRUE; }

int aos_hal_usb_kbd_list(char names[][48], int max)
{
    if (!K.mx) return 0;
    int n = 0;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    for (int i = 0; i < IF_MAX && n < max; i++) {
        const hid_t *h = &K.h[i];
        if (!h->dev || h->gone || h->step != 2 || !(h->apps & (1 << APP_KEYBOARD))) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= strcmp(names[k], h->name) == 0;
        if (!dup) snprintf(names[n++], 48, "%s", h->name);
    }
    xSemaphoreGive(K.mx);
    return n;
}

bool aos_hal_hid_mouse_present(void) { return K.mice > 0; }

bool aos_hal_hid_mouse_read(aos_mouse_event_t *ev)
{
    if (!K.mx || !K.mouse_new) return false;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    *ev = K.mouse;
    K.mouse.dx = K.mouse.dy = K.mouse.wheel = K.mouse.pan = 0;
    K.mouse_new = false;
    xSemaphoreGive(K.mx);
    return true;
}

int aos_hal_hid_gamepad_count(void)
{
    int n = 0;
    for (int i = 0; i < AOS_GAMEPAD_MAX; i++) n += K.pad[i].connected;
    return n;
}

bool aos_hal_hid_gamepad_get(int index, aos_gamepad_t *out)
{
    if (!K.mx || index < 0 || index >= AOS_GAMEPAD_MAX) return false;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    *out = K.pad[index];
    xSemaphoreGive(K.mx);
    return out->connected;
}

uint8_t aos_hal_hid_gamepad_dpad(const aos_gamepad_t *p)
{
    static const uint8_t HAT[8] = { AOS_DPAD_UP, AOS_DPAD_UP | AOS_DPAD_RIGHT, AOS_DPAD_RIGHT,
                                    AOS_DPAD_DOWN | AOS_DPAD_RIGHT, AOS_DPAD_DOWN, AOS_DPAD_DOWN | AOS_DPAD_LEFT,
                                    AOS_DPAD_LEFT, AOS_DPAD_UP | AOS_DPAD_LEFT };
    uint8_t d = p->hat >= 0 && p->hat < 8 ? HAT[p->hat] : 0;
    /* and the left stick, past half way */
    if (p->axes & 1) {
        if (p->axis[0] < -16384) d |= AOS_DPAD_LEFT;
        if (p->axis[0] > 16384) d |= AOS_DPAD_RIGHT;
    }
    if (p->axes & 2) {
        if (p->axis[1] < -16384) d |= AOS_DPAD_UP;
        if (p->axis[1] > 16384) d |= AOS_DPAD_DOWN;
    }
    return d;
}
