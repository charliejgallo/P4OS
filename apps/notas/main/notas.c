/*
 * P4OS - Notes: notes with styles, and task lists.
 *
 * Three screens in one root: the notes (cards in a grid or a list, pinned
 * first, filtered and searched), a note open in the rich-text editor
 * (nt_editor.c) with its formatting bar, and a task list (nt_tasks.c). The
 * bin is a fourth. Each screen is built from scratch when it is entered, or
 * when the screen turns; what has to survive that lives in S and in the
 * editor's own state.
 *
 * Notes are Markdown files in /sdcard/notas (nt_store.c, nt_doc.c), saved
 * two seconds after the last change and when the note is left. A note left
 * empty is not kept.
 *
 * Fonts: notas_p4.pak (tools/pack_fonts.py) brings Inter in bold, italic
 * and both; without it the app works in the firmware's Medium.
 */
#include "nt.h"
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* A notepad: the page, its yellow head and three lines of writing. */
static const uint8_t NOTAS_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 2, 58, 68, 9, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, 0, 9, 42, 7, 3, AIC_C_LIT(0xFF9F0A), 255),
    AIC_RECT(AIC_TOP_LEFT, 8, 26, 42, 4, 2, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_LEFT, 8, 37, 42, 4, 2, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_LEFT, 8, 48, 26, 4, 2, AIC_C_DIM, 255),
    AIC_OUT,
    AIC_END
};

extern lv_obj_t *nt_ui_root;

enum { SCR_LIST, SCR_NOTE, SCR_TASKS, SCR_TRASH };
enum { P_NONE, P_KB, P_FORMAT, P_LIST, P_COLOR };
enum { F_ALL, F_NOTES, F_TASKS };

#define SAVE_AFTER_MS   2000

static struct {
    lv_obj_t   *root;
    int32_t     W, H;
    bool        land;
    int         screen;

    /* the notes */
    nt_index_t  ix;
    int         filter, order;
    bool        rows;               /* list view instead of the grid */
    char        query[64];
    bool        searching;
    lv_obj_t   *page, *cards, *search_ta, *kb, *kb_area, *fab;
    int32_t     list_scroll;
    int         menu_entry;

    /* the open note */
    nt_doc_t    doc;
    nt_meta_t   meta;
    char        file[96];
    bool        open, dirty;
    uint32_t    dirty_ms;
    int         panel;
    lv_obj_t   *topbar, *toolbar, *btn_undo, *btn_redo;
    lv_obj_t   *tb_b, *tb_i, *tb_u, *tb_fg, *tb_hl, *tb_list, *tb_check, *tb_kb, *tb_aa;
    int32_t     kb_h;

    /* what the card had when we last read or wrote it (the portal edits it too) */
    uint32_t    f_mtime, f_size, dir_sig;
    int         ticks;

    /* typing into a computer */
    volatile bool typing, typing_stop;
} S;

static void build(void);
static void watch_card(void);
static void list_search_attach(void);
static void tasks_keyboard(bool show);
static void open_entry(int i);
static void close_note(void);
static void show_panel(int p);

/* -------------------------------------------------------------------------- */
/* Small things                                                                */
/* -------------------------------------------------------------------------- */

static const lv_font_t *F(int style, int size)
{
    return nt_font(style, size);
}

static void fmt_date(uint32_t t, char *out, size_t n)
{
    if (!t) { snprintf(out, n, "%s", ""); return; }
    time_t tt = (time_t)t, now = time(NULL);
    struct tm a, b;
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    if (aos_hal_time_is_valid() && a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        snprintf(out, n, "%02d:%02d", a.tm_hour, a.tm_min);
    else if (aos_hal_time_is_valid() && a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1)
        snprintf(out, n, "%s", _("Ayer"));
    else if (a.tm_year == b.tm_year)
        snprintf(out, n, "%d %s", a.tm_mday, aos_month_name(a.tm_mon));
    else
        snprintf(out, n, "%d %s %d", a.tm_mday, aos_month_name(a.tm_mon), a.tm_year + 1900);
}

/* Lower case, without accents: what a search compares. */
static void fold(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 1 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && s[i + 1]) {
            unsigned char d = (unsigned char)s[++i] | 0x20;     /* upper -> lower */
            char r = d >= 0xA0 && d <= 0xA5 ? 'a' : d >= 0xA8 && d <= 0xAB ? 'e' :
                     d >= 0xAC && d <= 0xAF ? 'i' : (d >= 0xB2 && d <= 0xB6) ? 'o' :
                     d >= 0xB9 && d <= 0xBC ? 'u' : d == 0xB1 ? 'n' : d == 0xA7 ? 'c' : '?';
            out[o++] = r;
        } else {
            out[o++] = (char)tolower(c);
        }
    }
    out[o] = 0;
}

static bool matches(const nt_entry_t *e)
{
    if (S.filter == F_NOTES && e->meta.kind != NT_KIND_NOTE) return false;
    if (S.filter == F_TASKS && e->meta.kind != NT_KIND_TASKS) return false;
    if (!S.query[0]) return true;
    char q[64], buf[512];
    fold(S.query, q, sizeof q);
    fold(e->title, buf, sizeof buf);
    if (strstr(buf, q)) return true;
    /* the body, a slice at a time so the buffer stays small */
    const char *t = e->text ? e->text : "";
    size_t len = strlen(t), step = 400;
    for (size_t i = 0; i < len; i += step) {
        char slice[464];
        size_t k = len - i < step + 60 ? len - i : step + 60;
        memcpy(slice, t + i, k);
        slice[k] = 0;
        fold(slice, buf, sizeof buf);
        if (strstr(buf, q)) return true;
    }
    return false;
}

static void save_prefs(void)
{
    aos_hal_pref_set_i32("nt_dark", NT.dark ? 1 : 0);
    aos_hal_pref_set_i32("nt_order", S.order);
    aos_hal_pref_set_i32("nt_rows", S.rows ? 1 : 0);
}

static void statusbar(void)
{
    aos_ui_statusbar_style(NT.dark ? AOS_BAR_LIGHT : AOS_BAR_DARK);
}

static bool doc_empty(void)
{
    if (S.doc.b[0].len) return false;
    for (int i = 1; i < S.doc.n; i++) if (S.doc.b[i].len || S.doc.b[i].type == BT_RULE) return false;
    return true;
}

static void save_now(bool rename_it)
{
    if (!S.open) return;
    if (S.screen == SCR_TASKS) nt_tasks_sync();
    S.dirty = false;
    if (!S.file[0] && doc_empty()) return;     /* nothing yet: no file */
    if (!nt_save(S.file, sizeof S.file, &S.doc, &S.meta, rename_it))
        aos_ui_toast(_("No se pudo guardar la nota"), 2000);
    else
        nt_file_stat(S.file, &S.f_mtime, &S.f_size);
}

static void mark_dirty(void)
{
    S.dirty = true;
    S.dirty_ms = lv_tick_get();
}

/* -------------------------------------------------------------------------- */
/* Typing a note into a computer                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *text;
    char  done[64];
} type_job_t;

/* The US layout the HID types in has no accents: they go as the letter. */
static void to_ascii(const char *s, nt_sb_t *o)
{
    for (size_t i = 0; s[i];) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { nt_sb_putc(o, (char)c); i++; continue; }
        uint32_t cp = 0;
        int n = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        cp = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && s[i + k]; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += n;
        const char *r = "";
        static const char *const lat[] = {
            /* 0xC0 */ "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
            /* 0xD0 */ "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "", "ss",
            /* 0xE0 */ "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
            /* 0xF0 */ "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "", "y" };
        if (cp >= 0xC0 && cp <= 0xFF) r = lat[cp - 0xC0];
        else if (cp == 0xBF) r = "?";
        else if (cp == 0xA1) r = "!";
        else if (cp == 0xB0) r = "o";
        else if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D) r = "\"";
        else if (cp == 0x2018 || cp == 0x2019) r = "'";
        else if (cp == 0x2013 || cp == 0x2014) r = "-";
        else if (cp == 0x2026) r = "...";
        else if (cp == 0x2022 || cp == 0x25E6 || cp == 0x25AA) r = "*";
        else if (cp == 0x20AC) r = "EUR";
        else if (cp == 0x2192) r = "->";
        else if (cp == 0xA0) r = " ";
        nt_sb_put(o, r, -1);
    }
}

static void type_thread(void *arg)
{
    type_job_t *job = arg;
    for (int k = 0; k < 30 && !S.typing_stop; k++) aos_hal_sleep_ms(100);   /* time to put the cursor */
    const char *p = job->text;
    while (*p && !S.typing_stop) {
        char chunk[49];
        size_t n = strlen(p) < 48 ? strlen(p) : 48;
        memcpy(chunk, p, n);
        chunk[n] = 0;
        if (aos_hal_usb_type(chunk) <= 0 && !aos_hal_usb_keys_ready()) break;
        p += n;
        aos_hal_sleep_ms(10);
    }
    if (!S.typing_stop) aos_ui_request_toast(job->done);
    nt_free(job->text);
    nt_free(job);
    S.typing = false;
}

static void type_into_computer(void)
{
    if (S.typing) { S.typing_stop = true; aos_ui_toast(_("Escritura cancelada"), 1500); return; }
    if (!aos_hal_usb_keys_ready()) {
        aos_ui_toast(_("Conectá una computadora en modo Teclado (USB o Bluetooth)"), 2600);
        return;
    }
    nt_sb_t plain = { 0 }, ascii = { 0 };
    nt_doc_plain(&S.doc, &plain, true);
    to_ascii(plain.s ? plain.s : "", &ascii);
    nt_sb_free(&plain);
    type_job_t *job = nt_alloc(sizeof *job);
    if (!job) { nt_sb_free(&ascii); return; }
    job->text = ascii.s ? ascii.s : nt_strdup("");
    snprintf(job->done, sizeof job->done, "%s", _("Nota escrita en la computadora"));
    S.typing = true;
    S.typing_stop = false;
    if (!aos_hal_thread_start("nt_type", type_thread, job, 4096, 3)) {
        S.typing = false;
        nt_free(job->text);
        nt_free(job);
        return;
    }
    aos_ui_toast(_("En 3 segundos: poné el cursor donde va el texto"), 2800);
}

/* -------------------------------------------------------------------------- */
/* Sheets shared by notes and lists                                            */
/* -------------------------------------------------------------------------- */

static void share_qr(void)
{
    nt_sb_t sb = { 0 };
    nt_doc_plain(&S.doc, &sb, true);
    int32_t cw;
    lv_obj_t *card = nt_sheet_custom(_("Escaneá con el teléfono"), &cw);
    bool cut = sb.n > 1100;
    if (cut) {
        int k = 1100;
        while (k > 0 && ((unsigned char)sb.s[k] & 0xC0) == 0x80) k--;
        sb.s[k] = 0;
        sb.n = k;
    }
    int32_t side = LV_MIN(cw - 40, S.land ? S.H - 260 : 560);
    lv_obj_t *wrap = nt_box(card, cw, side + 40);
    lv_obj_t *qr = lv_qrcode_create(wrap);
    lv_qrcode_set_size(qr, side);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 16, 0);
    lv_qrcode_update(qr, sb.s ? sb.s : "", (uint32_t)sb.n);
    lv_obj_center(qr);
    if (cut) {
        lv_obj_t *l = nt_text(card, _("La nota es larga: el código lleva el principio."), F(0, NT_SZ_S), NT.dim);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, cw - 24);
        lv_obj_set_style_pad_all(l, 12, 0);
    }
    nt_sb_free(&sb);
}

static void info_row(lv_obj_t *card, int32_t cw, const char *k, const char *v)
{
    lv_obj_t *r = nt_box(card, cw, 64);
    lv_obj_t *a = nt_text(r, k, F(0, NT_SZ_M), NT.dim);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *b = nt_text(r, v, F(0, NT_SZ_M), NT.text);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(b, cw * 55 / 100, lv_font_get_line_height(F(0, NT_SZ_M)));
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);
}

static void show_info(void)
{
    int32_t cw;
    lv_obj_t *card = nt_sheet_custom(_("Información"), &cw);
    char buf[160];
    int chars;
    int words = nt_doc_words(&S.doc, &chars);
    snprintf(buf, sizeof buf, "%d", words);
    info_row(card, cw, _("Palabras"), buf);
    snprintf(buf, sizeof buf, "%d", chars);
    info_row(card, cw, _("Caracteres"), buf);
    int paras = 0, tasks = 0, done = 0;
    for (int i = 1; i < S.doc.n; i++) {
        if (S.doc.b[i].len) paras++;
        if (S.doc.b[i].type == BT_CHECK) { tasks++; done += S.doc.b[i].checked; }
    }
    snprintf(buf, sizeof buf, "%d", paras);
    info_row(card, cw, _("Párrafos"), buf);
    if (tasks) {
        snprintf(buf, sizeof buf, _("%d de %d"), done, tasks);
        info_row(card, cw, _("Tareas hechas"), buf);
    }
    char d[48];
    fmt_date(S.meta.created, d, sizeof d);
    info_row(card, cw, _("Creada"), d[0] ? d : "-");
    fmt_date(S.meta.modified, d, sizeof d);
    info_row(card, cw, _("Modificada"), d[0] ? d : "-");
    snprintf(buf, sizeof buf, "notas/%s", S.file[0] ? S.file : _("(sin guardar)"));
    info_row(card, cw, _("Archivo"), buf);
}

static void tag_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    nt_sheet_close();
    if (S.open) {
        S.meta.tag = (uint8_t)i;
        mark_dirty();
    } else if (S.menu_entry >= 0 && S.menu_entry < S.ix.n) {
        nt_doc_t d;
        nt_meta_t m;
        nt_doc_init(&d);
        char file[96];
        snprintf(file, sizeof file, "%s", S.ix.e[S.menu_entry].file);
        nt_load(nt_dir(), file, &d, &m);
        m.tag = (uint8_t)i;
        nt_save(file, sizeof file, &d, &m, false);
        nt_doc_clear(&d);
        build();
    }
}

static void pick_tag(int current)
{
    int32_t cw;
    lv_obj_t *card = nt_sheet_custom(_("Color"), &cw);
    lv_obj_t *row = nt_swatches(card, cw - 16, NT_TAG_COUNT, nt_tag_color, current, true, tag_cb, NULL);
    lv_obj_set_style_pad_hor(row, 8, 0);
    nt_box(card, cw, 16);
}

static void trash_open_note(void *ud)
{
    (void)ud;
    if (S.file[0]) {
        save_now(false);
        nt_trash(S.file);
    }
    S.file[0] = 0;
    nt_doc_clear(&S.doc);
    nt_doc_init(&S.doc);
    nt_doc_insert(&S.doc, 1, BT_PARA);
    close_note();
    aos_ui_toast(_("Movida a la papelera"), 1500);
}

static void convert_kind(void)
{
    if (S.meta.kind == NT_KIND_NOTE) {
        for (int i = S.doc.n - 1; i >= 1; i--) {
            nt_blk_t *b = &S.doc.b[i];
            if (b->type == BT_RULE || b->len == 0) { nt_doc_remove(&S.doc, i); continue; }
            if (b->type != BT_CHECK) { b->type = BT_CHECK; b->style = 0; b->checked = 0; }
        }
        S.meta.kind = NT_KIND_TASKS;
        nt_ed_forget();
    } else {
        nt_tasks_commit();
        S.meta.kind = NT_KIND_NOTE;
        if (S.doc.n == 1) nt_doc_insert(&S.doc, 1, BT_PARA);
        nt_tasks_forget();
    }
    mark_dirty();
    S.screen = S.meta.kind == NT_KIND_TASKS ? SCR_TASKS : SCR_NOTE;
    S.panel = P_NONE;
    build();
}

static void dup_open(void)
{
    save_now(false);
    char out[96];
    if (S.file[0] && nt_duplicate(S.file, out, sizeof out)) aos_ui_toast(_("Duplicada"), 1200);
}

enum { NM_PIN, NM_TAG, NM_CONVERT, NM_QR, NM_TYPE, NM_INFO, NM_DUP, NM_TRASH,
       NM_FOLD, NM_UNCHECK, NM_CLEAR_DONE, NM_SORT };

static int s_menu_ids[16];

static void note_menu_pick(int i, void *ud)
{
    (void)ud;
    switch (s_menu_ids[i]) {
    case NM_PIN:     S.meta.pinned = !S.meta.pinned; mark_dirty();
                     aos_ui_toast(S.meta.pinned ? _("Fijada arriba") : _("Ya no está fijada"), 1200); break;
    case NM_TAG:     pick_tag(S.meta.tag); break;
    case NM_CONVERT: convert_kind(); break;
    case NM_QR:      share_qr(); break;
    case NM_TYPE:    type_into_computer(); break;
    case NM_INFO:    show_info(); break;
    case NM_DUP:     dup_open(); break;
    case NM_TRASH:   nt_confirm(_("¿Mover a la papelera?"), _("Se puede recuperar desde la papelera."),
                                _("Mover"), true, trash_open_note, NULL); break;
    case NM_FOLD:    S.meta.hide_done = !S.meta.hide_done; mark_dirty(); nt_tasks_refresh(); break;
    case NM_UNCHECK: nt_tasks_uncheck_all(); break;
    case NM_CLEAR_DONE: nt_tasks_delete_done(); break;
    case NM_SORT:    nt_tasks_sort_prio(); break;
    default: break;
    }
}

static void note_menu_cb(lv_event_t *e)
{
    (void)e;
    if (S.screen == SCR_NOTE) { show_panel(P_NONE); }
    nt_item_t it[16];
    int n = 0;
    bool tasks = S.screen == SCR_TASKS;
#define ADD(id, lab, ic, danger) do { s_menu_ids[n] = id; it[n++] = (nt_item_t){ lab, ic, danger, false }; } while (0)
    if (tasks) {
        ADD(NM_FOLD, S.meta.hide_done ? _("Mostrar completadas") : _("Ocultar completadas"), NT_IC_CHEVRON, false);
        ADD(NM_UNCHECK, _("Desmarcar todas"), NT_IC_RESTORE, false);
        ADD(NM_CLEAR_DONE, _("Borrar completadas"), NT_IC_CLOSE, false);
        ADD(NM_SORT, _("Ordenar por prioridad"), NT_IC_ROWS, false);
    }
    ADD(NM_PIN, S.meta.pinned ? _("Desfijar") : _("Fijar arriba"), NT_IC_PIN, false);
    ADD(NM_TAG, _("Color"), NT_IC_PALETTE, false);
    ADD(NM_CONVERT, tasks ? _("Convertir en nota") : _("Convertir en lista de tareas"),
        tasks ? NT_IC_NOTE : NT_IC_TASKS, false);
    ADD(NM_QR, _("Compartir con un QR"), NT_IC_QR, false);
    ADD(NM_TYPE, S.typing ? _("Parar de escribir") : _("Escribir en la computadora"), NT_IC_TYPE, false);
    if (!tasks) ADD(NM_INFO, _("Información"), NT_IC_INFO, false);
    ADD(NM_DUP, _("Duplicar"), NT_IC_COPY, false);
    ADD(NM_TRASH, _("Mover a la papelera"), NT_IC_TRASH, true);
#undef ADD
    nt_sheet(S.doc.b[0].len ? S.doc.b[0].txt : _("Sin título"), it, n, note_menu_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* The note screen: top bar, editor, formatting bar, panels                    */
/* -------------------------------------------------------------------------- */

static void refresh_toolbar(void)
{
    if (S.screen != SCR_NOTE || !S.toolbar) return;
    uint16_t a = nt_ed_attr();
    int style;
    int bt = nt_ed_block_type(&style);
    lv_obj_set_state(S.tb_b, LV_STATE_CHECKED, (a & A_B) || bt == BT_H1 || bt == BT_H2 || bt == BT_H3 || bt == BT_TITLE);
    lv_obj_set_state(S.tb_i, LV_STATE_CHECKED, (a & A_I) != 0);
    lv_obj_set_state(S.tb_u, LV_STATE_CHECKED, (a & A_U) != 0);
    lv_obj_set_state(S.tb_list, LV_STATE_CHECKED, bt == BT_BULLET || bt == BT_NUM || S.panel == P_LIST);
    lv_obj_set_state(S.tb_check, LV_STATE_CHECKED, bt == BT_CHECK);
    lv_obj_set_state(S.tb_aa, LV_STATE_CHECKED, S.panel == P_FORMAT);
    lv_obj_set_state(S.tb_fg, LV_STATE_CHECKED, S.panel == P_COLOR);
    lv_obj_invalidate(S.tb_fg);
    lv_obj_invalidate(S.tb_hl);
    nt_icon_set(S.tb_kb, S.panel == P_KB ? NT_IC_KB_HIDE : NT_IC_TYPE);
    if (S.btn_undo) lv_obj_set_state(S.btn_undo, LV_STATE_DISABLED, !nt_ed_can_undo());
    if (S.btn_redo) lv_obj_set_state(S.btn_redo, LV_STATE_DISABLED, !nt_ed_can_redo());
}

static void ed_changed(void)
{
    mark_dirty();
    refresh_toolbar();
}

static void ed_caret(void)
{
    refresh_toolbar();
}

static void ed_focus(void)
{
    if (S.panel == P_NONE) show_panel(P_KB);
    else if (S.panel != P_KB && S.panel != P_FORMAT && S.panel != P_COLOR && S.panel != P_LIST) show_panel(P_KB);
}

static const nt_ed_cbs_t ED_CBS = { ed_changed, ed_caret, ed_focus };

/* The formatting panels */

static void fmt_block_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v == BT_RULE) {
        /* a rule after the caret's block */
        int b, p;
        nt_ed_caret_pos(&b, &p);
        const nt_kb_target_t *t = nt_ed_target();
        if (S.doc.b[b].len) t->enter();
        nt_ed_caret_pos(&b, &p);
        nt_blk_set(&S.doc.b[b], "---", 0);
        nt_ed_place(b, 3);
        t->enter();
    } else {
        nt_ed_set_block(v, 0);
    }
    show_panel(S.panel);
}

static void fmt_toggle_cb(lv_event_t *e)
{
    nt_ed_toggle((uint16_t)(intptr_t)lv_event_get_user_data(e));
    show_panel(S.panel);
}

static void fmt_size_cb(lv_event_t *e)
{
    nt_ed_set_size((int)(intptr_t)lv_event_get_user_data(e));
    show_panel(S.panel);
}

static void fmt_fg_cb(lv_event_t *e)
{
    nt_ed_set_fg((int)(intptr_t)lv_event_get_user_data(e));
    show_panel(S.panel);
}

static void fmt_hl_cb(lv_event_t *e)
{
    nt_ed_set_hl((int)(intptr_t)lv_event_get_user_data(e));
    show_panel(S.panel);
}

static void fmt_clear_cb(lv_event_t *e)
{
    (void)e;
    nt_ed_clear_format();
    show_panel(S.panel);
}

static lv_obj_t *panel_label(lv_obj_t *p, const char *s)
{
    lv_obj_t *l = nt_text(p, s, F(NT_ST_BOLD, NT_SZ_S), NT.dim);
    lv_obj_set_style_pad_top(l, 6, 0);
    return l;
}

static lv_obj_t *flow_row(lv_obj_t *p, int32_t w)
{
    lv_obj_t *r = nt_box(p, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_style_pad_row(r, 10, 0);
    return r;
}

static lv_obj_t *letter_btn(lv_obj_t *p, const char *s, int style, lv_text_decor_t decor,
                            bool on, lv_event_cb_t cb, void *ud, int32_t w)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 76);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, on ? NT.accent : NT.surface2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = nt_text(b, s, F(style, NT_SZ_M), on ? lv_color_black() : NT.text);
    lv_obj_set_style_text_decor(l, decor, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *chip(lv_obj_t *p, const char *s, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = nt_pill(p, s, cb, ud);
    lv_obj_set_style_bg_color(c, NT.surface2, 0);
    lv_obj_set_height(c, 68);
    lv_obj_set_style_radius(c, 34, 0);
    if (on) lv_obj_add_state(c, LV_STATE_CHECKED);
    return c;
}

static void build_format(lv_obj_t *p, int32_t w, bool colours_only)
{
    uint16_t a = nt_ed_attr();
    int style;
    int bt = nt_ed_block_type(&style);
    if (!colours_only) {
        lv_obj_t *r = flow_row(p, w);
        static const int types[] = { BT_H1, BT_H2, BT_H3, BT_PARA, BT_QUOTE, BT_RULE };
        static const char *const names[] = { N_("Título"), N_("Encabezado"), N_("Subtítulo"),
                                             N_("Cuerpo"), N_("Cita"), N_("Línea") };
        for (int k = 0; k < 6; k++)
            chip(r, _(names[k]), bt == types[k] && types[k] != BT_RULE, fmt_block_cb, (void *)(intptr_t)types[k]);

        lv_obj_t *r2 = flow_row(p, w);
        int32_t bw = (w - 4 * 10) / 5;
        letter_btn(r2, "B", NT_ST_BOLD, 0, a & A_B, fmt_toggle_cb, (void *)(intptr_t)A_B, bw);
        letter_btn(r2, "I", NT_ST_ITALIC, 0, a & A_I, fmt_toggle_cb, (void *)(intptr_t)A_I, bw);
        letter_btn(r2, "U", 0, LV_TEXT_DECOR_UNDERLINE, a & A_U, fmt_toggle_cb, (void *)(intptr_t)A_U, bw);
        letter_btn(r2, "S", 0, LV_TEXT_DECOR_STRIKETHROUGH, a & A_S, fmt_toggle_cb, (void *)(intptr_t)A_S, bw);
        letter_btn(r2, "Tx", 0, 0, false, fmt_clear_cb, NULL, bw);

        panel_label(p, _("Tamaño"));
        lv_obj_t *r3 = flow_row(p, w);
        static const int sizes[] = { NT_SZ_S, NT_SZ_M, NT_SZ_L, NT_SZ_XL };
        static const char *const snames[] = { N_("Chica"), N_("Normal"), N_("Grande"), N_("Enorme") };
        for (int k = 0; k < 4; k++)
            chip(r3, _(snames[k]), A_SZ(a) == sizes[k], fmt_size_cb, (void *)(intptr_t)sizes[k]);
    }
    panel_label(p, _("Color del texto"));
    nt_swatches(p, w, NT_FG_COUNT, nt_fg_color, A_FG(a), false, fmt_fg_cb, NULL);
    panel_label(p, _("Resaltado"));
    nt_swatches(p, w, NT_HL_COUNT, nt_hl_color, A_HL(a), true, fmt_hl_cb, NULL);
}

static void list_style_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v >= 100) nt_ed_set_block(BT_NUM, v - 100);
    else nt_ed_set_block(BT_BULLET, v);
    show_panel(S.panel);
}

static void list_misc_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    switch (v) {
    case 0: nt_ed_toggle_check_block(); break;
    case 1: nt_ed_set_block(BT_PARA, 0); break;
    case 2: nt_ed_indent(-1); break;
    case 3: nt_ed_indent(1); break;
    }
    show_panel(S.panel);
}

static void bullet_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    int v = (int)(intptr_t)lv_obj_get_user_data(o);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    bool on = lv_obj_has_state(o, LV_STATE_CHECKED);
    lv_color_t c = on ? lv_color_black() : NT.text;
    int32_t cy = (a.y1 + a.y2) / 2;
    nt_draw_bullet(lv_event_get_layer(e), v, a.x1 + 30, cy, 5, c);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_50;
    d.radius = 2;
    lv_area_t l1 = { a.x1 + 50, cy - 2, a.x2 - 14, cy + 2 };
    lv_draw_rect(lv_event_get_layer(e), &d, &l1);
}

static void build_lists(lv_obj_t *p, int32_t w)
{
    int style;
    int bt = nt_ed_block_type(&style);
    panel_label(p, _("Viñetas"));
    lv_obj_t *r = flow_row(p, w);
    int32_t bw = (w - 3 * 10) / 4;
    for (int k = 0; k < NT_BS_COUNT; k++) {
        lv_obj_t *b = lv_obj_create(r);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, bw, 68);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_bg_color(b, NT.surface2, 0);
        lv_obj_set_style_bg_color(b, NT.accent, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        if (bt == BT_BULLET && style == k) lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_obj_set_user_data(b, (void *)(intptr_t)k);
        lv_obj_add_event_cb(b, bullet_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(b, list_style_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    }
    panel_label(p, _("Numeradas"));
    lv_obj_t *r2 = flow_row(p, w);
    static const char *const nums[NT_NS_COUNT] = { "1. 2. 3.", "a) b) c)", "A) B) C)", "i. ii. iii.", "I. II. III." };
    for (int k = 0; k < NT_NS_COUNT; k++)
        chip(r2, nums[k], bt == BT_NUM && style == k, list_style_cb, (void *)(intptr_t)(100 + k));
    lv_obj_t *r3 = flow_row(p, w);
    chip(r3, _("Casillas"), bt == BT_CHECK, list_misc_cb, (void *)0);
    chip(r3, _("Sin lista"), false, list_misc_cb, (void *)1);
    lv_obj_t *o = nt_icon_btn(r3, NT_IC_OUTDENT, 68, list_misc_cb, (void *)2);
    lv_obj_set_style_bg_color(o, NT.surface2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_t *i = nt_icon_btn(r3, NT_IC_INDENT, 68, list_misc_cb, (void *)3);
    lv_obj_set_style_bg_color(i, NT.surface2, 0);
    lv_obj_set_style_bg_opa(i, LV_OPA_COVER, 0);
}

static void show_panel(int p)
{
    if (!S.kb_area) return;
    int32_t scroll = -1;
    lv_obj_t *old = lv_obj_get_child(S.kb_area, 0);
    if (old && S.panel == p && p != P_KB) scroll = lv_obj_get_scroll_y(old);
    lv_obj_clean(S.kb_area);
    S.kb = NULL;
    nt_kb_destroyed();
    S.panel = p;
    bool landscape_kb = S.land && p != P_NONE;
    if (S.topbar) {
        if (landscape_kb) lv_obj_add_flag(S.topbar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(S.topbar, LV_OBJ_FLAG_HIDDEN);
    }
    if (S.fab) {
        if (p == P_NONE) lv_obj_remove_flag(S.fab, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(S.fab, LV_OBJ_FLAG_HIDDEN);
    }
    if (p == P_NONE) {
        lv_obj_add_flag(S.kb_area, LV_OBJ_FLAG_HIDDEN);
        if (S.screen == SCR_NOTE) nt_ed_set_focus(false);
        nt_kb_attach(NULL);
    } else {
        lv_obj_remove_flag(S.kb_area, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(S.kb_area, S.kb_h);
        if (p == P_KB) {
            S.kb = nt_kb_create(S.kb_area, S.W, S.kb_h);
            if (S.screen == SCR_NOTE) {
                nt_kb_attach(nt_ed_target());
                nt_ed_set_focus(true);
            } else if (S.screen == SCR_LIST && S.search_ta) {
                list_search_attach();
            }
        } else {
            nt_kb_attach(NULL);
            nt_ed_set_focus(true);
            lv_obj_t *pan = lv_obj_create(S.kb_area);
            lv_obj_remove_style_all(pan);
            lv_obj_set_size(pan, S.W, S.kb_h);
            lv_obj_set_style_bg_color(pan, NT.surface, 0);
            lv_obj_set_style_bg_opa(pan, LV_OPA_COVER, 0);
            int32_t pw = S.land ? 860 : S.W - 48;
            lv_obj_set_style_pad_hor(pan, (S.W - pw) / 2, 0);
            lv_obj_set_style_pad_ver(pan, 16, 0);
            lv_obj_set_style_pad_row(pan, 12, 0);
            lv_obj_set_flex_flow(pan, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_scroll_dir(pan, LV_DIR_VER);
            if (p == P_FORMAT) build_format(pan, pw, false);
            else if (p == P_COLOR) build_format(pan, pw, true);
            else build_lists(pan, pw);
            nt_box(pan, pw, 8);
            if (scroll > 0) {
                lv_obj_update_layout(pan);
                lv_obj_scroll_to_y(pan, scroll, LV_ANIM_OFF);
            }
        }
    }
    if (S.screen == SCR_NOTE) {
        lv_obj_update_layout(S.page);
        nt_ed_scroll_to_caret();
        refresh_toolbar();
    }
}

static void toolbar_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    switch (v) {
    case 0: show_panel(S.panel == P_FORMAT ? P_KB : P_FORMAT); break;
    case 1: nt_ed_toggle(A_B); break;
    case 2: nt_ed_toggle(A_I); break;
    case 3: nt_ed_toggle(A_U); break;
    case 4: show_panel(S.panel == P_COLOR ? P_KB : P_COLOR); break;
    case 5: {
        /* the highlighter: on with yellow, off if already on */
        uint16_t a = nt_ed_attr();
        nt_ed_set_hl(A_HL(a) ? 0 : 1);
        break;
    }
    case 6: show_panel(S.panel == P_LIST ? P_KB : P_LIST); break;
    case 7: nt_ed_toggle_check_block(); break;
    case 8: nt_ed_indent(-1); break;
    case 9: nt_ed_indent(1); break;
    case 10: show_panel(S.panel == P_KB ? P_NONE : P_KB); break;
    }
    refresh_toolbar();
}

static void bar_under_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    bool hl = o == S.tb_hl;
    uint16_t a = nt_ed_attr();
    lv_color_t c = hl ? (A_HL(a) ? nt_hl_color(A_HL(a)) : NT.hair) : nt_fg_color(A_FG(a));
    if (hl && !A_HL(a)) c = nt_hl_color(1);
    lv_area_t co;
    lv_obj_get_coords(o, &co);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.radius = 3;
    d.border_color = NT.hair;
    d.border_width = 1;
    int32_t cx = (co.x1 + co.x2) / 2;
    lv_area_t r = { cx - 16, co.y2 - 16, cx + 16, co.y2 - 9 };
    lv_draw_rect(lv_event_get_layer(e), &d, &r);
}

static lv_obj_t *tb_letter(lv_obj_t *bar, const char *s, int style, lv_text_decor_t decor, int v, int32_t w)
{
    lv_obj_t *b = nt_icon_btn(bar, -1, w, toolbar_cb, (void *)(intptr_t)v);
    lv_obj_set_height(b, 64);
    lv_obj_t *l = nt_text(b, s, F(style, NT_SZ_M), NT.text);
    lv_obj_set_style_text_decor(l, decor, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *tb_icon(lv_obj_t *bar, int icon, int v, int32_t w)
{
    lv_obj_t *b = nt_icon_btn(bar, icon, w, toolbar_cb, (void *)(intptr_t)v);
    lv_obj_set_height(b, 64);
    return b;
}

static void build_toolbar(lv_obj_t *page)
{
    S.toolbar = nt_box(page, S.W, 76);
    lv_obj_set_style_bg_color(S.toolbar, NT.surface, 0);
    lv_obj_set_style_bg_opa(S.toolbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(S.toolbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(S.toolbar, NT.hair, 0);
    lv_obj_set_style_border_width(S.toolbar, 1, 0);
    lv_obj_set_flex_flow(S.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(S.toolbar, S.land ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(S.toolbar, S.land ? 18 : 0, 0);
    int32_t w = S.land ? 76 : (S.W - 8) / 11;
    S.tb_aa = tb_icon(S.toolbar, NT_IC_AA, 0, w);
    S.tb_b = tb_letter(S.toolbar, "B", NT_ST_BOLD, 0, 1, w);
    S.tb_i = tb_letter(S.toolbar, "I", NT_ST_ITALIC, 0, 2, w);
    S.tb_u = tb_letter(S.toolbar, "U", 0, LV_TEXT_DECOR_UNDERLINE, 3, w);
    S.tb_fg = tb_letter(S.toolbar, "A", NT_ST_BOLD, 0, 4, w);
    lv_obj_set_style_pad_bottom(lv_obj_get_child(S.tb_fg, 0), 10, 0);
    lv_obj_add_event_cb(S.tb_fg, bar_under_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    S.tb_hl = tb_icon(S.toolbar, NT_IC_HIGHLIGHT, 5, w);
    lv_obj_add_event_cb(S.tb_hl, bar_under_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    S.tb_list = tb_icon(S.toolbar, NT_IC_LIST, 6, w);
    S.tb_check = tb_icon(S.toolbar, NT_IC_CHECKBOX, 7, w);
    tb_icon(S.toolbar, NT_IC_OUTDENT, 8, w);
    tb_icon(S.toolbar, NT_IC_INDENT, 9, w);
    S.tb_kb = tb_icon(S.toolbar, NT_IC_KB_HIDE, 10, w);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    close_note();
}

static void undo_cb(lv_event_t *e)
{
    (void)e;
    nt_ed_undo();
    refresh_toolbar();
}

static void redo_cb(lv_event_t *e)
{
    (void)e;
    nt_ed_redo();
    refresh_toolbar();
}

static lv_obj_t *topbar(lv_obj_t *page, const char *back_label, lv_event_cb_t back)
{
    lv_obj_t *bar = nt_box(page, S.W, 84);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_t *b = lv_obj_create(bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_pad_right(b, 16, 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, NT.surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ic = nt_icon_btn(b, NT_IC_BACK, 64, NULL, NULL);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    nt_icon_color(ic, NT.accent);
    nt_text(b, back_label, F(0, NT_SZ_M), NT.accent);
    lv_obj_add_event_cb(b, back, LV_EVENT_CLICKED, NULL);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
    return bar;
}

static void build_note(void)
{
    lv_obj_t *page = S.page;
    S.topbar = topbar(page, _("Notas"), back_cb);
    lv_obj_t *more = nt_icon_btn(S.topbar, NT_IC_MORE, 72, note_menu_cb, NULL);
    nt_icon_color(more, NT.accent);
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, 0, 0);
    if (S.screen == SCR_NOTE) {
        S.btn_redo = nt_icon_btn(S.topbar, NT_IC_REDO, 72, redo_cb, NULL);
        nt_icon_color(S.btn_redo, NT.accent);
        lv_obj_align(S.btn_redo, LV_ALIGN_RIGHT_MID, -80, 0);
        S.btn_undo = nt_icon_btn(S.topbar, NT_IC_UNDO, 72, undo_cb, NULL);
        nt_icon_color(S.btn_undo, NT.accent);
        lv_obj_align(S.btn_undo, LV_ALIGN_RIGHT_MID, -160, 0);
        nt_ed_create(page, &S.doc, &ED_CBS);
        build_toolbar(page);
    } else {
        static const nt_tasks_cbs_t TCBS = { mark_dirty, tasks_keyboard };
        nt_tasks_cbs_t c = TCBS;
        S.kb_area = nt_box(page, S.W, S.kb_h);      /* before the list builds: it may ask for it */
        lv_obj_add_flag(S.kb_area, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *list = nt_tasks_create(page, &S.doc, &S.meta, &c);
        lv_obj_move_to_index(list, 1);
        return;
    }
    S.kb_area = nt_box(page, S.W, S.kb_h);
    lv_obj_add_flag(S.kb_area, LV_OBJ_FLAG_HIDDEN);
    show_panel(S.panel);
}

static void tasks_keyboard(bool show)
{
    if (!S.kb_area || S.screen != SCR_TASKS) return;
    if (show) {
        if (!S.kb) {
            lv_obj_remove_flag(S.kb_area, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clean(S.kb_area);
            S.kb = nt_kb_create(S.kb_area, S.W, S.kb_h);
            S.panel = P_KB;
            if (S.land && S.topbar) lv_obj_add_flag(S.topbar, LV_OBJ_FLAG_HIDDEN);
        }
        nt_kb_refresh_caps();
    } else {
        lv_obj_clean(S.kb_area);
        S.kb = NULL;
        nt_kb_destroyed();
        lv_obj_add_flag(S.kb_area, LV_OBJ_FLAG_HIDDEN);
        S.panel = P_NONE;
        if (S.topbar) lv_obj_remove_flag(S.topbar, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------------------------------------------------------------------------- */
/* Opening and closing                                                         */
/* -------------------------------------------------------------------------- */

static void open_entry(int i)
{
    if (i < 0 || i >= S.ix.n) return;
    S.list_scroll = S.cards ? lv_obj_get_scroll_y(S.cards) : 0;
    nt_doc_clear(&S.doc);
    nt_doc_init(&S.doc);
    snprintf(S.file, sizeof S.file, "%s", S.ix.e[i].file);
    nt_load(nt_dir(), S.file, &S.doc, &S.meta);
    nt_file_stat(S.file, &S.f_mtime, &S.f_size);
    S.open = true;
    S.dirty = false;
    S.screen = S.meta.kind == NT_KIND_TASKS ? SCR_TASKS : SCR_NOTE;
    S.panel = P_NONE;
    nt_ed_forget();
    nt_tasks_forget();
    build();
}

static void new_note(int kind)
{
    S.list_scroll = S.cards ? lv_obj_get_scroll_y(S.cards) : 0;
    nt_doc_clear(&S.doc);
    nt_doc_init(&S.doc);
    memset(&S.meta, 0, sizeof S.meta);
    S.meta.kind = (uint8_t)kind;
    S.meta.created = nt_now();
    if (kind == NT_KIND_NOTE) nt_doc_insert(&S.doc, 1, BT_PARA);
    S.file[0] = 0;
    S.open = true;
    S.dirty = false;
    S.screen = kind == NT_KIND_TASKS ? SCR_TASKS : SCR_NOTE;
    nt_ed_forget();
    nt_tasks_forget();
    S.panel = kind == NT_KIND_NOTE ? P_KB : P_NONE;
    build();
    if (kind == NT_KIND_NOTE) nt_ed_place(0, 0);
}

static void close_note(void)
{
    if (!S.open) return;
    if (S.screen == SCR_TASKS) nt_tasks_commit();
    if (doc_empty()) {
        if (S.file[0]) nt_trash(S.file);
    } else {
        save_now(true);
    }
    S.open = false;
    S.dirty = false;
    nt_ed_forget();
    nt_tasks_forget();
    nt_doc_clear(&S.doc);
    S.screen = SCR_LIST;
    S.panel = P_NONE;
    S.searching = false;
    build();
}

/* -------------------------------------------------------------------------- */
/* The notes                                                                   */
/* -------------------------------------------------------------------------- */

static void card_cb(lv_event_t *e)
{
    open_entry((int)(intptr_t)lv_event_get_user_data(e));
}

static void entry_trash(void *ud)
{
    (void)ud;
    if (S.menu_entry < 0 || S.menu_entry >= S.ix.n) return;
    nt_trash(S.ix.e[S.menu_entry].file);
    aos_ui_toast(_("Movida a la papelera"), 1500);
    build();
}

static void entry_pick(int i, void *ud)
{
    (void)ud;
    int k = S.menu_entry;
    if (k < 0 || k >= S.ix.n) return;
    nt_entry_t *en = &S.ix.e[k];
    switch (i) {
    case 0: open_entry(k); break;
    case 1: {
        nt_doc_t d;
        nt_meta_t m;
        nt_doc_init(&d);
        char file[96];
        snprintf(file, sizeof file, "%s", en->file);
        nt_load(nt_dir(), file, &d, &m);
        m.pinned = !m.pinned;
        uint32_t keep = m.modified;
        nt_save(file, sizeof file, &d, &m, false);
        (void)keep;
        nt_doc_clear(&d);
        build();
        break;
    }
    case 2: pick_tag(en->meta.tag); break;
    case 3: {
        char out[96];
        if (nt_duplicate(en->file, out, sizeof out)) aos_ui_toast(_("Duplicada"), 1200);
        build();
        break;
    }
    case 4:
        nt_confirm(_("¿Mover a la papelera?"), en->title, _("Mover"), true, entry_trash, NULL);
        break;
    }
}

static void card_long_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= S.ix.n) return;
    S.menu_entry = k;
    lv_indev_wait_release(lv_indev_active());
    nt_item_t it[5] = {
        { _("Abrir"), S.ix.e[k].meta.kind == NT_KIND_TASKS ? NT_IC_TASKS : NT_IC_NOTE, false, false },
        { S.ix.e[k].meta.pinned ? _("Desfijar") : _("Fijar arriba"), NT_IC_PIN, false, false },
        { _("Color"), NT_IC_PALETTE, false, false },
        { _("Duplicar"), NT_IC_COPY, false, false },
        { _("Mover a la papelera"), NT_IC_TRASH, true, false },
    };
    nt_sheet(S.ix.e[k].title[0] ? S.ix.e[k].title : _("Sin título"), it, 5, entry_pick, NULL);
}

static void mini_box_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    bool on = lv_obj_get_user_data(o) != NULL;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    nt_draw_checkbox(lv_event_get_layer(e), &a, on, true, NT.dim, NT.accent);
}

/* The first lines of a task list: box and text. */
static void card_tasks(lv_obj_t *c, const nt_entry_t *en, int32_t w, int max)
{
    const char *p = en->text ? en->text : "";
    int shown = 0;
    while (*p && shown < max) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        const char *s = p;
        while (len > 0 && *s == ' ') { s++; len--; }
        bool box = len >= 4 && (strncmp(s, "[ ] ", 4) == 0 || strncmp(s, "[x] ", 4) == 0);
        if (box) {
            bool on = s[1] == 'x';
            lv_obj_t *r = nt_box(c, w, 40);
            lv_obj_t *b = nt_box(r, 28, 28);
            lv_obj_set_user_data(b, on ? (void *)1 : NULL);
            lv_obj_add_event_cb(b, mini_box_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
            lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
            char line[128];
            int n = len - 4 < (int)sizeof line - 1 ? len - 4 : (int)sizeof line - 1;
            memcpy(line, s + 4, n);
            line[n] = 0;
            lv_obj_t *l = nt_text(r, line, F(0, NT_SZ_S), on ? NT.dim : NT.text);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(l, w - 44);
            if (on) lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_STRIKETHROUGH, 0);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 40, 0);
            shown++;
        }
        if (!e) break;
        p = e + 1;
    }
}

static lv_obj_t *add_card(lv_obj_t *parent, int k, int32_t w, int32_t h)
{
    nt_entry_t *en = &S.ix.e[k];
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, 24, 0);
    lv_obj_set_style_bg_color(c, nt_tag_card(en->meta.tag), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_set_style_transform_scale(c, 245, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(c, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(c, h / 2, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c, card_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)k);
    lv_obj_add_event_cb(c, card_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
    if (!NT.dark && en->meta.tag == 0) {
        lv_obj_set_style_border_color(c, NT.hair, 0);
        lv_obj_set_style_border_width(c, 1, 0);
    }
    int32_t iw = w - 40;
    lv_obj_t *t = nt_text(c, en->title[0] ? en->title : _("Sin título"), F(NT_ST_BOLD, NT_SZ_M),
                          en->title[0] ? NT.text : NT.dim);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, iw - (en->meta.pinned ? 40 : 0));
    lv_obj_set_style_max_height(t, F(NT_ST_BOLD, NT_SZ_M)->line_height * (S.rows ? 1 : 2), 0);

    int32_t foot = 36;
    int32_t body_h = h - 40 - lv_font_get_line_height(F(NT_ST_BOLD, NT_SZ_M)) * (S.rows ? 1 : 2) - foot - 12;
    if (en->meta.kind == NT_KIND_TASKS) {
        int lines = body_h / 40;
        card_tasks(c, en, iw, lines < 1 ? 1 : lines);
    } else {
        lv_obj_t *p = nt_text(c, en->text ? en->text : "", F(0, NT_SZ_S), NT.dark ? lv_color_hex(0xC7C7CC) : lv_color_hex(0x3A3A3C));
        lv_label_set_long_mode(p, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(p, iw);
        lv_obj_set_height(p, body_h > 30 ? body_h : 30);
    }
    lv_obj_t *sp = nt_box(c, 1, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_t *f = nt_box(c, iw, foot);
    char d[48], line[96];
    fmt_date(en->meta.modified ? en->meta.modified : en->mtime, d, sizeof d);
    if (en->meta.kind == NT_KIND_TASKS && en->total)
        snprintf(line, sizeof line, "%s%s%d/%d", d, d[0] ? "  \xE2\x80\xA2  " : "", en->done, en->total);
    else
        snprintf(line, sizeof line, "%s", d);
    lv_obj_t *dl = nt_text(f, line, F(0, NT_SZ_S), NT.dim);
    lv_obj_align(dl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *kind = nt_icon_btn(f, en->meta.kind == NT_KIND_TASKS ? NT_IC_TASKS : NT_IC_NOTE, 36, NULL, NULL);
    lv_obj_remove_flag(kind, LV_OBJ_FLAG_CLICKABLE);
    nt_icon_color(kind, NT.dim);
    lv_obj_align(kind, LV_ALIGN_RIGHT_MID, 0, 0);
    if (en->meta.pinned) {
        lv_obj_t *pin = nt_icon_btn(c, NT_IC_PIN, 40, NULL, NULL);
        lv_obj_remove_flag(pin, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(pin, LV_OBJ_FLAG_FLOATING);
        nt_icon_color(pin, NT.accent);
        lv_obj_align(pin, LV_ALIGN_TOP_RIGHT, 6, -6);
    }
    return c;
}

static void section_title(lv_obj_t *parent, int32_t w, const char *s)
{
    lv_obj_t *l = nt_text(parent, s, F(NT_ST_BOLD, NT_SZ_S), NT.dim);
    lv_obj_set_width(l, w);
    lv_obj_set_style_pad_top(l, 12, 0);
}

static void fill_cards(void)
{
    if (!S.cards) return;
    lv_obj_clean(S.cards);
    int32_t pad = 24, gap = 16;
    int32_t inner = S.W - 2 * pad;
    int cols = S.rows ? (S.land ? 2 : 1) : (S.land ? 4 : 2);
    int32_t cw = (inner - gap * (cols - 1)) / cols;
    int32_t ch = S.rows ? 190 : (S.land ? 300 : 330);
    int shown = 0, pinned = 0, others = 0;
    for (int i = 0; i < S.ix.n; i++) {
        if (!matches(&S.ix.e[i])) continue;
        if (S.ix.e[i].meta.pinned) pinned++;
        else others++;
    }
    bool sections = pinned && others;
    for (int pass = 0; pass < 2; pass++) {
        if (sections) section_title(S.cards, inner, pass ? _("Otras") : _("Fijadas"));
        for (int i = 0; i < S.ix.n; i++) {
            nt_entry_t *en = &S.ix.e[i];
            if ((pass == 0) != en->meta.pinned || !matches(en)) continue;
            add_card(S.cards, i, cw, ch);
            shown++;
        }
    }
    if (!shown) {
        lv_obj_t *box = nt_box(S.cards, inner, 420);
        lv_obj_t *ic = nt_icon_btn(box, S.query[0] ? NT_IC_SEARCH : NT_IC_NOTE, 140, NULL, NULL);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        nt_icon_color(ic, NT.hair);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 60);
        lv_obj_t *l = nt_text(box, S.query[0] ? _("Nada coincide con la búsqueda") :
                              S.filter == F_TASKS ? _("Todavía no hay listas") : _("Todavía no hay notas"),
                              F(NT_ST_BOLD, NT_SZ_M), NT.dim);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 220);
        if (!S.query[0]) {
            lv_obj_t *h = nt_text(box, _("Tocá + para crear una"), F(0, NT_SZ_S), NT.dim);
            lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 270);
        }
    }
    nt_box(S.cards, inner, 160);
}

static void filter_cb(lv_event_t *e)
{
    S.filter = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_target(e));
    for (uint32_t k = 0; k < lv_obj_get_child_count(row); k++) {
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)k);
        lv_obj_set_state(c, LV_STATE_CHECKED, (int)k == S.filter);
    }
    fill_cards();
}

static void new_pick(int i, void *ud)
{
    (void)ud;
    new_note(i == 0 ? NT_KIND_NOTE : NT_KIND_TASKS);
}

static void fab_cb(lv_event_t *e)
{
    (void)e;
    nt_item_t it[2] = {
        { _("Nota"), NT_IC_NOTE, false, false },
        { _("Lista de tareas"), NT_IC_TASKS, false, false },
    };
    nt_sheet(_("Crear"), it, 2, new_pick, NULL);
}

static void search_done(lv_obj_t *ta)
{
    (void)ta;
    show_panel(P_NONE);
}

static void search_changed_cb(lv_event_t *e)
{
    snprintf(S.query, sizeof S.query, "%s", lv_textarea_get_text(lv_event_get_target(e)));
    fill_cards();
}

static void list_search_attach(void)
{
    if (S.search_ta) nt_kb_attach_textarea(S.search_ta, search_done);
}

static void search_focus_cb(lv_event_t *e)
{
    (void)e;
    S.searching = true;
    show_panel(P_KB);
}

/* the main menu */
static void order_pick(int i, void *ud)
{
    (void)ud;
    S.order = i;
    save_prefs();
    build();
}

static void empty_trash(void *ud)
{
    (void)ud;
    int n = nt_purge_all();
    char buf[64];
    snprintf(buf, sizeof buf, _("%d borradas"), n);
    aos_ui_toast(buf, 1500);
    build();
}

static void main_pick(int i, void *ud)
{
    (void)ud;
    switch (i) {
    case 0:
        nt_theme_set(!NT.dark);
        save_prefs();
        statusbar();
        build();
        break;
    case 1: {
        nt_item_t it[3] = {
            { _("Última modificación"), -1, false, S.order == NT_ORDER_MODIFIED },
            { _("Fecha de creación"), -1, false, S.order == NT_ORDER_CREATED },
            { _("Título"), -1, false, S.order == NT_ORDER_TITLE },
        };
        nt_sheet(_("Ordenar por"), it, 3, order_pick, NULL);
        break;
    }
    case 2:
        S.rows = !S.rows;
        save_prefs();
        build();
        break;
    case 3:
        S.screen = SCR_TRASH;
        build();
        break;
    }
}

static void main_menu_cb(lv_event_t *e)
{
    (void)e;
    nt_item_t it[4] = {
        { NT.dark ? _("Fondo claro") : _("Fondo oscuro"), NT.dark ? NT_IC_SUN : NT_IC_MOON, false, false },
        { _("Ordenar por..."), NT_IC_ROWS, false, false },
        { S.rows ? _("Ver en cuadrícula") : _("Ver en lista"), S.rows ? NT_IC_GRID : NT_IC_ROWS, false, false },
        { _("Papelera"), NT_IC_TRASH, false, false },
    };
    nt_sheet(_("Notas"), it, 4, main_pick, NULL);
}

static void build_list(void)
{
    lv_obj_t *page = S.page;
    nt_index_scan(&S.ix, false);
    nt_index_sort(&S.ix, S.order);
    S.dir_sig = nt_dir_signature();

    lv_obj_t *head = nt_box(page, S.W, S.land ? 84 : 104);
    lv_obj_set_style_pad_hor(head, 24, 0);
    lv_obj_t *t = nt_text(head, _("Notas"), F(NT_ST_BOLD, NT_SZ_XL), NT.text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 4);
    lv_obj_t *more = nt_icon_btn(head, NT_IC_MORE, 72, main_menu_cb, NULL);
    nt_icon_color(more, NT.accent);
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, 0, 0);

    /* search, and the filters */
    lv_obj_t *tools = nt_box(page, S.W, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(tools, 24, 0);
    lv_obj_set_style_pad_bottom(tools, 8, 0);
    lv_obj_set_flex_flow(tools, S.land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tools, 14, 0);
    lv_obj_set_style_pad_column(tools, 20, 0);
    int32_t sw = S.land ? 520 : S.W - 48;
    lv_obj_t *sb = nt_box(tools, sw, 68);
    lv_obj_set_style_radius(sb, 20, 0);
    lv_obj_set_style_bg_color(sb, NT.surface, 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_t *si = nt_icon_btn(sb, NT_IC_SEARCH, 56, NULL, NULL);
    lv_obj_remove_flag(si, LV_OBJ_FLAG_CLICKABLE);
    nt_icon_color(si, NT.dim);
    lv_obj_align(si, LV_ALIGN_LEFT_MID, 8, 0);
    S.search_ta = lv_textarea_create(sb);
    lv_obj_set_size(S.search_ta, sw - 80, 60);
    lv_obj_align(S.search_ta, LV_ALIGN_LEFT_MID, 66, 0);
    lv_textarea_set_one_line(S.search_ta, true);
    lv_textarea_set_placeholder_text(S.search_ta, _("Buscar"));
    lv_textarea_set_text(S.search_ta, S.query);
    lv_obj_set_style_text_font(S.search_ta, F(0, NT_SZ_M), 0);
    lv_obj_set_style_text_color(S.search_ta, NT.text, 0);
    lv_obj_set_style_text_color(S.search_ta, NT.dim, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_opa(S.search_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(S.search_ta, 0, 0);
    lv_obj_set_style_pad_all(S.search_ta, 10, 0);
    lv_obj_set_style_bg_color(S.search_ta, NT.accent, LV_PART_CURSOR);
    lv_obj_set_style_border_color(S.search_ta, NT.accent, LV_PART_CURSOR);
    lv_obj_set_user_data(S.search_ta, (void *)1);       /* no auto shift */
    if (S.searching) lv_obj_add_state(S.search_ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(S.search_ta, search_focus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(S.search_ta, search_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *chips = nt_box(tools, LV_SIZE_CONTENT, 68);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 10, 0);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const names[] = { N_("Todas"), N_("Notas"), N_("Listas") };
    for (int k = 0; k < 3; k++) {
        lv_obj_t *c = nt_pill(chips, _(names[k]), filter_cb, (void *)(intptr_t)k);
        if (k == S.filter) lv_obj_add_state(c, LV_STATE_CHECKED);
    }

    S.cards = lv_obj_create(page);
    lv_obj_remove_style_all(S.cards);
    lv_obj_set_width(S.cards, S.W);
    lv_obj_set_flex_grow(S.cards, 1);
    lv_obj_set_style_pad_hor(S.cards, 24, 0);
    lv_obj_set_style_pad_top(S.cards, 8, 0);
    lv_obj_set_style_pad_row(S.cards, 16, 0);
    lv_obj_set_style_pad_column(S.cards, 16, 0);
    lv_obj_set_flex_flow(S.cards, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(S.cards, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(S.cards, LV_SCROLLBAR_MODE_ACTIVE);
    fill_cards();
    lv_obj_update_layout(S.cards);
    lv_obj_scroll_to_y(S.cards, S.list_scroll, LV_ANIM_OFF);

    S.kb_area = nt_box(page, S.W, S.kb_h);
    lv_obj_add_flag(S.kb_area, LV_OBJ_FLAG_HIDDEN);

    /* the + */
    lv_obj_t *fab = S.fab = lv_obj_create(page);
    lv_obj_remove_style_all(fab);
    lv_obj_add_flag(fab, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(fab, 120, 120);
    lv_obj_set_style_radius(fab, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fab, NT.accent, 0);
    lv_obj_set_style_bg_opa(fab, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(fab, 30, 0);
    lv_obj_set_style_shadow_opa(fab, LV_OPA_40, 0);
    lv_obj_set_style_transform_scale(fab, 235, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(fab, 60, 0);
    lv_obj_set_style_transform_pivot_y(fab, 60, 0);
    lv_obj_align(fab, LV_ALIGN_BOTTOM_RIGHT, -32, -40);
    lv_obj_add_event_cb(fab, fab_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = nt_icon_btn(fab, NT_IC_PLUS, 120, NULL, NULL);
    lv_obj_remove_flag(plus, LV_OBJ_FLAG_CLICKABLE);
    nt_icon_color(plus, lv_color_black());

    if (S.searching) show_panel(P_KB);
}

/* -------------------------------------------------------------------------- */
/* The bin                                                                     */
/* -------------------------------------------------------------------------- */

static void trash_back_cb(lv_event_t *e)
{
    (void)e;
    S.screen = SCR_LIST;
    build();
}

static void trash_entry_pick(int i, void *ud)
{
    (void)ud;
    int k = S.menu_entry;
    if (k < 0 || k >= S.ix.n) return;
    if (i == 0) {
        nt_restore(S.ix.e[k].file);
        aos_ui_toast(_("Restaurada"), 1200);
    } else {
        nt_purge(S.ix.e[k].file);
    }
    build();
}

static void trash_row_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    S.menu_entry = k;
    nt_item_t it[2] = {
        { _("Restaurar"), NT_IC_RESTORE, false, false },
        { _("Borrar para siempre"), NT_IC_TRASH, true, false },
    };
    nt_sheet(S.ix.e[k].title[0] ? S.ix.e[k].title : _("Sin título"), it, 2, trash_entry_pick, NULL);
}

static void empty_cb(lv_event_t *e)
{
    (void)e;
    if (!S.ix.n) return;
    nt_confirm(_("¿Vaciar la papelera?"), _("Las notas se borran para siempre."), _("Vaciar"), true,
               empty_trash, NULL);
}

static void build_trash(void)
{
    lv_obj_t *page = S.page;
    nt_index_scan(&S.ix, true);
    nt_index_sort(&S.ix, NT_ORDER_MODIFIED);
    lv_obj_t *bar = topbar(page, _("Notas"), trash_back_cb);
    lv_obj_t *empty = nt_pill(bar, _("Vaciar"), empty_cb, NULL);
    lv_obj_set_style_text_color(empty, NT.danger, 0);
    lv_obj_align(empty, LV_ALIGN_RIGHT_MID, -8, 0);
    if (!S.ix.n) lv_obj_add_state(empty, LV_STATE_DISABLED);

    lv_obj_t *t = nt_text(page, _("Papelera"), F(NT_ST_BOLD, NT_SZ_XL), NT.text);
    lv_obj_set_style_pad_hor(t, 24, 0);
    lv_obj_set_style_pad_bottom(t, 12, 0);

    lv_obj_t *list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, S.W);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    int32_t w = LV_MIN(S.W - 48, 900);
    for (int i = 0; i < S.ix.n; i++) {
        nt_entry_t *en = &S.ix.e[i];
        lv_obj_t *r = lv_obj_create(list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, w, 120);
        lv_obj_set_style_radius(r, 20, 0);
        lv_obj_set_style_bg_color(r, nt_tag_card(en->meta.tag), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(r, 18, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, trash_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *a = nt_text(r, en->title[0] ? en->title : _("Sin título"), F(NT_ST_BOLD, NT_SZ_M), NT.text);
        lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(a, w - 36);
        lv_obj_t *b = nt_text(r, en->text ? en->text : "", F(0, NT_SZ_S), NT.dim);
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(b, w - 36, 32);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    if (!S.ix.n) {
        lv_obj_t *l = nt_text(list, _("La papelera está vacía"), F(0, NT_SZ_M), NT.dim);
        lv_obj_set_style_pad_top(l, 80, 0);
    }
}

/* -------------------------------------------------------------------------- */
/* Building a screen                                                           */
/* -------------------------------------------------------------------------- */

static void build(void)
{
    if (!S.root) return;
    nt_sheet_forget();
    lv_obj_clean(S.root);
    nt_ed_destroyed();
    nt_tasks_destroyed();
    nt_kb_destroyed();
    S.page = S.cards = S.search_ta = S.kb = S.kb_area = S.fab = NULL;
    S.topbar = S.toolbar = S.btn_undo = S.btn_redo = NULL;

    lv_obj_update_layout(S.root);
    S.W = lv_obj_get_width(S.root);
    S.H = lv_obj_get_height(S.root);
    S.land = S.W > S.H;
    S.kb_h = S.land ? S.H * 46 / 100 : S.H * 35 / 100;
    if (S.kb_h > 440) S.kb_h = 440;
    nt_ui_root = S.root;

    lv_obj_set_style_bg_color(S.root, NT.bg, 0);
    lv_obj_set_style_bg_opa(S.root, LV_OPA_COVER, 0);
    S.page = nt_box(S.root, S.W, S.H);
    lv_obj_set_flex_flow(S.page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(S.page, S.screen == SCR_LIST || S.screen == SCR_TRASH ? NT.bg :
                              (NT.dark ? NT.bg : NT.surface), 0);
    lv_obj_set_style_bg_opa(S.page, LV_OPA_COVER, 0);

    switch (S.screen) {
    case SCR_NOTE:
    case SCR_TASKS: build_note(); break;
    case SCR_TRASH: build_trash(); break;
    default:        build_list(); break;
    }
}

/* -------------------------------------------------------------------------- */
/* The app                                                                     */
/* -------------------------------------------------------------------------- */

static void *notas_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    S.root = root;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    int32_t v = 1;
    aos_hal_pref_get_i32("nt_dark", &v);
    nt_theme_set(v != 0);
    v = 0;
    aos_hal_pref_get_i32("nt_order", &v);
    S.order = v >= 0 && v < NT_ORDER_COUNT ? v : 0;
    v = 0;
    aos_hal_pref_get_i32("nt_rows", &v);
    S.rows = v != 0;
    S.menu_entry = -1;
    if (!S.open) {
        S.screen = SCR_LIST;
        nt_seed();
    }
    if (!nt_fonts_have_pack()) aos_hal_log("notas", "without notas_p4.pak: no bold or italic");
    statusbar();
    build();
    return &S;
}

static void notas_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (S.open) {
        if (S.screen == SCR_TASKS) nt_tasks_commit();
        if (doc_empty()) { if (S.file[0]) nt_trash(S.file); }
        else save_now(true);
    }
    S.typing_stop = true;
    nt_sheet_forget();
    nt_ed_forget();
    nt_ed_destroyed();
    nt_ed_clip_free();
    nt_tasks_forget();
    nt_tasks_destroyed();
    nt_kb_destroyed();
    nt_doc_clear(&S.doc);
    nt_index_free(&S.ix);
    aos_ui_statusbar_style(AOS_BAR_LIGHT);
    /* the typing thread holds its own copy and only reads S.typing_stop */
    if (!S.typing) nt_fonts_free();
    S.open = false;
    S.root = NULL;
}

static void notas_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    statusbar();
    /* the portal may have changed the folder, or the note, meanwhile */
    if (S.screen == SCR_LIST) build();
    else watch_card();
}

static void notas_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (S.dirty) save_now(false);
    aos_ui_statusbar_style(AOS_BAR_LIGHT);
}

static bool notas_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (nt_sheet_open()) { nt_sheet_close(); return true; }
    switch (S.screen) {
    case SCR_NOTE:
        if (nt_ed_has_selection()) { nt_ed_clear_selection(); return true; }
        if (S.panel != P_NONE) { show_panel(P_NONE); return true; }
        close_note();
        return true;
    case SCR_TASKS:
        if (nt_tasks_back()) return true;
        close_note();
        return true;
    case SCR_TRASH:
        S.screen = SCR_LIST;
        build();
        return true;
    default:
        if (S.searching || S.query[0]) {
            S.searching = false;
            S.query[0] = 0;
            build();
            return true;
        }
        return false;
    }
}

/* The portal writes the same files. Every two seconds: the open note, if it
 * changed on the card and there is nothing unsaved here, is read again (and
 * closed if it was moved to the bin); the notes screen follows the folder.
 * Not while the keyboard, a panel or a sheet is up: someone is using it. */
static void watch_card(void)
{
    if (nt_sheet_open() || S.panel != P_NONE) return;
    if (S.open && S.file[0] && !S.dirty) {
        uint32_t m, z;
        if (!nt_file_stat(S.file, &m, &z)) {
            S.open = false;
            nt_ed_forget();
            nt_tasks_forget();
            nt_doc_clear(&S.doc);
            S.screen = SCR_LIST;
            build();
            aos_ui_toast(_("La nota se borró desde el portal"), 2000);
        } else if (m != S.f_mtime || z != S.f_size) {
            int32_t keep = S.screen == SCR_NOTE ? nt_ed_get_scroll() : nt_tasks_get_scroll();
            nt_doc_clear(&S.doc);
            nt_doc_init(&S.doc);
            nt_load(nt_dir(), S.file, &S.doc, &S.meta);
            S.f_mtime = m;
            S.f_size = z;
            nt_ed_forget();
            nt_tasks_forget();
            S.screen = S.meta.kind == NT_KIND_TASKS ? SCR_TASKS : SCR_NOTE;
            build();
            if (S.screen == SCR_NOTE) nt_ed_set_scroll(keep);
            else nt_tasks_set_scroll(keep);
            aos_ui_toast(_("Actualizada desde el portal"), 1500);
        }
    } else if (S.screen == SCR_LIST && !S.searching) {
        uint32_t sig = nt_dir_signature();
        if (sig != S.dir_sig) {
            S.list_scroll = S.cards ? lv_obj_get_scroll_y(S.cards) : 0;
            build();
        }
    }
}

static void notas_tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (S.dirty && lv_tick_elaps(S.dirty_ms) > SAVE_AFTER_MS) save_now(false);
    if (++S.ticks % 10 == 0) watch_card();
}

static bool notas_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    S.root = root;
    int32_t keep = S.screen == SCR_NOTE ? nt_ed_get_scroll() : S.screen == SCR_TASKS ? nt_tasks_get_scroll() : 0;
    if (S.screen == SCR_TASKS) nt_tasks_commit();
    if (S.screen == SCR_LIST) S.list_scroll = 0;
    build();
    if (S.screen == SCR_NOTE) { nt_ed_set_scroll(keep); nt_ed_scroll_to_caret(); }
    else if (S.screen == SCR_TASKS) nt_tasks_set_scroll(keep);
    return true;
}

static bool notas_init(aos_app_t *app)
{
    app->desc.id       = "aos.notas";
    app->desc.name     = "Notas";
    app->desc.icon     = LV_SYMBOL_EDIT;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xFFD60A;
    app->desc.color_b  = 0xFF9F0A;
    app->desc.flags    = AOS_APP_FLAG_KEEP;
    app->desc.order    = 120;
    aos_icon_set_ops(app, NOTAS_ICON, sizeof NOTAS_ICON);

    app->create  = notas_create;
    app->destroy = notas_destroy;
    app->show    = notas_show;
    app->hide    = notas_hide;
    app->back    = notas_back;
    app->tick    = notas_tick;
    app->resize  = notas_resize;
    return true;
}

AOS_APP_ENTRY(notas_init);
