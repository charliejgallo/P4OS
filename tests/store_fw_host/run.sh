#!/bin/bash
# The firmware's store service (components/aos_store/aos_store.c) on the Mac,
# against tools/fake_github.py and against tools/store_cli.py, the reference:
# the same catalogue installed by each onto a card of its own must leave the
# same files, and each must read the other's receipts. Then an update, an
# extra pack kept through it, adopting what was copied by hand, removing, and
# the packages the service has to refuse, with the card left as it was. And
# sources: a second repository looked at, added, its apps offered beside the
# official ones but never in their place, taken off with its apps or without
# them; and a package handed in by itself.
#
#   tests/store_fw_host/run.sh         needs the apps built (tools/build_apps.sh)
#   BUILT=<a tree with apps/*/build> tests/store_fw_host/run.sh
#                                      (brew: mbedtls; cJSON from ESP-IDF)
set -e
cd "$(dirname "$0")"
P4=$(cd ../.. && pwd)
BUILT=${BUILT:-$P4}
MB=${MBEDTLS:-/opt/homebrew/opt/mbedtls}
CJ=${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON
APPS="hello_app dados emulador golf pinball mila"
for a in $APPS; do
    ls "$BUILT"/apps/$a/build/*.so >/dev/null 2>&1 || { echo "apps/$a is not built in $BUILT: tools/build_apps.sh $a (or BUILT=...)"; exit 1; }
done
out=$(mktemp -d)
PORT=${PORT:-18690}

cc -O1 -g -Wall -Wextra -Werror -DAOS_SIM -DAOS_STORE_ABI=100 \
   -I"$P4/components/aos_hal/include" -I"$P4/components/aos_hal" -I"$P4/components/aos_store/include" \
   -I"$CJ" -I"$MB/include" -o "$out/t" main.c fakes.c \
   "$P4/components/aos_store/aos_store.c" "$P4/components/aos_hal/aos_fetch.c" "$P4/components/aos_hal/aos_tls.c" \
   "$P4/components/aos_hal/aos_crypto.c" "$CJ/cJSON.c" -L"$MB/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lpthread

BASE="http://localhost:$PORT/fake/p4os/releases/download/"
dirs=""
for a in golf pinball mila; do dirs="$dirs --packs-dir $BUILT/apps/$a/build"; done
python3 "$P4/tools/gen_store.py" build --release v9.0.0 --out "$out/r1" --built "$BUILT" \
    --only $(echo $APPS | tr ' ' ',') --base "$BASE" $dirs > /dev/null
mkdir "$out/rel"
cp "$out"/r1/* "$out/rel/"
for a in golf pinball mila; do for f in "$BUILT"/apps/$a/build/*_p4.pak*; do ln -s "$f" "$out/rel/$(basename "$f")"; done; done
python3 "$P4/tools/fake_github.py" --dir "$out/rel" --port $PORT --quiet > "$out/fake.log" 2>&1 &
FAKE=$!
# somebody else's repository, on another port
mkdir "$out/rel3"
python3 "$P4/tools/fake_github.py" --dir "$out/rel3" --port $((PORT + 20)) --repo otra/gente --quiet > "$out/fake3.log" 2>&1 &
FAKE3=$!
trap 'rc=$?; kill $FAKE $FAKE3 2>/dev/null || true; wait $FAKE $FAKE3 2>/dev/null || true; rm -rf "$out"; exit $rc' EXIT
for i in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/_fake" && break; sleep 0.1; done

cd "$out"
P4="$P4" BUILT="$BUILT" PORT=$PORT python3 - <<'PY'
import glob, hashlib, io, json, os, shutil, subprocess, sys, zipfile
P4, BUILT, PORT = os.environ["P4"], os.environ["BUILT"], os.environ["PORT"]
LATEST = "http://localhost:%s/fake/p4os/releases/latest/download/" % PORT
CAT = LATEST + "p4os-store.ndjson"
ALL = ["hello_app", "dados", "emulador", "golf", "pinball", "mila"]
fails = 0

def fw(card, *steps, cat=CAT, env=None, dev=True):
    """The C service: a list of (ok, text) per job, and 'list' as a dict.
    The test's servers are http://, so it runs in developer mode unless told
    not to; the board's preferences are a file beside the card."""
    e = dict(os.environ, P4_CARD=card, P4_PREFS=card + ".prefs")
    e.update(env or {})
    first = ["dev=1", "url=" + cat] if dev else []
    p = subprocess.run(["./t"] + first + list(steps), capture_output=True, text=True, env=e, timeout=300)
    jobs, apps = [], {}
    for line in p.stdout.splitlines():
        w = line.split(" ")
        if w[0] in ("ok", "FAILED"):
            jobs.append((w[0] == "ok", line))
        elif len(w) == 5 and w[0] not in ("source", "probed", "proposed"):
            apps[w[0]] = w[1:]
    return jobs, apps, p.stdout + p.stderr

def cli(*args, card, cat=CAT):
    p = subprocess.run([sys.executable, P4 + "/tools/store_cli.py", args[0], "--card", card, "--catalog", cat] + list(args[1:]),
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr

def check(what, ok, detail=""):
    global fails
    print("%-58s %s" % (what, "ok" if ok else "FAILED"))
    if not ok:
        fails += 1
        print("    " + str(detail).replace("\n", "\n    ")[:1800])

def tree(card, receipts=False):
    out = {}
    for d, _, fs in os.walk(card):
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), card)
            if rel.startswith("apps/.store") and not (receipts and rel.endswith(".json") and "/tmp/" not in rel):
                continue
            out[rel] = hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest()
    return out

def receipt(card, file):
    return json.load(open("%s/apps/.store/%s.json" % (card, file)))

# 1. the same catalogue by the service and by the reference, each on a card of its own
os.mkdir("c"); os.mkdir("py")
jobs, apps, o = fw("c", "refresh", *["install=" + a for a in ALL], "list")
check("the service: an empty card, every app", len(jobs) == 7 and all(j[0] for j in jobs)
      and all(apps[a][0] == "installed" for a in ALL), o)
rc, o2 = cli("install", "--all", card="py")
tc, tp = tree("c"), tree("py")
check("the same files as the reference leaves, byte for byte", rc == 0 and tc == tp and len(tc) > 30,
      sorted(set(tc.items()) ^ set(tp.items()))[:8])
same = all(receipt("c", a)["files"] == receipt("py", a)["files"] and receipt("c", a)["packs"] == receipt("py", a)["packs"]
           and receipt("c", a)["pkg_sha256"] == receipt("py", a)["pkg_sha256"] and receipt("c", a)["ver"] == "v9.0.0" for a in ALL)
check("the same receipts: files, packs, version", same, receipt("c", "mila"))
rc, o = cli("status", card="c")
check("the reference reads the service's receipts", rc == 0 and o.count("up to date") == 6 and "by hand" not in o, o)
jobs, apps, o = fw("py", "refresh", "list")
check("the service reads the reference's receipts", all(apps[a][0] == "installed" for a in ALL), o)
check("nothing left behind in tmp/", not [f for d, _, fs in os.walk("c/apps/.store/tmp") for f in fs], tree("c", True))

# 2. again: nothing is downloaded but the packages
import urllib.request
served = lambda: json.loads(urllib.request.urlopen("http://localhost:%s/_fake" % PORT).read())["bytes"]
b = served()
jobs, apps, o = fw("c", "install=mila")
check("again: the packs on the card are not downloaded", jobs[0][0] and served() - b < 400_000, served() - b)

# 3. an extra by name; all of them; it survives an update
jobs, apps, o = fw("c", "install=pinball:pinball_circo_p4.pak")
check("an extra asked for by name", jobs[0][0] and os.path.exists("c/apps/pinball_circo_p4.pak")
      and not os.path.exists("c/apps/pinball_robots_p4.pak")
      and [p["file"] for p in receipt("c", "pinball")["packs"]] == ["pinball_zombis_p4.pak", "pinball_circo_p4.pak"], o)

# 4. a new release in which one app changed
head = json.loads(open("r1/p4os-store.ndjson").readline())
os.makedirs("built2/apps")
for a in ALL:
    if a != "pinball":
        os.symlink("%s/apps/%s" % (BUILT, a), "built2/apps/" + a)
os.makedirs("built2/apps/pinball/build")
so = glob.glob(BUILT + "/apps/pinball/build/*.so")[0]
open("built2/apps/pinball/build/pinball.so", "wb").write(open(so, "rb").read() + b"\0changed\0")
dirs = sum((["--packs-dir", "%s/apps/%s/build" % (BUILT, a)] for a in ("golf", "pinball", "mila")), [])
subprocess.run([sys.executable, P4 + "/tools/gen_store.py", "build", "--release", "v9.1.0", "--out", "r2", "--built",
                os.path.abspath("built2"), "--only", ",".join(ALL), "--base", head["base"], "--previous",
                "r1/p4os-store.ndjson"] + dirs, capture_output=True, check=True)
for f in os.listdir("r2"):
    shutil.copy("r2/" + f, "rel/" + f)
jobs, apps, o = fw("c", "refresh", "list")
check("a new catalogue: one update", jobs[0][0] and apps["pinball"][:3] == ["update", "v9.1.0", "v9.0.0"]
      and sum(v[0] == "installed" for v in apps.values()) == 5, o)
jobs, apps, o = fw("c", "install=pinball", "list")
check("the update: new code, the extra kept", jobs[0][0] and apps["pinball"][0] == "installed"
      and open("c/apps/pinball.so", "rb").read().endswith(b"\0changed\0")
      and "pinball_circo_p4.pak" in [p["file"] for p in receipt("c", "pinball")["packs"]], o)
apps2 = {json.loads(l)["file"]: json.loads(l) for l in open("r2/p4os-store.ndjson").read().splitlines()[1:]}
head2 = json.loads(open("r2/p4os-store.ndjson").readline())

# 5. copied by hand: seen, adopted when it is the catalogue's, left alone when not
os.makedirs("c2/apps")
shutil.copy(BUILT + "/apps/golf/build/golf.so", "c2/apps/golf.so")
shutil.copy(BUILT + "/apps/golf/build/golf_p4.pak", "c2/apps/golf_p4.pak")
shutil.copy(BUILT + "/apps/mila/build/mila.so", "c2/apps/mila.so")          # without its pack
open("c2/apps/dados.so", "wb").write(b"an older build")
jobs, apps, o = fw("c2", "refresh", "list")
check("by hand: the service says so", apps["golf"][0] == "by_hand" and apps["dados"][0] == "by_hand" and apps["emulador"][0] == "absent", o)
jobs, apps, o = fw("c2", "adopt", "list")
check("adopt: a receipt for the catalogue's, the pack with it", jobs[0][0] and "2 adopted, 1 left" in jobs[0][1]
      and apps["golf"][0] == "installed" and apps["dados"][0] == "by_hand"
      and receipt("c2", "golf").get("adopted") is True and receipt("c2", "golf")["packs"][0]["file"] == "golf_p4.pak"
      and receipt("c2", "mila")["packs"] == [] and os.path.exists("c2/lang/en/demo.golf.lang")
      and open("c2/apps/dados.so", "rb").read() == b"an older build", o)

# 6. remove
before = tree("c")
jobs, apps, o = fw("c", "remove=mila", "remove=nothing", "list")
t = tree("c", True)
check("remove: files, packs and receipt; the rest stays", jobs[0][0] and not jobs[1][0] and apps["mila"][0] == "absent"
      and not [x for x in t if "mila" in x] and "apps/golf_p4.pak" in t and len(before) - len(tree("c")) == 6, o)

# 6b. set aside by the loader (it fell twice while loading): seen, and installing again is the way back
os.rename("c/apps/dados.so", "c/apps/dados.so.off")
open("c/apps/.store/tmp/f0", "w").write("left by an install that a restart cut")
jobs, apps, o = fw("c", "list")
check("set aside by the loader: the service says so", apps["dados"][0] == "set_aside"
      and not os.path.exists("c/apps/.store/tmp/f0"), o)
jobs, apps, o = fw("c", "install=dados", "list")
check("...and installing it again puts it back", jobs[0][0] and apps["dados"][0] == "installed"
      and os.path.exists("c/apps/dados.so") and not os.path.exists("c/apps/dados.so.off"), o)

# 7. a firmware that is too old for the catalogue
bad = dict(apps2["dados"]); bad["fw_min"] = "9.9.9"
open("rel/old.ndjson", "w").write(json.dumps(head2) + "\n" + json.dumps(bad) + "\n")
os.mkdir("c3")
jobs, apps, o = fw("c3", "refresh", "list", "install=dados", cat=LATEST + "old.ndjson", env={"FAKE_FW": "v0.29.0-5-gefef5a5"})
check("a newer firmware's app: listed as not fitting, refused", apps["dados"][3] == "no-fit" and not jobs[1][0]
      and "needs P4OS 9.9.9" in jobs[1][1] and tree("c3") == {}, o)

# ---- packages that must be refused: a catalogue each, the card as it was
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

n_bad = 0
def bad_case(what, expect, mutate_pkg=None, mutate_entry=None, pre=None):
    global n_bad
    n_bad += 1
    card = "bad%d" % n_bad
    os.makedirs(card)
    if pre:
        pre(card)
    e = json.loads(json.dumps(apps2["hello_app"]))
    if mutate_pkg:
        data = mutate_pkg(open("rel/hello_app.p4app", "rb").read())
        open("rel/bad%d.p4app" % n_bad, "wb").write(data)
        e["pkg"].update(url="v9.1.0/bad%d.p4app" % n_bad, size=len(data), sha256=hashlib.sha256(data).hexdigest())
    if mutate_entry:
        mutate_entry(e)
    open("rel/bad%d.ndjson" % n_bad, "w").write(json.dumps(head2) + "\n" + json.dumps(e) + "\n")
    before = tree(card, True)
    jobs, apps, o = fw(card, "refresh", "install=hello_app", cat=LATEST + "bad%d.ndjson" % n_bad)
    after = tree(card, True)
    check("refused: " + what, len(jobs) == 2 and jobs[0][0] and not jobs[1][0] and expect in jobs[1][1] and after == before
          and not [f for d, _, fs in os.walk(card + "/apps/.store/tmp") for f in fs], (o, set(after) ^ set(before)))

bad_case("not the bytes the catalogue names", "not the one published", mutate_entry=lambda e: e["pkg"].update(sha256="0" * 64))
bad_case("another size", "expected", mutate_entry=lambda e: e["pkg"].update(size=e["pkg"]["size"] + 1))
bad_case("another ABI", "ABI", mutate_entry=lambda e: e.update(abi=101))
def esc(f): f["apps/../../evil.so"] = b"x"; relist(f)
bad_case("a path that climbs out", "may not write", mutate_pkg=lambda d: rezip(d, esc))
def sysf(f): f["menu.txt"] = b"x"; relist(f)
bad_case("a file outside apps/, web/ and lang/", "may not write menu.txt", mutate_pkg=lambda d: rezip(d, sysf))
def lang3(f): f["lang/en/x/deep.lang"] = b"x"; relist(f)
bad_case("a folder deeper than a package's", "may not write", mutate_pkg=lambda d: rezip(d, lang3))
def other_so(f): f["apps/doom.so"] = b"x"; relist(f)
bad_case("another app's .so", "may not write apps/doom.so", mutate_pkg=lambda d: rezip(d, other_so))
def hidden(f): f["apps/.store/evil.so"] = b"x"; relist(f)
bad_case("a file among the receipts", "may not write", mutate_pkg=lambda d: rezip(d, hidden))
def unlisted(f): f["web/extra.js"] = b"x"
bad_case("a file p4app.json does not list", "not the ones its p4app.json lists", mutate_pkg=lambda d: rezip(d, unlisted))
def changed(f): f["web/hello.js"] = f["web/hello.js"] + b"//"
bad_case("a file changed after p4app.json", "not what p4app.json says", mutate_pkg=lambda d: rezip(d, changed))
bad_case("a compressed package", "without compression", mutate_pkg=lambda d: rezip(d, lambda f: None, zipfile.ZIP_DEFLATED))
def wrong_id(f):
    m = json.loads(f["p4app.json"]); m["id"] = "demo.other"; f["p4app.json"] = json.dumps(m).encode()
bad_case("a package that says it is another app", "not what the catalogue lists", mutate_pkg=lambda d: rezip(d, wrong_id))
bad_case("not a zip at all", "not a package", mutate_pkg=lambda d: b"P4OS" * 300)
bad_case("a zip cut short", "", mutate_pkg=lambda d: d[:len(d) // 2])
def owned(card):
    os.makedirs(card + "/apps/.store"); os.makedirs(card + "/web")
    open(card + "/web/hello.js", "w").write("theirs")
    json.dump({"file": "other", "id": "x.other", "files": [{"path": "web/hello.js"}], "packs": []}, open(card + "/apps/.store/other.json", "w"))
bad_case("a file that is another package's", "belongs to other", pre=owned)

# 8. a catalogue that does not read must not take the place of the one that does
open("rel/broken.ndjson", "w").write("this is not a catalogue\n")
jobs, apps, o = fw("c", "refresh", "list", cat=LATEST + "broken.ndjson")
check("a catalogue that does not read: the old one stays", not jobs[0][0] and len(apps) == 6 and apps["golf"][0] == "installed", o)
open("rel/future.ndjson", "w").write(json.dumps(dict(head2, schema=2)) + "\n")
jobs, apps, o = fw("c", "refresh", "list", cat=LATEST + "future.ndjson")
check("a catalogue of a schema to come: says so, the old one stays", not jobs[0][0] and "update P4OS" in jobs[0][1] and len(apps) == 6, o)

# ---- sources: somebody else's repository beside the official one
PORT3 = int(PORT) + 20
THEIRS = "http://localhost:%d/otra/gente/releases/latest/download/p4os-store.ndjson" % PORT3
def package(file, app_id, name):
    """hello_app's package made into another app's."""
    def edit(f):
        f["apps/%s.so" % file] = f.pop("apps/hello_app.so")
        for k in [k for k in f if k.startswith(("lang/", "web/"))]:
            del f[k]
        m = json.loads(f["p4app.json"]); m.update(file=file, id=app_id, name={"es": name, "en": name}); f["p4app.json"] = json.dumps(m).encode()
        relist(f)
    return rezip(open("rel/hello_app.p4app", "rb").read(), edit)
def entry(file, app_id, name, data):
    e = json.loads(json.dumps(apps2["hello_app"]))
    e.update(file=file, id=app_id, name={"es": name, "en": name}, ver="v1.0.0", cat="juegos")
    e.pop("packs", None); e.pop("shots", None)
    e["pkg"].update(url="v1.0.0/%s.p4app" % file, size=len(data), sha256=hashlib.sha256(data).hexdigest())
    return e
tercero = package("tercero", "x.tercero", "De Terceros")
open("rel3/tercero.p4app", "wb").write(tercero)
dados_falso = package("dados", "x.dados", "Dados de otro")
open("rel3/dados.p4app", "wb").write(dados_falso)
head3 = {"schema": 1, "source": {"name": "Otra Gente", "by": "Alguien", "url": "https://example.org/otra", "about": {"es": "Apps de otra gente", "en": "Other people's apps"}},
         "cats": [], "release": "v1.0.0", "base": "http://localhost:%d/otra/gente/releases/download/" % PORT3}
open("rel3/p4os-store.ndjson", "w").write("\n".join(json.dumps(x) for x in (head3, entry("tercero", "x.tercero", "De Terceros", tercero),
                                                                             entry("dados", "x.dados", "Dados de otro", dados_falso))) + "\n")
def sources(o):
    return [l.split(" ") for l in o.splitlines() if l.startswith("source ")]

os.mkdir("s0")
jobs, apps, o = fw("s0", "look=" + THEIRS, dev=False)
check("an http:// source without developer mode: refused", not jobs[0][0] and "developer mode" in jobs[0][1], o)
jobs, apps, o = fw("c", "refresh", "add=" + THEIRS, "sources")
check("adding a source nobody looked at: refused", jobs[0][0] and not jobs[1][0] and "look at the source first" in jobs[1][1] and len(sources(o)) == 1, o)
jobs, apps, o = fw("c", "look=" + THEIRS, "probed", "add=" + THEIRS, "sources", "list")
src = sources(o)
check("looked at, then added: who it says it is", jobs[0][0] and jobs[1][0] and '"name":"Otra Gente"' in o and "probed 1 apps=2" in o, o)
check("its apps beside the official ones, never in their place", len(src) == 2 and "source 1 Otra Gente apps=1 skipped=1 loaded=1" in o
      and apps["tercero"][0] == "absent" and apps["tercero"][3].endswith("+") and apps["dados"][0] == "installed"
      and not apps["dados"][3].endswith("+") and len(apps) == 7, o)
jobs, apps, o = fw("c", "install=tercero", "install=dados", "sources", "list")
r = receipt("c", "tercero")
check("an app of that source: installed, its receipt says whose", jobs[0][0] and apps["tercero"][0] == "installed"
      and r["source"]["name"] == "Otra Gente" and r["source"]["catalog"] == THEIRS and os.path.exists("c/apps/tercero.so")
      and receipt("c", "dados")["source"]["name"] == "P4OS" and len(sources(o)) == 2, o)
jobs, apps, o = fw("c", "refresh", "sources", "list")
check("a refresh brings both catalogues", jobs[0][0] and len(sources(o)) == 2 and "tercero" in apps, o)

# a package handed in: by itself, not in the place of the store's
open("c/in.p4app", "wb").write(package("suelto", "x.suelto", "Suelto"))
open("c/hello.p4app", "wb").write(open("rel/hello_app.p4app", "rb").read())
open("c/otro.p4app", "wb").write(tercero)
jobs, apps, o = fw("c", "sideload=in.p4app", "sideload=/hello.p4app", "sideload=otro.p4app", "sideload=../x.p4app")
check("a package handed in: installed, the file left where it was", jobs[0][0] and os.path.exists("c/apps/suelto.so")
      and receipt("c", "suelto")["source"] == {"name": "local", "url": "", "catalog": ""} and os.path.exists("c/in.p4app"), o)
check("...but not an app the store offers", not jobs[1][0] and "is in the Tienda (P4OS)" in jobs[1][1]
      and not jobs[2][0] and "is in the Tienda (Otra Gente)" in jobs[2][1] and "FAILED refused: sideload" in o, o)

# what is on the card and no source lists is in the list too: by its receipt, or as a .so and no more
shutil.copy("c/apps/suelto.so", "c/apps/a_mano.so")
jobs, apps, o = fw("c", "list", "install=suelto", "install=a_mano", "adopt", "list")
check("a package handed in and a .so copied by hand: listed, as what they are", apps["suelto"] == ["installed", "local", "local", "fits~"]
      and apps["a_mano"] == ["by_hand", "-", "-", "fits!"] and apps["dados"][3] == "fits", o)
check("...and neither can be brought from a source", not jobs[0][0] and jobs[0][1].endswith("kind=14") and not jobs[1][0]
      and not os.path.exists("c/apps/.store/a_mano.json"), o)
cat_text = subprocess.run(["./t", "dev=1", "url=" + CAT, "catalog"], capture_output=True, text=True,
                          env=dict(os.environ, P4_CARD="c", P4_PREFS="c.prefs")).stdout
local = {json.loads(l)["file"]: json.loads(l) for l in cat_text.splitlines() if l.startswith("{") and "\"local\"" in l}
check("their lines in the catalogue's text", local.get("suelto", {}).get("local") == "pkg" and local["suelto"]["id"] == "x.suelto"
      and local["suelto"]["name"]["es"] == "Suelto" and local.get("a_mano", {}).get("local") == "hand"
      and local["a_mano"]["pkg"]["installed"] == os.path.getsize("c/apps/a_mano.so"), cat_text[-600:])
jobs, apps, o = fw("c", "remove=suelto", "remove=a_mano", "list")
check("the package handed in is removed like any other; the .so by hand is not the store's", jobs[0][0] and "suelto" not in apps
      and not os.path.exists("c/apps/suelto.so") and not jobs[1][0] and apps["a_mano"][0] == "by_hand", o)
os.remove("c/apps/a_mano.so")
fw("c", "sideload=in.p4app")

# the source taken off: without its apps, then with them
jobs, apps, o = fw("c", "unsource=1:0", "sources", "list")
check("a source taken off, its apps kept: listed as nobody's now", jobs[0][0] and len(sources(o)) == 1
      and apps["tercero"][0] == "installed" and apps["tercero"][3] == "fits~"
      and os.path.exists("c/apps/tercero.so") and os.path.exists("c/apps/.store/tercero.json"), o)
jobs, apps, o = fw("c", "sideload=otro.p4app")
check("a package over what a source installed: refused", not jobs[0][0] and "remove it first" in jobs[0][1], o)
jobs, apps, o = fw("c", "look=" + THEIRS, "add=" + THEIRS, "unsource=1:1", "unsource=0:1", "sources", "list")
check("taken off with its apps; the official one stays", jobs[1][0] and jobs[2][0] and "with 1 of its apps" in jobs[2][1]
      and not os.path.exists("c/apps/tercero.so") and not os.path.exists("c/apps/.store/tercero.json")
      and len(sources(o)) == 1 and apps["dados"][0] == "installed" and os.path.exists("c/apps/suelto.so"), o)

# how each failure is told to the screen: its kind, to say it in the person's language
kinds = {}
for card in ("k1", "k2", "k3"):
    os.mkdir(card)
jobs, apps, o = fw("k1", "refresh", "install=nothing", "remove=nothing", "sideload=nothing.p4app", "add=https://example.org/c.ndjson")
kinds["not listed"] = jobs[1][1].endswith("kind=14"); kinds["not the store's"] = jobs[2][1].endswith("kind=16")
kinds["no such package"] = jobs[3][1].endswith("kind=7"); kinds["a source not looked at"] = jobs[4][1].endswith("kind=11")
jobs, apps, o = fw("k2", "refresh", "install=hello_app", cat=LATEST + "bad1.ndjson")
kinds["not the bytes published"] = jobs[1][1].endswith("kind=7")
jobs, apps, o = fw("k2", "refresh", "install=hello_app", cat=LATEST + "bad4.ndjson")
kinds["a path it may not write"] = jobs[1][1].endswith("kind=8")
jobs, apps, o = fw("k3", "refresh", "install=dados", cat=LATEST + "old.ndjson", env={"FAKE_FW": "v0.29.0"})
kinds["a newer firmware's"] = jobs[1][1].endswith("kind=9")
jobs, apps, o = fw("k3", "refresh", cat=LATEST + "nothing-here.ndjson")
kinds["not on the server"] = jobs[0][1].endswith("kind=3")
jobs, apps, o = fw("k3", "refresh", cat=LATEST + "broken.ndjson")
kinds["a catalogue that does not read"] = jobs[0][1].endswith("kind=10")
jobs, apps, o = fw("k3", "refresh", cat="http://localhost:1/x.ndjson")
kinds["nobody answers"] = jobs[0][1].endswith("kind=2")
check("each failure says its kind for the screen", all(kinds.values()), kinds)
# a catalogue that never ends is dropped while it downloads, not once it filled the card
with open("rel/endless.ndjson", "w") as f:
    f.write(open("rel/p4os-store.ndjson").read())
    f.write(" " * (3 * 1024 * 1024) + "\n")
jobs, apps, o = fw("k3", "refresh", cat=LATEST + "endless.ndjson")
check("a catalogue over the limit: stopped, nothing kept", not jobs[0][0] and "it is over 1024 KB" in jobs[0][1] and jobs[0][1].endswith("kind=10")
      and not [n for n in os.listdir("k3/apps/.store") if "catalog.new" in n], o)
os.remove("rel/endless.ndjson")

# a source asked for from the portal: kept for the board to decide, never added
jobs, apps, o = fw("k1", "propose=ana/apps", "propose=ftp://x", "sources")
check("a source proposed: kept, not added", "proposed 1 https://github.com/ana/apps/releases/latest/download/p4os-store.ndjson" in o
      and "proposed 0" in o and len(sources(o)) == 1, o)
jobs, apps, o = fw("s0", "propose=http://localhost:9/c.ndjson", dev=False)
check("...and not an http one without developer mode", "proposed 0" in o, o)
sys.exit(1 if fails else 0)
PY
echo "all passed"
