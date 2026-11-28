# The board's Telegram bot

Since 2026-11-27. The board answers in a Telegram chat and says what
happens: its state, a screenshot, the Zigbee devices with buttons to switch
them, the IR codes kept with a Zigbee blaster, a notice on its screen; and
rules that send a message or a screenshot, or switch something, when a door
opens, at an hour, when the board starts or when a notification reaches it.

It is a service of the firmware (`components/aos_apps/aos_bot.c`, API in
`include/aos_bot.h`), so it runs with no app open. Its settings: the **Bot**
app on the board (`aos_app_bot.c`) and the portal's **Bot** page
(`aos_portal_bot.c`, `#bot` in `web/app.js`).

It is not a Telegram client: the bot is a bot account of your own, made with
`@BotFather`, and the board speaks the Bot API (HTTPS and JSON) as that bot.
A client with your own account is the Mensajes app (a card app, MTProto).

## Setting it up

1. In Telegram, **`@BotFather`**, `/newbot`: a name and a username ending in
   `bot`. It gives a token, `123456789:AA...`.
2. The token goes into the board: the Bot app (Token, Load) or, easier to
   paste, the portal's Bot page. It is kept in the pref `sec_bot_token`
   (a backup without a passphrase leaves it out, `docs/BACKUP.md`) and is
   never shown again, on the board or the portal.
3. Switch it on. It says "Online · @yourbot" once Telegram answers.
4. **Link your chat:** Bot app, *Link a chat*. The board shows six digits
   for ten minutes; send the bot `/vincular 123456` (or `/link 123456`).
   Up to eight chats. Anyone else who writes to the bot is told how to
   link, once every ten minutes, and nothing more.

While it is on, a robot in the status bar says how it is: blue online,
grey connecting or without network, orange with a problem (no token, a
token Telegram refuses, another error).

## Commands

Each one can be switched off in the app or the portal. The menu Telegram
shows is in the board's language; both names work.

| Spanish | English | What |
|---|---|---|
| `/ayuda` | `/help` | what it can do (only the commands that are on) |
| `/estado` | `/status` | name, firmware, time, uptime, network, free memory, the app on screen, Zigbee devices, battery |
| `/captura` | `/screenshot` | the screen as a JPEG (it goes through the card: `<sd>/bot/captura.jpg`) |
| `/zigbee` | `/zigbee` | the devices with their values, and a pair of buttons for each output (a two-relay module gets two) |
| `/prender <name> [n]`, `/apagar <name> [n]` | `/on`, `/off` | a device by its name (exact, then contained); with several outputs (a two-relay module), `n` picks one and without it they all switch |
| `/ir [name]` | `/ir` | the IR codes kept with the Zigbee blaster as buttons, or one sent by name |
| `/aviso <text>` | `/notice` | a notification on the board's screen, from whoever sent it |
| `/reglas` | `/rules` | the rules, with buttons to pause or resume each |

"Apps may send messages" lets a card app call `aos_bot_send(text)` (in the
symbol table): it goes to every linked chat.

## Rules

Up to 16, each "when … do …", at most once every 30 s to 1 h (so a door
that bangs does not flood the chat):

- **When:** a Zigbee device changes (on/off, the IAS alarm bit — open,
  motion, water —, occupancy, temperature, humidity, battery, level, or an
  event such as a button), with *changes*, *goes above*, *goes below* or
  *becomes*; an hour on chosen days; the board starts; a notification
  arrives (all, or the ones whose app or title has a text: forwarding
  WhatsApp's or Home Assistant's to Telegram).
- **Do:** send a message, send a screenshot, switch a Zigbee device on, off
  or toggle it, send a kept IR code.
- **A device with several outputs** (a two-relay module): a rule watches one
  of them, and switches one or all of them.
- **The message** may carry `{dispositivo} {valor} {hora} {placa} {app}
  {titulo} {texto}` (or `{device} {value} {time} {board} {app} {title}
  {text}`); empty, a usual one ("🔔 Door: open").
- Crossings only: "goes above 30" fires when it goes from 30 or less to more
  than 30, not every reading above. A time rule fires as the clock enters
  its minute, so a restart at that minute does not say it twice. "When the
  board starts" fires once Telegram answers after a boot, only if the bot
  was already on at the boot.

They are kept in the prefs `bot_rn` and `bot_r0`..`bot_r15`, one line each.

## How it works

- **Two threads, two connections** kept open to the API: `bot_in` long-polls
  `getUpdates` (25 s) and handles each update; `bot_out` empties the outbox
  (16 jobs) and runs the rules once a second. With one connection every
  alert would wait for the long poll. A new token or the switch cuts the
  long poll within a second.
- **The offset** of the last update read is kept (`bot_offset`), so a
  restart does not run the last commands again.
- **Memory:** nothing but its settings (~10 KB of PSRAM) until it is first
  switched on; then the outbox (~58 KB), two copies of the Zigbee table
  (~70 KB) and two 10 KB stacks, all in PSRAM. TLS through the HAL's
  `aos_tls_*` (mbedtls' buffers in PSRAM too).
- **Errors:** 401 stops it until the token changes ("Telegram doesn't
  recognise the token"); 409 means a webhook or another program polls the
  same bot; the network's, retried with a backoff up to a minute.

## Trying it without Telegram

`tools/fake_telegram_bot.py` fakes the Bot API (getMe, getUpdates with the
long poll, sendMessage, sendPhoto, answerCallbackQuery, setMyCommands) and
the person on the other side:

```
tools/fake_telegram_bot.py --port 18081
# the simulator's prefs: bot_on=1, sec_bot_token=<the fake's token>, bot_api=http://127.0.0.1:18081
curl 'localhost:18081/_say?text=/estado'
curl 'localhost:18081/_tap?data=ir:1'
curl 'localhost:18081/_log'
```

Tried in the simulator (2026-11-27): linking with the code, a wrong code,
an unlinked chat, every command, a screenshot, the IR buttons, a time rule,
the boot rule with a screenshot, a wrong and a good token, off and on.
Not tried yet: the board against Telegram itself, a Zigbee rule with real
devices, notification forwarding.
