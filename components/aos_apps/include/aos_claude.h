/*
 * P4OS - how much of the Claude plan is used, straight from Anthropic
 * (aos_claude.c).
 *
 * The same numbers Claude Code's /usage shows: the 5-hour window and the
 * week, as percentages of the plan, with the time each resets. They are
 * the account's, so they are right however many machines use it.
 *
 * Where they come from: GET https://api.anthropic.com/api/oauth/usage with
 * the subscription's OAuth token, the endpoint Claude Code itself calls.
 * It is NOT a documented public API: its shape can change without notice,
 * and the service says so when an answer does not parse.
 *
 * The token: the P4 has a login of its own, made once from the portal
 * (docs/CLAUDE-APP.md): aos_claude_login_url() gives a link to Claude's login
 * page (OAuth with PKCE), the user signs in there and pastes the code it
 * shows back into the portal, aos_claude_login_code() trades it for a
 * token pair, and from then on the service refreshes the token itself. It
 * never touches any computer's Claude Code login. The tokens live in the
 * preferences and never leave the board (the portal only says whether
 * there is one).
 *
 * A thread of its own asks every few minutes, keeps the history on the
 * card (data/claude_hist.bin, a week at 5-minute steps) and hands the
 * latest to whoever asks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AOS_CLAUDE_SIGNED_OUT = 0,  /* no token: sign in from the portal */
    AOS_CLAUDE_WAITING_CODE,    /* a login was started: paste the code */
    AOS_CLAUDE_EXCHANGING,      /* trading the code for a token */
    AOS_CLAUDE_OK,              /* signed in; the numbers below are fresh or on their way */
    AOS_CLAUDE_EXPIRED,         /* the refresh was refused: sign in again */
    AOS_CLAUDE_ERROR,           /* the last request failed; retrying */
} aos_claude_state_t;

typedef struct {
    bool   valid;
    float  pct;                 /* 0-100 */
    time_t resets;              /* 0 unknown */
} aos_claude_window_t;

typedef struct {
    aos_claude_state_t state;
    char   error[192];          /* the last reason, in Spanish, for the user */
    aos_claude_window_t five_hour, seven_day, seven_day_opus, seven_day_sonnet;
    bool   extra_enabled;       /* pay-as-you-go beyond the plan */
    float  extra_pct;           /* of its monthly limit, -1 unknown */
    time_t fetched;             /* when the numbers above arrived, 0 never */
    uint32_t seq;               /* bumps with every change */
} aos_claude_status_t;

void aos_claude_start(void);                        /* idempotent */
void aos_claude_status(aos_claude_status_t *out);
void aos_claude_refresh_now(void);                  /* someone is looking: ask soon */

/* Login, from the portal. url() starts a new PKCE exchange (forgetting any
 * previous one) and writes the address to open; code() takes what Claude's
 * page shows ("code#state") and trades it in the background: the state goes
 * EXCHANGING, then OK or back to WAITING_CODE with the reason in error.
 * false when the text is not a code at all. */
bool aos_claude_login_url(char *out, size_t n);
bool aos_claude_login_code(const char *pasted);
void aos_claude_logout(void);

/* The history of a window (0: 5 hours, 1: the week), oldest first: the
 * percentage and when (epoch seconds). Returns how many. */
#define AOS_CLAUDE_HIST_MAX 2016
int aos_claude_history(int which, float *pct, time_t *when, int max);

#ifdef __cplusplus
}
#endif
