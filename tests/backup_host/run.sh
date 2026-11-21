#!/bin/bash
# The firmware's backups (components/aos_backup/aos_backup.c) on the Mac: a
# card that is a folder and preferences that are a file. What a plain backup
# leaves out and a sealed one carries, a restore onto another board, the
# backup it makes of what there was, a wrong passphrase, a backup of a newer
# P4OS, the list of apps to ask the Tienda for, the copies it makes by
# itself (a day's, before a firmware, the five it keeps, sealed under a kept
# key), the offer to a new board, a pendrive, and a server by SFTP (a
# make-believe one here: the real client is tried in tests/sshc_host).
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
   -I"$P4/components/aos_hal/include" -I"$P4/components/aos_hal" -I"$P4/components/aos_backup/include" -I"$P4/components/aos_ssh/include" \
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
want = dict(PLAIN, **SECRET, own="stays", boot_bad=9, bk_seen=1)     # bk_seen: this board made a backup, the one of before
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

# ---- the copies it makes by itself (stage 2)
today = int(time.strftime("%Y%m%d"))
def autos(c): return sorted(n for n in os.listdir(c + "/backups") if "-auto" in n or "-fw" in n)
c4 = card("c4")
write_prefs("p4", dict(ALL))
jobs, made, o = run("c4", "p4", "auto")
a1 = autos("c4")
b = json.load(open("c4/backups/" + a1[0])) if a1 else {}
got = {p[1]: p[2] for p in b.get("prefs", [])}
now = read_prefs("p4")
check("the day's copy: plain, without credentials, marked as the day's", jobs[0][0] and "made=1 day=%d" % today in jobs[0][1] and len(a1) == 1
      and a1[0].endswith("-auto.p4bak") and b.get("why") == "daily" and got == PLAIN and now.get("bk_day") == today and now.get("bk_sum"), (o, a1))
jobs, made, o = run("c4", "p4", "auto", "fw")
check("nothing changed: no copy, the day's or the one before a firmware", all(j[0] and "made=0" in j[1] for j in jobs) and autos("c4") == a1, o)
write_prefs("p4", dict(read_prefs("p4"), boot_bad=77, st_today=5))
jobs, made, o = run("c4", "p4", "auto")
check("...nor when only this board's counters did", "made=0" in jobs[0][1] and autos("c4") == a1, o)
write_prefs("p4", dict(read_prefs("p4"), volume=41))
jobs, made, o = run("c4", "p4", "fw")
a2 = [n for n in autos("c4") if n not in a1]
check("a preference changed: the copy before a firmware is made", jobs[0][0] and "made=1" in jobs[0][1] and len(a2) == 1 and "-fw" in a2[0]
      and json.load(open("c4/backups/" + a2[0])).get("why") == "firmware", (o, a2))
write_prefs("p4", dict(read_prefs("p4"), volume=42, bk_auto=0))
jobs, made, o = run("c4", "p4", "auto", "fw", "bg-fw")
check("turned off: none", "made=0" in jobs[0][1] and "made=0" in jobs[1][1] and len(autos("c4")) == 2, o)
write_prefs("p4", dict(read_prefs("p4"), bk_auto=1))
jobs, made, o = run("c4", "p4", "bg-fw")
check("before a firmware, as the update asks for it: made and waited for", jobs[0][0] and "busy=0 job=auto" in jobs[0][1] and len(autos("c4")) == 3, o)

# the card keeps five of them; the ones made by hand are not touched
jobs, made, o = run("c4", "p4", "make")
by_hand = made[0]
for v in range(50, 57):
    write_prefs("p4", dict(read_prefs("p4"), volume=v))
    run("c4", "p4", "auto")
left = autos("c4")
jobs, made, o = run("c4", "p4", "list")
newest = [l.split(" ")[1] for l in o.splitlines() if l.startswith("backup ") and ("why=2" in l or "why=3" in l)]
last = json.load(open("c4/backups/" + newest[0])) if newest else {}
check("ten automatic copies made: the last five are on the card, and the one by hand", len(left) == 5 and by_hand in os.listdir("c4/backups")
      and a1[0] not in left and {p[1]: p[2] for p in last.get("prefs", [])}.get("volume") == 56, (left, o))

# a passphrase for them: sealed from then on, under a key the board keeps
jobs, made, o = run("c4", "p4", "key=la de las copias")
kept_key = read_prefs("p4").get("bk_auto_key", "")
write_prefs("p4", dict(read_prefs("p4"), volume=60))
run("c4", "p4", "auto")
write_prefs("p4", dict(read_prefs("p4"), volume=61))
run("c4", "p4", "auto")
jobs, made, o = run("c4", "p4", "list")
newest = [l.split(" ")[1] for l in o.splitlines() if l.startswith("backup ") and "why=2" in l][:2]
t1, t2 = (open("c4/backups/" + n).read() for n in newest)
s1, s2 = json.loads(t1)["sealed"], json.loads(t2)["sealed"]
check("a passphrase for the automatic copies: sealed, credentials in", "sealed=1" in o and "la de las copias" not in kept_key and len(kept_key) == 64
      and json.loads(t1)["nprefs"] == len(PLAIN) + len(SECRET) + 2 and "secreto123" not in t1 and "casa" not in t1, (o, kept_key))
check("...each with its own IV under the one key", s1["salt"] == s2["salt"] and s1["iv"] != s2["iv"] and s1["data"] != s2["data"], (s1["iv"], s2["iv"]))
c5 = card("c5"); os.makedirs("c5/backups"); shutil.copy("c4/backups/" + newest[0], "c5/backups/")
write_prefs("p5", {"volume": 1})
jobs, made, o = run("c5", "p5", "restore=" + newest[0] + ":otra", "restore=" + newest[0] + ":la de las copias")
now = read_prefs("p5")
check("...and it opens with that passphrase on another board, and with no other", not jobs[0][0] and "kind=5" in jobs[0][1] and jobs[1][0]
      and now.get("volume") == 61 and now.get("wifi_pass") == "secreto123" and now.get("bk_auto_key") == kept_key, (o, now.get("volume")))
jobs, made, o = run("c4", "p4", "key")
check("the passphrase taken off: plain again", "sealed=0" in o and not read_prefs("p4").get("bk_auto_key"), o)

# ---- a new board with a backup on its card
c6 = card("c6"); os.makedirs("c6/backups"); shutil.copy("c1/backups/" + plain, "c6/backups/")
shutil.copy("c4/backups/" + newest[0], "c6/backups/")
open("p6", "w").close()
jobs, made, o = run("c6", "p6", "offer")
check("a board that knows no network, a backup on its card: offered, the newest", ("offer 1 " + newest[0]) in o and read_prefs("p6") == {}, o)
jobs, made, o = run("c6", "p6", "offer-done", "offer")
check("...once: not after the answer", "offer 0" in o, o)
open("p6", "w").close()
jobs, made, o = run("c6", "p6", "offer", env={"FAKE_SSID": "casa"})
jobs2, made2, o2 = run("c6", "p6", "offer")
check("a board with a network of its own: not offered, then or later", "offer 0" in o and "offer 0" in o2, o + o2)
open("p6", "w").close()
c7 = card("c7")
jobs, made, o = run("c7", "p6", "offer", "make", "offer")
check("no backup on the card: nothing to offer; nor the one this board just made", o.count("offer 0") == 2, o)
open("p6", "w").close()
jobs, made, o = run(None, "p6", "offer")
check("no card: no offer", "offer 0" in o, o)

# ---- to a pendrive and back (stage 3)
def tree(root):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            out[os.path.relpath(os.path.join(d, f), root)] = open(os.path.join(d, f), "rb").read()
    return out
c8 = card("c8")
write_prefs("p8", dict(PLAIN))
for d in ("notas/viajes", "redes", "music", "lang/en", "web", "photos", ".oculto"):
    os.makedirs("c8/" + d)
open("c8/notas/a.txt", "w").write("una nota\n")
open("c8/notas/viajes/b.txt", "w").write("otra, más larga\n" * 4000)       # more than one chunk
open("c8/redes/casa.csv", "w").write("ssid;canal\n")
open("c8/music/tema.mp3", "wb").write(b"x" * 5000)
open("c8/lang/en/_sistema.lang", "w").write("a\tb\n")
open("c8/suelto.txt", "w").write("no es una carpeta\n")
jobs, made, o = run("c8", "p8", "folders")
check("the card's data folders: not the ones fetched again, the big ones off", "folders music=0 notas=1 photos=0 redes=1\n" in o, o)
jobs, made, o = run("c8", "p8", "to-drive")
check("no pendrive: said as such", not jobs[0][0] and "kind=9" in jobs[0][1], o)
os.makedirs("u1")
jobs, made, o = run("c8", "p8", "to-drive", env={"P4_USB": "u1"})
t = tree("u1/p4os-backup")
baks = [k for k in t if k.startswith("backups/")]
check("to the pendrive: a backup of now and the chosen folders, whole", jobs[0][0] and len(baks) == 1 and "-auto" in baks[0]
      and {k for k in t if k.startswith("card/")} == {"card/notas/a.txt", "card/notas/viajes/b.txt", "card/redes/casa.csv"}
      and t["card/notas/viajes/b.txt"] == open("c8/notas/viajes/b.txt", "rb").read() and "4 files copied" in jobs[0][1], (o, sorted(t)))
jobs, made, o = run("c8", "p8", "to-drive", env={"P4_USB": "u1"})
check("again: nothing changed, nothing copied", jobs[0][0] and "0 files copied" in jobs[0][1] and "4 were there already" in jobs[0][1] and tree("u1/p4os-backup") == t, o)
time.sleep(1.1)
open("c8/notas/a.txt", "w").write("una nota, corregida\n")
open("c8/notas/c.txt", "w").write("nueva\n")
run("c8", "p8", "folder=music:1", "folder=redes:0")
os.remove("u1/p4os-backup/card/redes/casa.csv")
jobs, made, o = run("c8", "p8", "folders", "to-drive", env={"P4_USB": "u1"})
t = tree("u1/p4os-backup")
check("a file changed, one new, another folder chosen and one left: only those travel", "folders music=1 notas=1 photos=0 redes=0\n" in o
      and t["card/notas/a.txt"] == b"una nota, corregida\n" and "card/notas/c.txt" in t and "card/music/tema.mp3" in t and "card/redes/casa.csv" not in t
      and "4 files copied" in jobs[0][1] and not [k for k in t if k.endswith(".p4t")], (o, sorted(t)))      # the three, and a backup: the choice is a preference
baks = sorted(k for k in t if k.startswith("backups/"))[-1:]

# a new card and a new board: back from the pendrive, then a restore
c9 = card("c9"); open("p9", "w").close()
os.makedirs("u1/p4os-backup/card/apps"); open("u1/p4os-backup/card/apps/malo.so", "w").write("no")
jobs, made, o = run("c9", "p9", "from-drive", "offer", env={"P4_USB": "u1"})
t9 = tree("c9")
check("back to a new card: the backups and the folders, and nothing into /apps", jobs[0][0] and t9.get("notas/a.txt") == b"una nota, corregida\n"
      and "notas/viajes/b.txt" in t9 and "music/tema.mp3" in t9 and "apps/malo.so" not in t9 and ("backups/" + baks[0].split("/")[1]) in t9, (o, sorted(t9)))
check("...and the new board is offered the backup that came", ("offer 1 " + baks[0].split("/")[1]) in o, o)
jobs, made, o = run("c9", "p9", "restore=" + baks[0].split("/")[1])
check("...which restores: the board as it was", jobs[0][0] and read_prefs("p9").get("tz") == "ART3", o)
open("c9/notas/a.txt", "w").write("escrita en la tarjeta nueva\n")
time.sleep(1.1); os.utime("c9/notas/a.txt")
jobs, made, o = run("c9", "p9", "from-drive", env={"P4_USB": "u1"})
check("a file the card has newer is not replaced by the pendrive's older one", jobs[0][0] and "0 files copied" in jobs[0][1]
      and open("c9/notas/a.txt").read() == "escrita en la tarjeta nueva\n", o)
os.makedirs("u2")
jobs, made, o = run("c9", "p9", "from-drive", env={"P4_USB": "u2"})
check("a pendrive with no copy of a board: said, nothing touched", not jobs[0][0] and "kind=3" in jobs[0][1], o)

# ---- to a server of the person's own (stage 4): what goes, and when it does not
ca = card("ca")
write_prefs("pa", dict(ALL))
os.makedirs("srv")
E = {"P4_SRV": "srv", "P4_SRV_KEY": "bGEgY2xhdmU="}
jobs, made, o = run("ca", "pa", "make", "make=una frase", "send", env=E)
check("no server set: nothing goes", not jobs[2][0] and "kind=10" in jobs[2][1] and os.listdir("srv") == [], o)
jobs, made, o = run("ca", "pa", "srv=nas.example:2222:respaldos:copias/p4", "srv-key", "srv-key", "send", env=E)
lines = [l for l in o.splitlines() if l.startswith("keyline 1 ssh-ed25519 ")]
check("the server set; the board's key, the same line each time", "server nas.example:2222 respaldos copias/p4 on=0 pinned=0" in o
      and len(lines) == 2 and lines[0] == lines[1] and "p4os@placa" in lines[0] and read_prefs("pa").get("sshc_seed"), o)
check("...its key not accepted yet: nothing goes", not jobs[0][0] and "kind=10" in jobs[0][1] and os.listdir("srv") == [], o)
jobs, made, o = run("ca", "pa", "look", env=dict(E, P4_SRV_DENY="1"))
check("a look: the server's key shown, though it does not let the board in yet", not jobs[0][0] and "kind=13" in jobs[0][1] and "seen=1 in=0 fp=ED25519 SHA256:bGEgY2xhdmU=" in o, o)
jobs, made, o = run("ca", "pa", "trust", "look", "trust", "send", env=E)
check("accepted at the board after a look that got in", "trust 0 pinned=0" in o and "seen=1 in=1" in o and "trust 1 pinned=1 fp=ED25519 SHA256:bGEgY2xhdmU=" in o, o)
check("...but no passphrase for the copies: nothing sealed to send, nothing goes", not jobs[1][0] and "kind=10" in jobs[1][1] and os.listdir("srv") == [], o)
jobs, made, o = run("ca", "pa", "key=la de las copias", "send", env=E)
up = sorted(os.listdir("srv/copias/p4/placa_de_prueba")) if os.path.isdir("srv/copias/p4/placa_de_prueba") else []
on_card = sorted(os.listdir("ca/backups"))
sealed_on_card = sorted(n for n in on_card if "sealed" in json.load(open("ca/backups/" + n)))
check("with a passphrase: the sealed ones go, one of now among them, and no plain one", jobs[1][0] and "last=1" in jobs[1][1] and up == sealed_on_card
      and len(up) == 2 and any("-auto" in n for n in up) and len(on_card) == 3
      and all(open("srv/copias/p4/placa_de_prueba/" + n, "rb").read() == open("ca/backups/" + n, "rb").read() for n in up)
      and "secreto123" not in "".join(open("srv/copias/p4/placa_de_prueba/" + n).read() for n in up), (o, up, on_card))
jobs, made, o = run("ca", "pa", "send", env=E)
check("again: they are there already", jobs[0][0] and "0 sealed backups sent" in jobs[0][1] and "2 were there already" in jobs[0][1], o)
jobs, made, o = run("ca", "pa", "send", env=dict(E, P4_SRV_KEY="b3RyYSBjbGF2ZQ=="))
check("another server where that one was: refused by its key, nothing sent", not jobs[0][0] and "kind=12" in jobs[0][1]
      and sorted(os.listdir("srv/copias/p4/placa_de_prueba")) == up, o)
jobs, made, o = run("ca", "pa", "send", env={"P4_SRV_KEY": "bGEgY2xhdmU="})
check("the server is not there: said as such", not jobs[0][0] and "kind=11" in jobs[0][1], o)
jobs, made, o = run("ca", "pa", "srv=nas.example:22:respaldos:copias/p4", "send", env=E)
check("its address changed: the key accepted is forgotten, and nothing goes until another look", "pinned=0" in o and not jobs[0][0] and "kind=10" in jobs[0][1], o)
old_seed = read_prefs("pa").get("sshc_seed")
jobs, made, o = run("ca", "pa", "srv-key", "srv-key-new", "srv-key", "srv-key", env=E)
lines = [l for l in o.splitlines() if l.startswith("keyline 1 ")]
check("a new key for the board: another line, kept, and the old one gone", "keynew 1" in o and len(lines) == 3 and lines[0] != lines[1]
      and lines[1] == lines[2] and read_prefs("pa").get("sshc_seed") not in ("", None, old_seed), o)
b = json.load(open("ca/backups/" + [n for n in on_card if n not in sealed_on_card][0]))
keys = {p[1] for p in b["prefs"]}
check("a plain backup carries where the server is, not the board's key", "bk_srv_host" not in keys or "sshc_seed" not in keys, sorted(k for k in keys if k.startswith(("bk_", "ssh"))))
sys.exit(1 if fails else 0)
PY
echo "all passed"
