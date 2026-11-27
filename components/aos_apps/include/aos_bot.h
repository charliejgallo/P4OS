/*
 * P4OS - The board's Telegram bot (aos_bot.c): the board answers in a chat
 * and says what happens, from anywhere.
 *
 * A service, not an app: it runs from boot while it is on, with the Bot app
 * (aos_app_bot.c) and the portal's #bot page as its settings. It speaks the
 * Bot API (HTTPS + JSON) to api.telegram.org, or to the address in the pref
 * "bot_api" (the fake server of tools/fake_telegram_bot.py).
 *
 * Who can talk to it: only the chats linked to the board. A chat links by
 * sending "/vincular <code>" with the six digits the Bot app shows on the
 * board's screen (valid ten minutes); anyone else is told how and nothing
 * more. The token is a secret (pref "sec_bot_token": a backup without a
 * passphrase leaves it out) and is never shown back, on the board or the
 * portal.
 *
 * What it does:
 *   - commands, each one on or off: the board's state, a screenshot, the
 *     Zigbee devices with buttons to switch them, the IR codes kept with the
 *     Zigbee blaster, a notice on the board's screen, the rules;
 *   - rules: when a Zigbee device changes (a door opens, a temperature goes
 *     over a value), at an hour of chosen days, when the board starts, or
 *     when a notification reaches the board, it sends a message or a
 *     screenshot, switches a Zigbee device or sends an IR code;
 *   - apps can send a message to the linked chats: aos_bot_send().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AOS_BOT_OFF = 0,
    AOS_BOT_NO_TOKEN,
    AOS_BOT_WAITING_NET,
    AOS_BOT_CONNECTING,
    AOS_BOT_ONLINE,
    AOS_BOT_BAD_TOKEN,      /* Telegram said 401: the token is wrong or was revoked */
    AOS_BOT_ERROR,          /* see why; it retries by itself */
} aos_bot_state_t;

typedef struct {
    aos_bot_state_t state;
    char     username[40];  /* the bot's @name, once Telegram said it */
    char     why[100];      /* the last error, in words */
    uint32_t received, sent;
    int      chats;         /* linked */
    uint32_t code;          /* the link code being shown, 0 none */
    uint32_t code_left_s;
} aos_bot_info_t;

/* The commands that can be turned off (all on by default) */
enum {
    AOS_BOT_CMD_STATE   = 1u << 0,  /* /estado */
    AOS_BOT_CMD_SHOT    = 1u << 1,  /* /captura */
    AOS_BOT_CMD_ZIGBEE  = 1u << 2,  /* /zigbee, /prender, /apagar */
    AOS_BOT_CMD_IR      = 1u << 3,  /* /ir */
    AOS_BOT_CMD_NOTICE  = 1u << 4,  /* /aviso: a notice on the board */
    AOS_BOT_CMD_RULES   = 1u << 5,  /* /reglas: list them, turn them on and off */
    AOS_BOT_CMD_APPS    = 1u << 6,  /* apps may send messages (aos_bot_send) */
    AOS_BOT_CMD_ALL     = 0x7F,
};

#define AOS_BOT_CHATS_MAX 8
typedef struct {
    int64_t id;
    char    name[48];
} aos_bot_chat_t;

/* ---- rules ---- */

#define AOS_BOT_RULES_MAX 16
enum { AOS_BOT_T_ZIGBEE = 0, AOS_BOT_T_TIME, AOS_BOT_T_BOOT, AOS_BOT_T_NOTIF, AOS_BOT_T_N };
/* what of a Zigbee device a rule watches */
enum { AOS_BOT_V_ON = 0,    /* on/off (its first output) */
       AOS_BOT_V_ALARM,     /* a contact open, motion, water: IAS's alarm bit */
       AOS_BOT_V_OCC,       /* occupancy */
       AOS_BOT_V_TEMP,      /* degC */
       AOS_BOT_V_HUM,       /* % */
       AOS_BOT_V_BAT,       /* % */
       AOS_BOT_V_LEVEL,     /* 0-100 % */
       AOS_BOT_V_EVENT,     /* it did something itself: a button, a remote */
       AOS_BOT_V_N };
enum { AOS_BOT_OP_CHANGE = 0, AOS_BOT_OP_ABOVE, AOS_BOT_OP_BELOW, AOS_BOT_OP_IS, AOS_BOT_OP_N };
enum { AOS_BOT_A_MESSAGE = 0, AOS_BOT_A_SHOT, AOS_BOT_A_ZB_ON, AOS_BOT_A_ZB_OFF, AOS_BOT_A_ZB_TOGGLE,
       AOS_BOT_A_IR, AOS_BOT_A_N };

typedef struct {
    bool     on;
    uint8_t  trig;          /* AOS_BOT_T_* */
    /* T_ZIGBEE */
    uint8_t  ieee[8];
    uint8_t  var, op;       /* AOS_BOT_V_*, AOS_BOT_OP_* */
    float    value;         /* above/below/is: degC, %, 1/0 */
    /* T_TIME: minutes after midnight, the days (bit 0 Sunday .. bit 6 Saturday) */
    uint16_t at_min;
    uint8_t  days;
    /* T_NOTIF: only those whose app or title has this ("" all) */
    char     filter[40];
    /* the action */
    uint8_t  act;           /* AOS_BOT_A_* */
    uint8_t  act_ieee[8];   /* the Zigbee device to switch */
    char     ir[33];        /* the IR code's name (kept with the Zigbee blaster) */
    /* the message, with {dispositivo} {valor} {hora} {placa} {app} {titulo} {texto}; "" a default one */
    char     text[160];
    uint16_t gap_s;         /* at most once every gap_s seconds (0: 30) */
    /* a device with several outputs (a two-relay module): which one is
     * watched (AOS_BOT_V_ON) and which one the action switches; 0 the
     * first, AOS_BOT_OUT_ALL every one (the action only) */
    uint8_t  out, act_out;
} aos_bot_rule_t;
#define AOS_BOT_OUT_ALL 0xFF

/* Starts the service (from main, once): it waits on its own for the switch,
 * the token and the network. */
void aos_bot_boot(void);

void aos_bot_info(aos_bot_info_t *out);
bool aos_bot_enabled(void);
void aos_bot_enable(bool on);
bool aos_bot_has_token(void);
/* "" forgets it. Checked by Telegram on the next connection. */
bool aos_bot_set_token(const char *token);

/* A fresh link code, shown on the board for ten minutes. */
uint32_t aos_bot_link_code(void);
void     aos_bot_link_cancel(void);
int  aos_bot_chats(aos_bot_chat_t *out, int max);
bool aos_bot_chat_forget(int64_t id);

uint32_t aos_bot_cmds(void);
void     aos_bot_set_cmds(uint32_t mask);

int  aos_bot_rules(aos_bot_rule_t *out, int max);
/* i < 0 adds one; false when full or i is out of range */
bool aos_bot_rule_set(int i, const aos_bot_rule_t *r);
bool aos_bot_rule_delete(int i);
/* The rule's sentence, for lists: "Cuando Puerta se abre: mandar un mensaje" */
void aos_bot_rule_text(const aos_bot_rule_t *r, char *out, size_t n);

/* A message to every linked chat (queued; false if the bot is off, the
 * queue is full or apps may not send). From any task. Exported to apps. */
bool aos_bot_send(const char *text);

/* The last things it did, newest last: up to max lines of up to 96 chars;
 * returns how many. */
int aos_bot_log(char (*lines)[96], int max);

/* A counter that moves whenever the info, the chats, the rules or the log
 * change: the app and the portal redraw when it does. */
uint32_t aos_bot_version(void);

#ifdef __cplusplus
}
#endif
