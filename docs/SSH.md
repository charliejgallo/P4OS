# SSH: a console on the board

`ssh p4os.local` opens a shell on the board: the card's files, the log,
the tasks, the memory, the network, the apps. `ssh p4os.local log -f` runs
one command and ends. It is P4OS's own server (`components/aos_ssh`), not
a port of OpenSSH or Dropbear, which assume a Unix underneath. The idea
came from a question at Home Assistant's Community Day in Buenos Aires
(2026-11-07): "does it have an SSH console?"

## Switching it on

Settings, Portal web, **SSH (port 22)**. It listens at once (no restart)
and stops listening when it is switched off, closing the sessions open. In
the simulator, `P4_SIM_SSH_PORT=2222` starts it on that port, and the
preference `ssh_on=1` switches it on.

The first time it is on, the board makes its host key (Ed25519) and shows
its fingerprint there, under the switch. `ssh` shows the same one the first
time it connects:

    ED25519 key fingerprint is SHA256:YNT6nA1a7HUBz1FQdAVeBMB4CqOVILQ2UlO2wNWL79M.

"New key for the board" makes another; every computer that knew the old one
will then warn that the board changed (`ssh-keygen -R p4os.local` forgets
it).

## Who gets in

The portal's rules (`docs/SECURITY.md`), where the connection came in:

| Where the portal is | SSH |
|---|---|
| open (the cable, home and the board's own network without "ask always") | anyone, with no password |
| asking for the password | the portal's password, or a key on the list |
| closed (a network that is not home) | nobody: the connection is cut before it says anything |

The rules are checked again every second: a session that came in without a
password is closed when the network starts asking for one, and every
session is closed when the board leaves for a network where it is closed.
A new password, "Close every session", taking a key off the list, a new
host key or switching SSH off close the SSH sessions too.

### Keys

There is no `authorized_keys` to upload. A key not on the list asks the
board: the first time you log in with it, `ssh` prints

    This key (ED25519 SHA256:Il0Ko27E...) is not on the board's list. Look at
    its screen: you have 60 seconds to allow it.

and the board shows the key's type, the user name it came as, the address
and its fingerprint, with "Reject" and "Allow". Compare the fingerprint with
`ssh-keygen -lf ~/.ssh/id_ed25519.pub`. Allowed, it goes on the list
(Settings, Portal web, "Keys that may log in", eight at most) and does not
ask again. The question waits for the lock screen to go: with a code on the
board, letting a key in is for whoever knows it. Back and the BOOT button
are a no. Only one question at a time, and only one per connection.

That is the only way onto the list, on purpose: getting into the portal is
not enough to add a way in. The password and the rules are set on the
board only for the same reason.

The keys it takes: `ssh-ed25519`, `ecdsa-sha2-nistp256` and RSA of 2048
bits or more, signed with `rsa-sha2-256` or `rsa-sha2-512` (never `ssh-rsa`,
which is SHA-1). Not yet: security keys (`sk-ssh-ed25519@openssh.com`) or
certificates.

## The shell

    help      ls [-la]  cd    pwd   cat   head  tail  cp    mv    rm [-rf]
    mkdir [-p] rmdir   touch df    log [-f] [-p] [-n N]  top   ps    free
    uptime    date      uname ip    apps  open  home  back  who   reboot
    clear     echo      exit

- **Paths:** `/` is the card. `/usb` (and `/usb2`, `/usb3`) is a pendrive
  while one is mounted, as in the portal. `..` never leaves `/`. The card's
  root and a pendrive's cannot be deleted or moved.
- **Line editing:** arrows, Home/End, Delete, Ctrl-A/E/B/F/K/U/W/L,
  Ctrl-P/N or up/down for the last 32 lines, Tab for commands and paths
  (twice shows the choices). Ctrl-C drops the line, or stops `cat`, `log
  -f`, `top` or `cp`; `q` also ends `top`. Ctrl-D on an empty line, or
  `exit [status]`, leaves.
- **Without a terminal** (`ssh p4os.local cmd`, or `ssh -T`) there is no
  line editing and no colour, newlines stay `\n`, and the exit status is
  the command's: `ssh p4os.local cat /menu.txt > menu.txt` copies a file
  byte for byte.
- **`log`** is the ring the portal shows (`aos_logring.c`): `-n` lines
  (40), `-f` to follow it, `-p` the previous boot's tail.
- **`top`** refreshes every second (CPU per task, priority, core, stack and
  the least it ever had free; `K*` is a stack in PSRAM). `ps` once.
- **`open`** takes an app's id (`apps` lists them) or its name as the menu
  shows it; `home` and `back` are the screen's. They go through the same
  queue as the portal's `/api/open` and `/api/nav`.
- What the commands print is in the board's language (`_()`, packs in
  `lang/`); their names are the Unix ones.

Not there yet: SFTP and `scp` (macOS's `scp` speaks SFTP), port forwarding,
the agent, and editing files on the board. `cat > file` does not work: the
shell has no redirection.

## The protocol, as built

| Layer | What the board does |
|---|---|
| Transport (RFC 4253) | `curve25519-sha256` (RFC 8731), `ssh-ed25519` host key, `aes128-gcm@openssh.com` and `aes256-gcm@openssh.com` (the P4's AES engine), no compression. "Strict KEX" (`kex-strict-s-v00@openssh.com`, OpenSSH's answer to Terrapin, CVE-2023-48795) when the client offers it. The client may exchange keys again at any time (tested with `RekeyLimit=1M` during an 8 MB `cat`). `server-sig-algs` (RFC 8308), so an RSA key signs with SHA-2 |
| Authentication (RFC 4252) | `none` where the portal is open, `password` (the portal's, with its growing wait after three wrong ones), `publickey`. Six tries, two minutes to log in |
| Connection (RFC 4254) | two `session` channels per connection; `pty-req`, `shell`, `exec`, `window-change`. Everything else is refused: `direct-tcpip`, `tcpip-forward`, `subsystem`, X11, the agent |

- **Code:** `aos_ssh.c` (listener, connection, transport, authentication,
  channels), `aos_ssh_crypto.c` (the algorithms), `aos_ssh_wire.c` (SSH's
  types, a reader and a writer that mark themselves bad instead of reading
  past the end), `aos_ssh_shell.c` (line editor, jobs, paths),
  `aos_ssh_cmds.c` (the commands). The rules, the host key, the list and
  the board's question are in `components/aos_ui/aos_access_ssh.c`; the
  question's screen is `aos_sshkey_ui.c`.
- **Crypto:** mbedTLS for SHA-256/512, AES-GCM, ECDSA and RSA checks;
  Monocypher 4.0.2 (`components/monocypher`, vendored unchanged, BSD-2 or
  CC0) for X25519 and Ed25519, which mbedTLS 3.6 does not have in a usable
  form. Random numbers are the chip's: on the P4, which has no radio of its
  own, ESP-IDF keeps the SAR ADC's noise feeding the generator from boot.
- **Memory (PSRAM first):** each connection is a thread with a 20 KB stack
  in PSRAM, two packet buffers of ~35 KB and a 48 KB output buffer per
  shell, all PSRAM; the component's statics are mapped to PSRAM
  (`psram.lf`). Nothing new in internal RAM but what lwIP and the sockets
  already use. Three connections at most.
- **Why not a library:** wolfSSH is GPLv3 (P4OS is MIT), libssh is LGPL and
  wants pthreads and a big footprint, Dropbear and TinySSH are built around
  `fork()` and a Unix pty.

## The warning from OpenSSH 10

OpenSSH 10.1 and later print, on every connection,

    ** WARNING: connection is not using a post-quantum key exchange algorithm.

because the board does not do `mlkem768x25519-sha256` yet. It is a warning
about recorded traffic being decrypted by a future quantum computer, not
about the connection today. To quiet it for the board only, in
`~/.ssh/config`:

    Host p4os.local
        WarnWeakCrypto no-pq-kex

ML-KEM on the board is the natural next step for the transport.

## Testing it

On the simulator (never the user's network):

    cd sim && P4_SIM_PREFS=/tmp/x/prefs.txt P4_SIM_PORTAL_PORT=18481 \
        P4_SIM_SSH_PORT=22481 ./build/p4os_sim
    ssh -p 22481 -o UserKnownHostsFile=/tmp/x/known_hosts 127.0.0.1

`P4_SIM_ZONE=home` with `pt_ask=1` and a `pt_pass` in the preferences gives
a network that asks for the password; a `P4_SIM_SCRIPT` with `tap 454 814`
answers "Allow" on the board's question in portrait. What was checked on
2026-11-07 against OpenSSH 10.2 on macOS, in the simulator only: exec and
shell, a file of 8 MB byte for byte with and without rekeying, the password
right and wrong, the three kinds of key on the list, a new key allowed from
the screen, a key taken off the list in Settings closing its open session,
line editing, Tab, `log -f` and `top` stopped from the keyboard. Under ASan
and UBSan: 3000 connections of malformed input before authentication
(garbage, bad lengths and padding, mutated KEXINIT and ECDH_INIT) and 1200
rounds of random typing into a logged-in shell (control keys, escape
sequences, broken UTF-8, 700-character lines), with nothing found. Not yet
on the board.
