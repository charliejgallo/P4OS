#!/bin/bash
# The Tienda end to end on the Mac: tools/gen_store.py makes the packages and
# the catalogue of a few real apps, tools/fake_github.py serves them as a
# release, and tools/store_cli.py - the reference for what the board's store
# does - installs them on an empty card, updates one, adopts what was copied
# by hand, removes, and refuses what it must: a package that is not the
# catalogue's, one that writes outside its place or over another's files, a
# compressed one, one for a newer firmware.
#
#   tests/store_host/run.sh            needs the apps built: tools/build_apps.sh
#   BUILT=<a tree with apps/*/build> tests/store_host/run.sh
#
# The apps it takes: hello_app (a page and its languages), dados (nothing but
# its .so), emulador (modules in a folder of its own), golf (a pack), pinball
# (a pack it needs and five extras), mila (a pack in three parts).
set -e
cd "$(dirname "$0")"
P4=$(cd ../.. && pwd)
BUILT=${BUILT:-$P4}
APPS="hello_app dados emulador golf pinball mila"
for a in $APPS; do
    ls "$BUILT"/apps/$a/build/*.so >/dev/null 2>&1 || { echo "apps/$a is not built in $BUILT: tools/build_apps.sh $a (or BUILT=...)"; exit 1; }
done
out=$(mktemp -d)
PORT=${PORT:-18490}
BASE="http://localhost:$PORT/fake/p4os/releases/download/"
dirs=""
for a in golf pinball mila; do dirs="$dirs --packs-dir $BUILT/apps/$a/build"; done
ONLY=$(echo $APPS | tr ' ' ',')

gen() {   # gen <release> <out> [--previous x]
    rel=$1; dst=$2; shift 2
    python3 "$P4/tools/gen_store.py" build --release $rel --out "$dst" --built "$BUILT" --only $ONLY --base "$BASE" $dirs "$@"
}
gen v9.0.0 "$out/r1" > "$out/gen1.log"
gen v9.0.0 "$out/r1b" > /dev/null
diff -rq "$out/r1" "$out/r1b" > /dev/null && echo "the same packages from the same files                     ok" || { echo "two runs differ"; exit 1; }

# what the release serves: the packages, the catalogue and the packs
mkdir "$out/rel"
cp "$out"/r1/* "$out/rel/"
for a in golf pinball mila; do for f in "$BUILT"/apps/$a/build/*_p4.pak*; do ln -s "$f" "$out/rel/$(basename "$f")"; done; done
python3 "$P4/tools/fake_github.py" --dir "$out/rel" --port $PORT --quiet > "$out/fake.log" 2>&1 &
FAKE=$!
trap 'rc=$?; kill $FAKE 2>/dev/null || true; wait $FAKE 2>/dev/null || true; rm -rf "$out"; exit $rc' EXIT
for i in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/_fake" && break; sleep 0.1; done

cd "$out"
P4="$P4" BUILT="$BUILT" PORT=$PORT python3 - <<'PY'
import glob, hashlib, io, json, os, shutil, subprocess, sys, zipfile
P4, BUILT, PORT = os.environ["P4"], os.environ["BUILT"], os.environ["PORT"]
CAT = "http://localhost:%s/fake/p4os/releases/latest/download/p4os-store.ndjson" % PORT
fails = 0

def cli(*args, card="card", cat=CAT):
    p = subprocess.run([sys.executable, P4 + "/tools/store_cli.py", args[0], "--card", card, "--catalog", cat] + list(args[1:]),
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr

def check(what, ok, detail=""):
    global fails
    print("%-58s %s" % (what, "ok" if ok else "FAILED"))
    if not ok:
        fails += 1
        print("    " + str(detail).replace("\n", "\n    ")[:1500])

def tree(card="card"):
    return sorted(os.path.relpath(os.path.join(d, f), card) for d, _, fs in os.walk(card) for f in fs)

def same(a, b):
    return open(a, "rb").read() == open(b, "rb").read()

def lines(path):
    L = [json.loads(l) for l in open(path, encoding="utf-8")]
    return L[0], {e["file"]: e for e in L[1:]}

os.mkdir("card")
# 1. an empty card, everything, with the packs each app needs
rc, o = cli("install", "--all")
t = tree()
want_so = ["apps/%s.so" % a for a in ("hello_app", "dados", "emulador", "golf", "pinball", "mila")]
check("an empty card: every app and what it needs", rc == 0 and all(x in t for x in want_so)
      and "apps/emulador/nes.so" in t and "web/hello.js" in t and "lang/en/demo.hello.lang" in t
      and "apps/golf_p4.pak" in t and "apps/mila_p4.pak.2" in t and "apps/pinball_zombis_p4.pak" in t
      and "apps/pinball_robots_p4.pak" not in t and not [x for x in t if x.endswith(".part")], o)
check("the files are the built ones, byte for byte",
      all(same("card/apps/%s.so" % a, glob.glob("%s/apps/%s/build/*.so" % (BUILT, a))[0]) for a in ("dados", "emulador", "mila"))
      and same("card/apps/mila_p4.pak.1", BUILT + "/apps/mila/build/mila_p4.pak.1")
      and same("card/apps/emulador/snes.so", BUILT + "/apps/emulador/build/modules/snes.so")
      and same("card/web/hello.js", P4 + "/apps/hello_app/web/hello.js"))
r = json.load(open("card/apps/.store/mila.json"))
check("a receipt: source, version, files and packs with hashes", r["ver"] == "v9.0.0" and r["source"]["name"] == "P4OS"
      and r["files"][0]["path"] == "apps/mila.so" and len(r["packs"][0]["parts"]) == 3
      and r["packs"][0]["parts"][2]["name"] == "mila_p4.pak.2", r)

# 2. status, and installing again downloads no pack twice
rc, o = cli("status")
check("status: all up to date", rc == 0 and o.count("up to date") == 6 and "by hand" not in o, o)
import urllib.request
served = lambda: json.loads(urllib.request.urlopen("http://localhost:%s/_fake" % PORT).read())["bytes"]
b = served()
rc, o = cli("install", "mila")
check("again: the packs on the card are not downloaded", rc == 0 and served() - b < 400_000, served() - b)

# 3. an extra, by name; it survives an update of the app
rc, o = cli("install", "pinball", "--packs", "pinball_circo_p4.pak")
t = tree()
check("an extra asked for by name", rc == 0 and "apps/pinball_circo_p4.pak" in t and "apps/pinball_robots_p4.pak" not in t, o)

# 4. a new release: one app changed, the rest stay where they were
head, apps = lines("r1/p4os-store.ndjson")
os.makedirs("built2/apps")
for a in ("hello_app", "dados", "emulador", "golf", "pinball", "mila"):
    os.symlink("%s/apps/%s" % (BUILT, a), "built2/apps/" + a) if a not in ("pinball", "dados") else None
for a in ("pinball", "dados"):
    os.makedirs("built2/apps/%s/build" % a)
    so = glob.glob("%s/apps/%s/build/*.so" % (BUILT, a))[0]
    data = open(so, "rb").read()
    open("built2/apps/%s/build/%s" % (a, os.path.basename(so)), "wb").write(data + b"\0changed\0" if a == "pinball" else data)
dirs = sum((["--packs-dir", "%s/apps/%s/build" % (BUILT, a)] for a in ("golf", "pinball", "mila")), [])
p = subprocess.run([sys.executable, P4 + "/tools/gen_store.py", "build", "--release", "v9.1.0", "--out", "r2",
                    "--built", os.path.abspath("built2"), "--only", "hello_app,dados,emulador,golf,pinball,mila",
                    "--base", head["base"], "--previous", "r1/p4os-store.ndjson"] + dirs, capture_output=True, text=True)
head2, apps2 = lines("r2/p4os-store.ndjson")
check("next release: one package written, five kept", p.returncode == 0 and os.listdir("r2").count("pinball.p4app") == 1
      and len([f for f in os.listdir("r2") if f.endswith(".p4app")]) == 1 and apps2["pinball"]["ver"] == "v9.1.0"
      and apps2["mila"]["ver"] == "v9.0.0" and apps2["mila"]["pkg"]["url"] == "v9.0.0/mila.p4app", p.stdout + p.stderr)
for f in os.listdir("r2"):
    shutil.copy("r2/" + f, "rel/" + f)
rc, o = cli("status")
check("status: one update", rc == 0 and o.count("update: v9.0.0 -> v9.1.0") == 1 and o.count("up to date") == 5, o)
rc, o = cli("install", "pinball")
r = json.load(open("card/apps/.store/pinball.json"))
check("the update: new code, the extra kept", rc == 0 and r["ver"] == "v9.1.0" and open("card/apps/pinball.so", "rb").read().endswith(b"\0changed\0")
      and "pinball_circo_p4.pak" in [p["file"] for p in r["packs"]] and os.path.exists("card/apps/pinball_circo_p4.pak"), o)

# 5. copied by hand, then adopted
os.makedirs("card2/apps")
shutil.copy(BUILT + "/apps/golf/build/golf.so", "card2/apps/golf.so")
shutil.copy(BUILT + "/apps/golf/build/golf_p4.pak", "card2/apps/golf_p4.pak")
open("card2/apps/dados.so", "wb").write(b"an older build")
rc, o = cli("status", card="card2")
check("by hand: status says so", "copied by hand" in o and "golf" in o, o)
rc, o = cli("adopt", card="card2")
r = json.load(open("card2/apps/.store/golf.json")) if os.path.exists("card2/apps/.store/golf.json") else {}
check("adopt: the catalogue's file gets a receipt, with its pack", rc == 0 and r.get("adopted") and r["packs"]
      and os.path.exists("card2/lang/en/demo.golf.lang") and not os.path.exists("card2/apps/.store/dados.json")
      and open("card2/apps/dados.so", "rb").read() == b"an older build" and "left alone: dados" in o, o)

# 6. remove: its files and packs go, the rest stays
rc, o = cli("remove", "mila")
t = tree()
check("remove: files, packs and receipt", rc == 0 and not [x for x in t if "mila" in x] and "apps/golf_p4.pak" in t, t)
rc, o = cli("remove", "dados", card="card2")
check("remove refuses what it did not install", rc != 0 and os.path.exists("card2/apps/dados.so"), o)

# ---- what must be refused. Each: a catalogue of its own, a card of its own.
def bad_case(what, mutate_pkg=None, mutate_entry=None, expect="", fw=None, pre=None):
    card = "bad_" + str(abs(hash(what)))
    os.makedirs(card)
    if pre:
        pre(card)
    e = json.loads(json.dumps(apps2["hello_app"]))
    data = open("rel/hello_app.p4app", "rb").read()
    if mutate_pkg:
        data = mutate_pkg(data)
        name = "bad_%d.p4app" % (abs(hash(what)) % 100000)
        open("rel/" + name, "wb").write(data)
        e["pkg"].update(url="v9.1.0/" + name, size=len(data), sha256=hashlib.sha256(data).hexdigest())
    if mutate_entry:
        mutate_entry(e)
    cat = card + ".ndjson"
    with open(cat, "w", encoding="utf-8") as f:
        f.write(json.dumps(head2) + "\n" + json.dumps(e) + "\n")
    before = tree(card)
    args = ["install", "hello_app"] + (["--fw", fw] if fw else [])
    rc, o = cli(*args, card=card, cat=cat)
    check("refused: " + what, rc != 0 and expect in o and tree(card) == before, o)

def rezip(data, edit, method=zipfile.ZIP_STORED):
    z = zipfile.ZipFile(io.BytesIO(data))
    files = {n: z.read(n) for n in z.namelist()}
    edit(files)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as out:
        for n, d in files.items():
            out.writestr(n, d)
    return buf.getvalue()

def relist(files):
    m = json.loads(files["p4app.json"])
    m["files"] = [{"path": n, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()} for n, d in files.items() if n != "p4app.json"]
    files["p4app.json"] = json.dumps(m).encode()

bad_case("not the bytes the catalogue names", mutate_entry=lambda e: e["pkg"].update(sha256="0" * 64), expect="not the file the catalogue names")
bad_case("another size", mutate_entry=lambda e: e["pkg"].update(size=e["pkg"]["size"] + 1), expect="bytes and the catalogue says")
bad_case("a newer firmware's", mutate_entry=lambda e: e.update(fw_min="9.9.9"), fw="0.29.0", expect="needs P4OS 9.9.9")
bad_case("another ABI", mutate_entry=lambda e: e.update(abi=101), expect="ABI")
def esc(files): files["apps/../../evil.so"] = b"x"; relist(files)
bad_case("a path that climbs out", mutate_pkg=lambda d: rezip(d, esc), expect="may not write")
def sysf(files): files["lang/en/_sistema.lang"] = b"x"; files["menu.txt"] = b"x"; relist(files)
bad_case("a file outside apps/, web/ and lang/", mutate_pkg=lambda d: rezip(d, sysf), expect="may not write menu.txt")
def other_so(files): files["apps/doom.so"] = b"x"; relist(files)
bad_case("another app's .so", mutate_pkg=lambda d: rezip(d, other_so), expect="may not write apps/doom.so")
def unlisted(files): files["web/extra.js"] = b"x"
bad_case("a file p4app.json does not list", mutate_pkg=lambda d: rezip(d, unlisted), expect="not the ones its p4app.json lists")
def changed(files): files["web/hello.js"] = files["web/hello.js"] + b"//"
bad_case("a file changed after p4app.json", mutate_pkg=lambda d: rezip(d, changed), expect="not what p4app.json says")
bad_case("a compressed package", mutate_pkg=lambda d: rezip(d, lambda f: None, zipfile.ZIP_DEFLATED), expect="WITHOUT compression")
def wrong_id(files):
    m = json.loads(files["p4app.json"]); m["id"] = "demo.other"; files["p4app.json"] = json.dumps(m).encode()
bad_case("a package that says it is another app", mutate_pkg=lambda d: rezip(d, wrong_id), expect="the package says id")
def owned(card):
    os.makedirs(card + "/apps/.store"); os.makedirs(card + "/web")
    open(card + "/web/hello.js", "w").write("theirs")
    json.dump({"file": "other", "id": "x.other", "files": [{"path": "web/hello.js"}], "packs": []}, open(card + "/apps/.store/other.json", "w"))
bad_case("a file that is another package's", pre=owned, expect="belongs to other")
sys.exit(1 if fails else 0)
PY
echo "all passed"
