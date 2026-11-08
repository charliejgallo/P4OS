# SSH

An SSH client for P4OS: a terminal on the board into a Mac, a Raspberry Pi
or any computer with an SSH server. The other direction of the board's own
SSH server (`docs/SSH.md`). Id `aos.ssh`.

It builds two ways from the same source: the simulator compiles it in
(`-DP4OS_SIM_APPS="hello_app;ssh"`), and for the board it is a dynamic app,
`tools/build_apps.sh ssh` (~60 KB). It **needs the firmware of 2026-11-07
or later**: the crypto is the firmware's, lent to the apps
(`tools/gen_symbols.py`): Monocypher's X25519 and Ed25519,
`aos_ssh_chacha.h` and `aos_ssh_wire.h`. A board with older firmware does
not load the `.so` (the loader names the missing symbols in the log).

## What it speaks

| | |
| --- | --- |
| key exchange | curve25519-sha256 (and its @libssh.org name); strict KEX when the server offers it |
| host keys | ssh-ed25519 first, then ecdsa-sha2-nistp256, rsa-sha2-512, rsa-sha2-256 |
| cipher | chacha20-poly1305@openssh.com only: every OpenSSH since 6.5 (2014) and Dropbear since 2020.79 have it. An older server says "no cipher in common" |
| login | the board's own Ed25519 key, then the password by `password` or `keyboard-interactive` (a Mac, and PAM in general, want the second; a second question, a code, is asked too) |
| channel | one session: `pty-req` as xterm-256color with the view's size, `env LANG`, `shell`, `window-change` on every resize; the server's rekeying, keepalives and `hostkeys-00` are answered |

## The terminal

`sc_term.c` is what xterm does that a shell, top, htop, vim, nano, less and
mc use: cursor movement, erase, insert and delete, a scroll region, 16, 256
and 24-bit colour (the last to the nearest of the 256), bold as the bright
colour, dim, underline, reverse, the alternate screen, application cursor
keys, bracketed paste, auto-wrap, tab stops, DEC line drawing, the title
(OSC 0/2) and the answers to `ESC[6n` and `ESC[c`. 1000 lines of
scrollback in PSRAM: a finger dragged up and down goes through them, typing
comes back to the live screen.

The font is `aos_mono_18` (JetBrains Mono): ASCII, Latin-1, box drawing and
blocks. Anything else (CJK, emoji) is drawn as `?` so the columns stay put.
65 columns upright, 116 lying down; the keyboard on the screen takes rows
away and the server is told.

## Keys

The keyboard on the screen is VNC's made for a shell (`sc_kbd.c`): a top
row of Esc, Tab, Ctrl, Alt and the arrows on every layer, then letters,
numbers, symbols and Fn (F1-F12, Ins, Home, PgUp, Del, End, PgDn). Ctrl and
Alt stay lit for the next key. A USB keyboard types straight in
(`aos_ui_hwkbd_handler`), Ctrl and Alt included; with one plugged in, the
screen's keyboard starts hidden. A tap on the terminal shows or hides it.

## What it keeps

In the app's data folder (`/data/ssh/` on the card):

- `hosts.txt`: name, address, port and user, tab-separated. **No
  password:** it is asked for on every connection and never written.
- `known_hosts`: each computer's key the first time, with the user's yes
  after seeing its fingerprint. A changed key is refused unless the user
  accepts the new one, warned in red.
- `id_ed25519.pub`: the board's public key, the line for a computer's
  `~/.ssh/authorized_keys`.

The private key is an Ed25519 seed in the board's preferences
(`sshc_seed`), in its flash and not on the card that comes out.

## Logging in without a password

Log in once with the password and tap the padlock in the bar: a second
channel runs, on the computer,

    umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys &&
    { grep -qF '<key>' ~/.ssh/authorized_keys || echo '<line>' >> ~/.ssh/authorized_keys; }

and the terminal says how it went. From then on the board's key logs in.
The padlock on the list shows the line and its QR, for doing it by hand.
On a server that is not a Unix shell (the board's own SSH, Windows) it
says it could not.

## Testing

Only against the simulator's own SSH server (never the user's computers):

    cd sim && cmake -B build-sshc -DP4OS_SIM_APPS="hello_app;ssh" && cmake --build build-sshc -j8
    # data/ssh/hosts.txt in the simulator's card: "la placa<TAB>127.0.0.1<TAB>22484<TAB>charlie"
    SSHC_OPEN=0 SSHC_TYPE='ls\nfree\n' P4_SIM_SSH_PORT=22484 P4_SIM_PREFS=... ./build-sshc/p4os_sim

`SSHC_OPEN=<n>` connects to the n-th computer when the app opens and
`SSHC_TYPE` is typed once the shell is up (`\n` as Enter). With
`P4_SIM_ZONE=home` and a password in the preferences the server asks for
it: the password sheet, a wrong one and a right one were tried that way, as
were the host key's first time and a changed one, the padlock, `top`
redrawing live, and turning the screen with a question open.

Not tried yet: a real OpenSSH server (a Mac, a Raspberry Pi), rekeying
started by the server, and `keyboard-interactive`, which the board's own
server does not offer.
