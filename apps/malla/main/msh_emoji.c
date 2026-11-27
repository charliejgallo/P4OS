/*
 * Malla - emoji in, emoji out.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * The system draws an emoji as one private-use code point (U+F0000 + its
 * place in the pack, docs/EMOJI.md): aos_text_safe() makes them from UTF-8
 * for what is shown. A text area shows the same code points, so the picker
 * puts that code point in it, and here it is remembered which UTF-8 each
 * one stood for; on sending, msh_emoji_to_utf8() puts the real emoji back,
 * which is what the other end's phone or node understands.
 *
 * Without the pack (aos_emoji_ready() false) the picker writes the UTF-8
 * itself: it does not show in the field, but it travels right.
 */
#include "msh_ui.h"

#include "aos_emoji.h"

#include <stdio.h>
#include <string.h>

/* The ones a mesh uses most: answers, the weather, the way, the battery. */
static const char *const PICK[] = {
    "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\x8A", "\xF0\x9F\x98\xAE",
    "\xF0\x9F\x98\xA2", "\xF0\x9F\x99\x8F", "\xF0\x9F\x91\x8B", "\xF0\x9F\x91\x8C", "\xF0\x9F\x8E\x89",
    "\xF0\x9F\x94\xA5", "\xE2\x9C\x85", "\xE2\x9D\x8C", "\xE2\x9A\xA0\xEF\xB8\x8F", "\xF0\x9F\x93\x8D",
    "\xF0\x9F\x8F\xA0", "\xF0\x9F\x9A\x97", "\xE2\x9B\xB0\xEF\xB8\x8F", "\xE2\x98\x80\xEF\xB8\x8F", "\xF0\x9F\x8C\xA7\xEF\xB8\x8F",
    "\xE2\x9B\xBA", "\xF0\x9F\x93\xA1", "\xF0\x9F\x94\x8B", "\xE2\x8F\xB0", "\xF0\x9F\x8D\xBA",
    "\xE2\x98\x95", "\xF0\x9F\xA4\x99", "\xF0\x9F\x98\x8E", "\xF0\x9F\xA4\x94", "\xF0\x9F\x92\xAA",
    "\xF0\x9F\x86\x98", "\xF0\x9F\x91\x80",
};
#define N_PICK (int)(sizeof PICK / sizeof PICK[0])

/* the six of a reaction */
static const int REACT[] = { 0, 1, 2, 4, 5, 6 };

/* code point -> UTF-8, for what the picker put */
static struct { uint32_t cp; char utf8[16]; } s_map[64];
static int s_map_n;

static void remember(uint32_t cp, const char *utf8)
{
    for (int i = 0; i < s_map_n; i++)
        if (s_map[i].cp == cp) return;
    if (s_map_n >= (int)(sizeof s_map / sizeof s_map[0])) return;
    s_map[s_map_n].cp = cp;
    msh_scpy(s_map[s_map_n].utf8, sizeof s_map[s_map_n].utf8, utf8);
    s_map_n++;
}

static int put_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* What to put in a text area for this emoji: its code point when the pack
 * draws it, else its UTF-8. */
static void for_field(const char *utf8, char *out, size_t cap)
{
    uint32_t cp = 0;
    if (aos_emoji_ready() && aos_emoji_match(utf8, strlen(utf8), &cp) && cp) {
        remember(cp, utf8);
        char b[5];
        int n = put_utf8(cp, b);
        b[n] = 0;
        msh_scpy(out, cap, b);
    } else {
        msh_scpy(out, cap, utf8);
    }
}

void msh_emoji_to_utf8(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < cap) {
        if (p[0] == 0xF3 && p[1] && p[2] && p[3]) {
            /* U+F0000..U+FFFFF are 0xF3 0xB0..0xBF ...: plane 15 */
            uint32_t cp = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12
                        | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            const char *rep = NULL;
            for (int i = 0; i < s_map_n && !rep; i++)
                if (s_map[i].cp == cp) rep = s_map[i].utf8;
            if (rep) {
                size_t l = strlen(rep);
                if (o + l >= cap) break;
                memcpy(out + o, rep, l);
                o += l;
                p += 4;
                continue;
            }
        }
        out[o++] = (char)*p++;
    }
    out[o] = 0;
}

/* ---- the picker ---- */

static lv_obj_t *s_ta;

static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_ta && i >= 0 && i < N_PICK) {
        char f[20];
        for_field(PICK[i], f, sizeof f);
        lv_textarea_add_text(s_ta, f);
    }
    msh_ui_sheet_close();
}

static lv_obj_t *grid(lv_obj_t *p)
{
    lv_obj_t *g = lv_obj_create(p);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(g, 12, 0);
    lv_obj_set_style_pad_column(g, 12, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    return g;
}

static void emoji_btn(lv_obj_t *g, int i, lv_event_cb_t cb, void *ud)
{
    char safe[24];
    msh_ui_safe(safe, sizeof safe, PICK[i]);
    lv_obj_t *b = lv_button_create(g);
    lv_obj_set_size(b, 96, 96);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = msh_ui_label(b, safe, aos_font_title, AOS_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
}

void msh_emoji_sheet(lv_obj_t *ta)
{
    s_ta = ta;
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_EMOJI, NULL);
    if (!aos_emoji_ready())
        msh_ui_label(p, _("Sin el paquete de emojis en la tarjeta se mandan igual, pero no se ven."), aos_font_caption, AOS_C_DIM);
    lv_obj_t *g = grid(p);
    for (int i = 0; i < N_PICK; i++) emoji_btn(g, i, pick_cb, (void *)(intptr_t)i);
}

/* ---- reactions ---- */

static uint32_t s_react_to;

static void react_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    msh_lock();
    msh_msg_t *m = msh_msg_by_id(s_react_to);
    uint32_t to = 0;
    int ch = 0;
    if (m) {
        to = m->peer ? m->peer : MSH_BROADCAST;
        ch = m->ch;
    }
    msh_unlock();
    msh_ui_sheet_close();
    if (!m) return;
    if (!msh_send_text(to, ch, PICK[i], s_react_to, true)) aos_ui_toast(_("No se pudo encolar el mensaje"), 1800);
    msh_ui_chat_refresh();
}

void msh_reaction_sheet(uint32_t msg_id)
{
    s_react_to = msg_id;
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_REACT, _("Reaccionar"));
    lv_obj_t *g = grid(p);
    for (size_t k = 0; k < sizeof REACT / sizeof REACT[0]; k++) emoji_btn(g, REACT[k], react_cb, (void *)(intptr_t)REACT[k]);
}
