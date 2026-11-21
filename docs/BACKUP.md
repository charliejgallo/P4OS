# Backups

What a board holds and cannot fetch again, in one file on the card: its
preferences (the system's and the apps', the networks, the accounts), the
list of the apps the Store installed with their extras, and the two small
files that say how the card is arranged (`menu.txt`, `modules.txt`). The
apps themselves are not copied: the Store brings them back from that list.

Since 0.31. `components/aos_backup`; the plan, with what comes next (a copy
a day by itself, a pendrive, a server of your own), is
[docs/plan/RESPALDO.md](plan/RESPALDO.md), in Spanish.

## Making one

**Settings → Backup**, or the portal's **Backup** page. The file goes to
`/backups/p4os-<date>-<time>.p4bak`.

| | Without a passphrase | With one |
|---|---|---|
| The preferences | all but what looks like a credential | all of them |
| The file | plain: it can be opened and read | sealed: nothing of it reads without the passphrase |
| For | putting a board back as it was, typing the passwords again | moving everything, and anything that leaves the house |

- **What looks like a credential** is decided by the preference's name: a
  password, a token, a key, a code, the lock screen's, the portal's private
  keys, the SSH server's. It is a best effort, and the screens say "what
  looks like": an app can keep a secret under a name this does not see.
- **A sealed backup** is AES-256-GCM under a key derived from the
  passphrase (PBKDF2-HMAC-SHA256, 40 000 rounds, the number is in the
  file). The board asks for the passphrase twice. There is no way around a
  forgotten one.
- **In neither:** what belongs to this very board and moment: the counts of
  failed starts, the crash dump's marks, the Bluetooth pairing (its keys
  are not preferences: pair again), the portal's open sessions.

## Restoring one

Tap a backup in Settings → Backup (twice), or **Restore** in the portal. A
sealed one asks for its passphrase.

1. The backup is read and, when sealed, opened: a wrong passphrase stops
   here, with nothing touched.
2. **What there is now is kept first**, in a backup of its own (`…-antes`,
   of the same kind and under the same passphrase), so a restore can be
   undone by restoring that one.
3. The backup's preferences are written over the ones there are. A
   preference that is not in the backup stays as it is; so a plain backup
   does not touch the passwords the board has.
4. `menu.txt` and `modules.txt` go back to the card.
5. **Restart.** Most preferences are read at startup. Nothing restarts by
   itself.
6. **The apps:** when the backup lists apps the card does not have, the
   screen offers to ask the Store for them, each with the extras it had.

A backup made by a newer P4OS than the one running is refused: update
first.

## Keeping it off the card

A backup on the card survives a bad update and an erased flash, and not a
lost card. The portal's Backup page downloads the file to a computer and
brings one back ("Bring a backup…"); it is also just a file under
`/backups`, for the USB's disk mode or the Files page.

A passphrase typed in the portal travels as the portal's traffic does: in
the clear unless HTTPS is on ([SECURITY.md](SECURITY.md)). It is not kept.

## The file

One JSON object:

```json
{"p4bak": 1, "made": 1795295087, "seq": 0, "fw": "v0.31.0", "board": "p4os",
 "nprefs": 600, "left_out": 12,
 "prefs": [["i", "volume", 40], ["s", "tz", "ART3"]],
 "apps": [{"file": "bastion", "ver": "v0.30.0", "from": "https://…/p4os-store.ndjson", "packs": ["bastion_p4.pak"]}],
 "files": {"menu.txt": "…", "modules.txt": "…"}}
```

A sealed one has `"sealed": {"kdf": "pbkdf2-sha256", "iter": 40000, "salt",
"iv", "tag", "data"}` in the place of `"prefs"`: that same array, as text,
encrypted; the rest stays readable (when it was made, by which P4OS, how
many preferences, which apps).

## From a script

| Call | What it does |
|---|---|
| `GET /api/backup` | whether a job is running, how the last one ended, the backups on the card |
| `POST /api/backup` | `{"pass": "…"}`: makes one now; without `pass`, a plain one |
| `POST /api/backup/restore` | `{"name": "…", "pass": "…"}` |
| `POST /api/backup/delete` | `{"name": "…"}` |
| `GET /api/backup/apps?name=` | the apps that backup lists and the card lacks |
| `POST /api/backup/apps` | `{"name": "…"}`: asks the Store for them |
| `GET /api/fs/get?path=/backups/<name>&attachment=1` | the file itself |

## Tests

`tests/backup_host/run.sh` builds the service for the Mac, with a folder
for a card and a file for the preferences: 20 cases, among them a long
value with its line breaks, a restore onto another board and its undoing,
a wrong passphrase, and a backup of a newer P4OS.
