/*
 * SOLITARIO - deals the solver has won, per game (so_seeds.c is generated
 * by tools/so_harness.c seeds). The app deals from here when it cannot
 * search (the day's hand) or the search takes too long on the board.
 * They were solved with the stock limited to three passes, so they hold
 * with or without that handicap.
 */
#pragma once

#include <stdint.h>
#include "so_game.h"

extern const uint32_t *const so_seed_tab[SO_NMODES];
extern const uint16_t so_seed_n[SO_NMODES];
