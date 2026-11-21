#!/bin/bash
# The firmware's backups (components/aos_backup/aos_backup.c) on the Mac: a
# card that is a folder and preferences that are a file. What a plain backup
# leaves out and a sealed one carries, a restore onto another board, the
# backup it makes of what there was, a wrong passphrase, a backup of a newer
# P4OS, and the list of apps to ask the Tienda for.
#
#   tests/backup_host/run.sh        (brew: mbedtls; cJSON from ESP-IDF)
set -e
cd "$(dirname "$0")"
P4=$(cd ../.. && pwd)
MB=${MBEDTLS:-/opt/homebrew/opt/mbedtls}
CJ=${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON
out=$(mktemp -d)
trap 'rc=$?; rm -rf "$out"; exit $rc' EXIT

cc -O1 -g -Wall -Wextra -Werror -DAOS_SIM \
   -I"$P4/components/aos_hal/include" -I"$P4/components/aos_hal" -I"$P4/components/aos_backup/include" \
   -I"$CJ" -I"$MB/include" -o "$out/t" main.c fakes.c \
   "$P4/components/aos_backup/aos_backup.c" "$P4/components/aos_hal/aos_crypto.c" "$CJ/cJSON.c" \
   -L"$MB/lib" -lmbedcrypto -lpthread

cd "$out"
python3 - <<'PY'
import json, os, shutil, subprocess, sys, time

fails = 0
def check(what, ok, detail=""):
    global fails
    print("%-66s %s" % (what, "ok" if ok else "FAILED"))
    if not ok:
        fails += 1
        print("     " + str(detail)[-900:])

def run(card, prefs, *steps, env=None):
    e = dict(os.environ, P4_PREFS=prefs)
    if card: e["P4_CARD"] = card
    e.update(env or {})
    p = subprocess.run(["./t"] + list(steps), capture_output=True, text=True, env=e, timeout=120)
    jobs = [(l.startswith("ok"), l) for l in p.stdout.splitlines() if l.startswith(("ok ", "FAILED "))]
    made = [l.split(" ", 1)[1] for l in p.stdout.splitlines() if l.startswith("made ")]
    return jobs, made, p.stdout + p.stderr

def write_prefs(path, items):
    with open(path, "w") as f:
        for k, v in items.items():
            f.write("%s %s %s\n" % ("i" if isinstance(v, int) else "s", k, str(v).replace("\n", "\x01")))

def read_prefs(path):
    out = {}
    for l in open(path).read().splitlines():
        t, k, v = l.split(" ", 2)
        out[k] = int(v) if t == "i" else v.replace("\x01", "\n")
    return out

PEM = "-----BEGIN CERTIFICATE-----\n" + "\n".join("MIIB" + "Ab3" * 20 for _ in range(22)) + "\n-----END CERTIFICATE-----\n"
SECRET = {"wifi_pass": "secreto123", "ha_token": "t" * 183, "pt_key": PEM.replace("CERTIFICATE", "PRIVATE KEY"), "lock_pin": "1234",
          "mqtt_pass": "x", "claude_at": "tok", "api_key": "k", "mb_key": "m", "ssh_hk": "host key",
          "sshc_seed": "c2VtaWxsYQ==", "sec_mine": "an app's own", "x_privkey": "p"}
OWN = {"boot_bad": 3, "time_ok": 1, "bt_bond": 1, "pt_sess": "s", "lock_fails": 2}
PLAIN = {"volume": 40, "tz": "ART3", "wifi_ssid": "casa", "pt_crt": PEM, "lock_on": 1, "store_srcs": "https://a.example/c\nhttps://b.example/c",
         "key_snd": 1, "hotkeys": "x", "ssh_k0": "ssh-ed25519 AAAAC3Nz persona", "px_seed": 7, "second": 2, "fs_fav": "/a,/b", "ee.chip": "24C32", "neg": -7}
ALL = dict(PLAIN, **SECRET, **OWN)

def card(name):
    os.makedirs(name + "/apps/.store")
    return name
c1 = card("c1")
write_prefs("p1", ALL)
open("c1/menu.txt", "w").write("Juegos: bastion gemas\n")
open("c1/modules.txt", "w").write("i2c.ext bme280\n")
for file, packs, extra in (("bastion", ["bastion_p4.pak", "bastion_selva_p4.pak"], {}), ("gemas", [], {}), ("sys-emoji", [], {"sys": True})):
    r = dict({"schema": 1, "file": file, "ver": "v0.30.0", "source": {"name": "P4OS", "catalog": "https://x/c.ndjson"},
              "packs": [{"file": p, "parts": []} for p in packs]}, **extra)
    json.dump(r, open("c1/apps/.store/%s.json" % file, "w"))

# ---- a plain one
jobs, made, o = run("c1", "p1", "make")
plain = made[0] if made else ""
b = json.load(open("c1/backups/" + plain)) if plain else {}
got = {p[1]: p[2] for p in b.get("prefs", [])}
check("a plain backup: the preferences, without what looks like a credential", jobs and jobs[0][0] and got == PLAIN
      and b["left_out"] == len(SECRET) and b["nprefs"] == len(PLAIN) and "sealed" not in b, (sorted(set(got) ^ set(PLAIN)), o))
check("...a long value with its line breaks, whole", got.get("pt_crt") == PEM and got.get("store_srcs") == PLAIN["store_srcs"] and got.get("neg") == -7, got.get("pt_crt"))
check("...nothing of this board and this moment", not set(got) & set(OWN), set(got) & set(OWN))
check("...the apps to ask for again, with their extras, and the two files", [(a["file"], a["packs"]) for a in sorted(b["apps"], key=lambda a: a["file"])]
      == [("bastion", ["bastion_p4.pak", "bastion_selva_p4.pak"]), ("gemas", []), ("sys-emoji", [])]
      and b["files"] == {"menu.txt": "Juegos: bastion gemas\n", "modules.txt": "i2c.ext bme280\n"} and b["fw"] == "v0.30.0", b.get("apps"))

# ---- a sealed one
time.sleep(1.1)
jobs, made, o = run("c1", "p1", "make=tres palabras")
sealed = made[0] if made else ""
text = open("c1/backups/" + sealed).read() if sealed else ""
b = json.loads(text) if text else {}
check("a sealed backup: everything in it, and nothing of it readable", jobs and jobs[0][0] and "prefs" not in b
      and b["nprefs"] == len(PLAIN) + len(SECRET) and b["left_out"] == 0 and "secreto123" not in text and "casa" not in text
      and "ART3" not in text and b["sealed"]["kdf"] == "pbkdf2-sha256" and b["sealed"]["iter"] >= 10000, text[:300])
jobs2, made2, o2 = run("c1", "p1", "make=tres palabras")
b2 = json.load(open("c1/backups/" + made2[0]))
check("...sealed again, another salt and other bytes", b2["sealed"]["salt"] != b["sealed"]["salt"] and b2["sealed"]["data"] != b["sealed"]["data"], o2)

# ---- onto another board
c2 = card("c2"); os.makedirs("c2/backups")
shutil.copy("c1/backups/" + sealed, "c2/backups/")
shutil.copy("c1/backups/" + plain, "c2/backups/")
BEFORE = {"volume": 10, "wifi_pass": "la de antes", "own": "stays", "boot_bad": 9}
write_prefs("p2", BEFORE)
open("c2/menu.txt", "w").write("otro orden\n")
jobs, made, o = run("c2", "p2", "restore=" + sealed + ":otra frase", "restore=" + sealed)
check("a wrong passphrase, or none: refused, and nothing touched", not jobs[0][0] and "kind=5" in jobs[0][1] and not jobs[1][0]
      and "kind=5" in jobs[1][1] and read_prefs("p2") == BEFORE and sorted(os.listdir("c2/backups")) == sorted([plain, sealed])
      and open("c2/menu.txt").read() == "otro orden\n", (o, os.listdir("c2/backups")))
jobs, made, o = run("c2", "p2", "restore=" + sealed + ":tres palabras")
now = read_prefs("p2")
want = dict(PLAIN, **SECRET, own="stays", boot_bad=9)
check("restored with its passphrase: every preference, the credentials too", jobs[0][0] and "written=%d" % (len(PLAIN) + len(SECRET)) in jobs[0][1]
      and now == want, (sorted(set(now) ^ set(want)), [k for k in want if now.get(k) != want[k]], o))
check("...what is not in the backup stays, what is this board's is not written", now.get("own") == "stays" and now.get("boot_bad") == 9 and "time_ok" not in now, now)
check("...and the card's two files", open("c2/menu.txt").read() == "Juegos: bastion gemas\n" and open("c2/modules.txt").read() == "i2c.ext bme280\n")
kept = [n for n in os.listdir("c2/backups") if "-antes" in n]
kb = json.load(open("c2/backups/" + kept[0])) if kept else {}
check("what there was is kept first, sealed under the same passphrase", len(kept) == 1 and kb.get("before") is True and "sealed" in kb
      and "la de antes" not in open("c2/backups/" + kept[0]).read() and kb["files"]["menu.txt"] == "otro orden\n", kept)
jobs, made, o = run("c2", "p2", "restore=" + kept[0] + ":tres palabras")
back = read_prefs("p2")
check("...and restoring that one undoes it", jobs[0][0] and back["volume"] == 10 and back["wifi_pass"] == "la de antes"
      and open("c2/menu.txt").read() == "otro orden\n", back)

# ---- a plain one onto a board with credentials of its own
write_prefs("p3", {"volume": 1, "wifi_pass": "queda", "lock_pin": "9999"})
c3 = card("c3"); os.makedirs("c3/backups"); shutil.copy("c1/backups/" + plain, "c3/backups/")
jobs, made, o = run("c3", "p3", "restore=" + plain)
now = read_prefs("p3")
check("a plain backup restored: the credentials there were stay", jobs[0][0] and now["wifi_pass"] == "queda" and now["lock_pin"] == "9999"
      and now["volume"] == 40 and now["wifi_ssid"] == "casa", now)

# ---- what is refused
jobs, made, o = run("c3", "p3", "restore=" + plain, env={"FAKE_FW": "v0.29.0-3-gabc"})
check("a backup of a newer P4OS: refused, nothing written", not jobs[0][0] and "kind=6" in jobs[0][1] and "written=0" in jobs[0][1], o)
open("c3/backups/roto.p4bak", "w").write("{\"p4bak\": 1, \"prefs\": \"no\"}")
open("c3/backups/otro.p4bak", "w").write("not json at all")
jobs, made, o = run("c3", "p3", "restore=roto.p4bak", "restore=otro.p4bak", "restore=../p1", "restore=nada.p4bak", "list")
check("not a backup, a damaged one, a name out of its folder, one that is not there", [("kind=%d" % k) in j[1] and not j[0] for k, j in zip((4, 4, 3, 3), jobs)]
      == [True] * 4 and not [l for l in o.splitlines() if l.startswith(("backup roto", "backup otro"))], o)
jobs, made, o = run(None, "p3", "make")
check("no card: said as such", not jobs[0][0] and "kind=2" in jobs[0][1], o)

# ---- the list, and the apps to ask for
jobs, made, o = run("c1", "p1", "list")
names = [l.split(" ")[1] for l in o.splitlines() if l.startswith("backup ")]
check("the list, the newest first", len(names) == 3 and names[-1] == plain and names[0] == made2[0]
      and ("backup %s " % plain) in o and "sealed=0 before=0 prefs=%d left_out=%d apps=3" % (len(PLAIN), len(SECRET)) in o, o)
jobs, made, o = run("c2", "p2", "missing=" + plain)
check("the apps a backup lists and the card lacks, with their extras", "missing bastion [bastion_p4.pak,bastion_selva_p4.pak]" in o
      and "missing gemas []" in o and "missing sys-emoji []" in o, o)
json.dump({"file": "gemas"}, open("c2/apps/.store/gemas.json", "w"))
jobs, made, o = run("c2", "p2", "missing=" + plain)
check("...not the ones that are there", "missing gemas" not in o and "missing bastion" in o, o)

# ---- through the module's thread
jobs, made, o = run("c3", "p3", "bg-make=una frase")
check("in the background: one at a time, and its result", jobs[0][0] and "started=1 twice=0" in jobs[0][1] and made
      and "sealed" in json.load(open("c3/backups/" + made[0])), o)
sys.exit(1 if fails else 0)
PY
echo "all passed"
