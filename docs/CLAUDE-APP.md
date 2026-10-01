# The Claude app

> **Unofficial.** This app is not made, affiliated with or endorsed by
> Anthropic; Claude is Anthropic's. It reads your own plan's usage with your
> own sign-in, the same way Claude Code's `/usage` does, through Claude Code's
> public OAuth client (`aos_claude.c`; each address and the client id can be
> changed from the preferences). What it keeps, the tokens, stays in the
> board's preferences and only ever goes to Anthropic: no other server, no
> MQTT, no Home Assistant. Using it is subject to
> [Anthropic's terms](https://www.anthropic.com/legal), as signing in to
> Claude Code is. If Anthropic changes or closes that endpoint, the app stops
> working.

The Claude app shows how much of the Claude plan is used. It shows the same
numbers as Claude Code's `/usage`:

- the 5-hour window and the week, as percentages;
- when each one resets;
- the per-model weekly limits, when the plan has them.

The numbers belong to the account, so they are right however many computers
use it. They come straight from Anthropic, with no computer or MQTT in
between.

A home screen widget shows the two windows as bars (`widget claude 2x2` or
`4x2` in `menu.txt`).

## Signing in

The board has a login of its own, made once from the portal:

1. Open `http://<name>.local/#claude` on a computer and press **Iniciar sesión
   con Claude**.
2. Claude's login page opens. Sign in and authorize. The page shows a code;
   paste it into the portal and press **Listo**.

From then on the board refreshes its token by itself. This login is separate
from the Claude Code login on any computer: signing in here, or out, does not
affect them.

The tokens live in the board's preferences. The portal never shows them: it
only says whether there is a session. **Cerrar sesión** in the portal
forgets them.

## Where the numbers come from, and what can break

- **The endpoint:** they come from `GET https://api.anthropic.com/api/oauth/usage`,
  the endpoint Claude Code itself calls. It is not a documented public API, so
  Anthropic can change it without notice. When an answer does not parse, the
  app says so instead of showing stale numbers.
- **The OAuth client:** the login uses Claude Code's public OAuth client (PKCE,
  with the code pasted by hand). Using it from another device is a grey area
  under Anthropic's terms.
- **Overriding the URLs:** each of these can be changed in the preferences
  without a new firmware, for the day one of them moves:

  | Preference | What it sets |
  |---|---|
  | `claude_auth` | the login page |
  | `claude_token` | the token endpoint |
  | `claude_redir` | the redirect URI |
  | `claude_api` | the API base |
  | `claude_client` | the client id |

- **Tried against Anthropic (2026-09-30):** the first real login worked with
  the default URLs, scopes and answer shape; the numbers matched Claude
  Code's. If a step ever fails, the portal shows the server's own error
  message and the URLs above can be changed without a new firmware.

## What the board adds

The API gives only the current percentages. The board keeps the history
itself:

- **History:** it stores `data/claude_hist.bin` on the card, one point every
  5 minutes, a week long. That feeds the day and week charts.
- **Pace:** it works out the pace of the 5-hour window, in percent per hour
  over the last hour. It needs at least 20 minutes of history.
- **Limit estimate:** from the pace, it says when you reach the limit, or that
  the window resets first.

How often it asks: every 3 minutes, or every minute while the app is open.

## Developing without Anthropic

`tools/fake_claude_api.py` stands in for the login page, the token endpoint
(it checks PKCE and rotates refresh tokens) and the usage endpoint. Start it
with:

```
tools/fake_claude_api.py --port 8765 --expires 150
```

Then give the simulator a preferences file of its own
(`P4_SIM_PREFS=/path/prefs.txt`) that contains:

```
claude_auth=http://127.0.0.1:8765/oauth/authorize
claude_token=http://127.0.0.1:8765/v1/oauth/token
claude_api=http://127.0.0.1:8765
```
