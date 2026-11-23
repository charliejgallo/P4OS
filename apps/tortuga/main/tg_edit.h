/*
 * Tortuga - the text editor (Logo, Lua, Python): a textarea with line
 * numbers and the running line marked, the system keyboard (a USB or
 * Bluetooth one types into it too), and a palette that writes the orders
 * and the structures with their indentation (tg_edit.c).
 */
#pragma once

#include "tg_app.h"

void tg_edit_open(lv_obj_t *scr, int lang, const tg_level_t *lv, int world, const char *text,
                  void (*focus)(bool editing));
void tg_edit_close(void);
const char *tg_edit_text(void);
void tg_edit_mark(int line, bool error);        /* 0: none */
void tg_edit_place(int x, int y, int w, int h);
void tg_edit_keyboard(bool show);
int  tg_edit_keyboard_height(void);
