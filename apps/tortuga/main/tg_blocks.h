/*
 * Tortuga - the blocks editor (tg_blocks.c): a column of pieces that are
 * touched, never typed. The program is kept as Logo, one block per line,
 * so the run, the pieces and "see it in" are Logo's.
 */
#pragma once

#include "tg_app.h"

void tg_blocks_open(lv_obj_t *scr, const tg_level_t *lv, int world, const char *logo);
void tg_blocks_close(void);
char *tg_blocks_logo(void);                     /* tg_free */
void tg_blocks_mark(int line, bool error);      /* 0: none */
void tg_blocks_place(int x, int y, int w, int h);
void tg_blocks_pad(aos_pad_t *p);
