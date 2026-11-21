#!/bin/bash
# backup.sh [host] [folder]: a backup of the board, kept on this computer.
#
#   tools/backup.sh                         a new backup of p4os.local, into ./p4os-backups
#   tools/backup.sh 10.0.0.23 ~/Backups     by address, into that folder
#   SEALED=1 tools/backup.sh                sealed: asks for the passphrase here, twice
#   FOLDERS="notas redes" tools/backup.sh   and those folders of the card, file by file
#   KEEP=1 tools/backup.sh                  leaves the new backup on the card too
#
# It asks the board for a backup (docs/BACKUP.md: its preferences, the list
# of apps and how the card is arranged), downloads the file and takes it off
# the card again, so a run a day from cron does not fill /backups. Without
# SEALED the file is plain and carries nothing that looks like a credential.
#
# The folders are what a backup does not carry: what the apps wrote on the
# card (notes, saved networks, scripts). They go to <folder>/card/<name>, and
# a file that is already there with the same size and date is not fetched
# again. Not the big ones by default: /photos, /music, /videos and /maps come
# from a computer in the first place.
#
# A passphrase typed here travels as the portal's traffic does: in the clear
# unless the portal is on HTTPS (docs/SECURITY.md). Use https://<host> as the
# host then: HOST=https://p4os.local tools/backup.sh.
# The portal's token, for a board that asks for its password here:
# P4OS_TOKEN=<token> tools/backup.sh
set -e
[ -n "$P4OS_TOKEN" ] && curl() { command curl -H "Authorization: Bearer $P4OS_TOKEN" "$@"; }

HOST=${1:-${HOST:-p4os.local}}
OUT=${2:-p4os-backups}
case "$HOST" in http://*|https://*) B=$HOST ;; *) B="http://$HOST" ;; esac
mkdir -p "$OUT"

pass=""
if [ -n "$SEALED" ]; then
    read -r -s -p "A passphrase for this backup: " pass; echo
    read -r -s -p "The same, again: " again; echo
    [ "$pass" = "$again" ] || { echo "not the same passphrase: nothing made"; exit 1; }
    [ ${#pass} -ge 6 ] || { echo "six characters or more"; exit 1; }
fi

# the passphrase goes in the request's body, from a pipe: not on a command
# line, where any user of this computer would read it
state=$(PASS="$pass" python3 -c 'import json, os; print(json.dumps({"pass": os.environ["PASS"]}))' |
        curl -4 -sk --max-time 20 -X POST --data-binary @- "$B/api/backup")
echo "$state" | grep -q '"ok":true' || { echo "the board did not take it: $state"; exit 1; }
for i in $(seq 1 60); do
    state=$(curl -4 -sk --max-time 10 "$B/api/backup")
    echo "$state" | grep -q '"busy":false' && break
    sleep 0.5
done
name=$(echo "$state" | python3 -c '
import json, sys
s = json.load(sys.stdin)
print(s["last_name"] if s.get("last_ok") and s.get("last_job") == "make" else "")
if not s.get("last_ok"): sys.stderr.write("the backup failed: %s\n" % s.get("last", "?"))')
[ -n "$name" ] || exit 1

curl -4 -sk --max-time 60 -o "$OUT/$name.part" "$B/api/fs/get?path=/backups/$name"
# what came is a backup, whole: the board's own file starts with its mark
python3 -c '
import json, sys
b = json.load(open(sys.argv[1]))
assert b.get("p4bak") == 1 and ("prefs" in b or "sealed" in b)
print("%s: %d preferences%s, %d apps, P4OS %s" % (sys.argv[2], b["nprefs"], ", sealed" if "sealed" in b else
      " (%d that look like credentials left out)" % b["left_out"], len(b.get("apps", [])), b.get("fw", "?")))' \
    "$OUT/$name.part" "$OUT/$name" || { rm -f "$OUT/$name.part"; echo "what the board sent is not a backup: nothing kept"; exit 1; }
mv "$OUT/$name.part" "$OUT/$name"
chmod 600 "$OUT/$name"

if [ -z "$KEEP" ]; then
    curl -4 -sk --max-time 10 -X POST -d "{\"name\":\"$name\"}" "$B/api/backup/delete" >/dev/null || true
fi

# ---- the folders of the card, file by file
[ -z "$FOLDERS" ] && exit 0
B="$B" OUT="$OUT" TOKEN="$P4OS_TOKEN" python3 - $FOLDERS <<'PY'
import json, os, ssl, sys, urllib.parse, urllib.request

B, OUT, TOKEN = os.environ["B"], os.environ["OUT"], os.environ.get("TOKEN", "")
ctx = ssl._create_unverified_context()          # the board's certificate is its own (docs/SECURITY.md)

def get(path, **kw):
    r = urllib.request.Request(B + path, headers={"Authorization": "Bearer " + TOKEN} if TOKEN else {})
    return urllib.request.urlopen(r, timeout=kw.get("timeout", 30), context=ctx)

def walk(folder, n):
    try:
        d = json.load(get("/api/fs?path=" + urllib.parse.quote("/" + folder)))
    except Exception as e:
        print("  /%s: %s" % (folder, e))
        return n
    for e in d.get("entries", []):
        # a name is a name: nothing that would land outside the folder
        if e["name"] in (".", "..") or "/" in e["name"] or "\\" in e["name"]:
            continue
        rel = folder + "/" + e["name"]
        if e["dir"]:
            n = walk(rel, n)
            continue
        dest = os.path.join(OUT, "card", *rel.split("/"))
        if os.path.exists(dest) and os.path.getsize(dest) == e["size"] and int(os.path.getmtime(dest)) == e["mtime"]:
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with get("/api/fs/get?path=" + urllib.parse.quote("/" + rel), timeout=300) as r, open(dest + ".part", "wb") as f:
            while True:
                chunk = r.read(65536)
                if not chunk:
                    break
                f.write(chunk)
        if os.path.getsize(dest + ".part") != e["size"]:
            os.remove(dest + ".part")
            print("  /%s came short: not kept" % rel)
            continue
        os.replace(dest + ".part", dest)
        os.utime(dest, (e["mtime"], e["mtime"]))
        n[0] += 1
        n[1] += e["size"]
    return n

for folder in sys.argv[1:]:
    folder = folder.strip("/")
    if not folder or ".." in folder.split("/"):
        continue
    n = walk(folder, [0, 0])
    print("/%s: %d files fetched, %d bytes" % (folder, n[0], n[1]))
PY
