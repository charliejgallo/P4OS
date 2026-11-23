/*
 * DERIVA - a player that is not there: the demo on the title screen
 * (good), and for tools/harness.c a person with a thumb on the stick
 * (BOT_PERSON): they decide every 150 ms what they saw 220 ms ago, aim
 * a little off, miss one danger in five, jump only half the times they
 * should, and keep the fire button down most of the time.
 */
#pragma once

#include "ast_game.h"

enum { BOT_PERSON, BOT_GOOD };

typedef struct {
    int       skill;
    float     react, think, attention, aim_err, jump_k, fire_hold;
    float     t, next;
    ag_hand_t q[32];
    float     q_t[32];
    int       qn;
    ag_hand_t now;
    uint32_t  rnd;
    bool      jump_sent;
    float     wander;
} ast_bot_t;

void ast_bot_init(ast_bot_t *b, int skill, uint32_t seed);
/* the hand for ship s this step */
void ast_bot_hand(ast_bot_t *b, const ag_game_t *g, int s, ag_hand_t *out);
