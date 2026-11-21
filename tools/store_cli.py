#!/usr/bin/env python3
"""
The Tienda, from a computer: reads a catalogue and installs, updates and
removes apps on a card or on a board, the way the board's own store will
(docs/plan/TIENDA.md). It is the reference for that: what is checked, what
is refused and what is written here is what the firmware has to do.

    tools/store_cli.py list     [where] [--catalog URL|file]
    tools/store_cli.py status   <where> [--catalog URL|file]
    tools/store_cli.py install  <where> <app>...|--all [--packs need|all|none|a,b] [--catalog ...]
    tools/store_cli.py adopt    <where> [--catalog ...]
    tools/store_cli.py remove   <where> <app>...

<where> is one of:

    --card <folder>     the card in a reader, or the simulator's sim/sim_fs
    --board <host>      a board or the simulator, through the portal
                        (p4os.local, an address, localhost:8080;
                        P4OS_TOKEN=<token> when it asks for its password)

<app> is a package's file name ("bastion") or the app's id ("demo.bastion").
The catalogue is the official one unless --catalog says another.

What an install does, in order:

 1. The catalogue's line is checked against the board: the ABI, and
    "fw_min" against its firmware (--fw says it for a card).
 2. The package is downloaded and its size and SHA-256 compared with the
    catalogue's. Nothing has touched the card yet.
 3. The package is opened: a zip without compression, a p4app.json that
    agrees with the catalogue, every file with the hash p4app.json gives it,
    and every path one a package may write - apps/<file>.so, apps/<app>/...,
    web/..., lang/<code>/... - and not one that another package's receipt
    lists. Anything else refuses the whole package.
 4. On a board, the app is taken off the launcher if it can be
    (/api/store/unload).
 5. The files are written, each to <name>.part first; files the old
    version had and the new one does not are deleted.
 6. The packs: the needed ones and the extras asked for, each part checked
    like the package; parts the old pack had beyond the new one's are
    deleted (a stale .pak.2 would be read as part of the new pack).
 7. The receipt, apps/.store/<file>.json, is written LAST: source, version,
    every file and pack with its hash. An install cut short leaves the old
    receipt, or none, and is simply done again.
 8. On a board, the app is registered again (/api/store/load), or the tool
    says that a restart will.

"adopt" writes receipts for what was copied by hand: an apps/<file>.so with
no receipt whose SHA-256 is the catalogue's becomes that version, with the
rest of the package's files checked one by one. What does not match is left
alone and reported.

The standard library only.
"""
import argparse
import hashlib
import io
import json
import os
import re
import socket
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

OFFICIAL = "https://github.com/charliejgallo/P4OS/releases/latest/download/p4os-store.ndjson"
SCHEMA = 1
ABI = 100
RECEIPTS = "apps/.store"
UA = {"User-Agent": "P4OS-store-cli/1"}


class Refused(Exception):
    pass


def vkey(v):
    return tuple(int(x) for x in re.findall(r"\d+", v or "")[:3]) or (0,)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# ---- where the files go ----------------------------------------------------

class Card:
    """A folder that is the card's root."""

    def __init__(self, root):
        self.root = root
        self.name = root
        if not os.path.isdir(root):
            raise Refused("%s is not a folder" % root)

    def _p(self, path):
        return os.path.join(self.root, *path.split("/"))

    def read(self, path):
        try:
            with open(self._p(path), "rb") as f:
                return f.read()
        except OSError:
            return None

    def sha256(self, path):
        h = hashlib.sha256()
        try:
            with open(self._p(path), "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
        except OSError:
            return None
        return h.hexdigest()

    def write(self, path, src):
        """src: bytes, or the path of a local file."""
        dst = self._p(path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst + ".part", "wb") as out:
            if isinstance(src, bytes):
                out.write(src)
            else:
                with open(src, "rb") as f:
                    for block in iter(lambda: f.read(1 << 20), b""):
                        out.write(block)
        os.replace(dst + ".part", dst)

    def delete(self, path):
        try:
            os.remove(self._p(path))
        except OSError:
            pass

    def list(self, path):
        try:
            return sorted(n for n in os.listdir(self._p(path)) if not n.endswith(".part"))
        except OSError:
            return []

    def firmware(self):
        return None

    def unload(self, file):
        return None

    def load(self, file):
        return None


class Board:
    """A board, or the simulator, through its portal's file calls."""

    def __init__(self, host):
        # the name looked up once: p4os.local answers no AAAA and urllib
        # waits five seconds for one on every call
        h, _, port = host.partition(":")
        try:
            h = socket.gethostbyname(h)
        except OSError:
            pass
        self.base = "http://%s%s/api/" % (h, ":" + port if port else "")
        self.name = host
        self._info = self._json("info")

    def _req(self, method, path, data=None, timeout=600):
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if os.environ.get("P4OS_TOKEN"):
            req.add_header("Authorization", "Bearer " + os.environ["P4OS_TOKEN"])
        return urllib.request.urlopen(req, timeout=timeout)

    def _json(self, path, method="GET"):
        return json.loads(self._req(method, path, timeout=30).read())

    def read(self, path):
        try:
            return self._req("GET", "fs/get?path=/" + urllib.parse.quote(path)).read()
        except urllib.error.HTTPError:
            return None

    def sha256(self, path):
        data = self.read(path)
        return sha256(data) if data is not None else None

    def write(self, path, src):
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            try:
                self._req("POST", "fs/mkdir?path=/" + urllib.parse.quote("/".join(parts[:i])), timeout=30).read()
            except urllib.error.HTTPError:
                pass                        # there already
        # the portal writes to <file>.part and renames at the end itself
        if isinstance(src, bytes):
            self._req("PUT", "fs/put?path=/" + urllib.parse.quote(path), data=src).read()
        else:
            with open(src, "rb") as f:
                req = urllib.request.Request(self.base + "fs/put?path=/" + urllib.parse.quote(path), data=f, method="PUT")
                req.add_header("Content-Length", str(os.path.getsize(src)))
                if os.environ.get("P4OS_TOKEN"):
                    req.add_header("Authorization", "Bearer " + os.environ["P4OS_TOKEN"])
                urllib.request.urlopen(req, timeout=1800).read()

    def delete(self, path):
        try:
            self._req("POST", "fs/delete?path=/" + urllib.parse.quote(path), timeout=30).read()
        except urllib.error.HTTPError:
            pass

    def list(self, path):
        try:
            d = json.loads(self._req("GET", "fs?path=/" + urllib.parse.quote(path), timeout=30).read())
        except urllib.error.HTTPError:
            return []
        return sorted(e["name"] for e in d.get("entries", []) if not e["name"].endswith(".part"))

    def firmware(self):
        return self._info.get("firmware")

    def _hot(self, what, file):
        try:
            return bool(self._json("store/%s?file=%s.so" % (what, file), "POST").get("ok"))
        except urllib.error.HTTPError as e:
            # 409: not registered (nothing to take off) or it would not load; 404: no such call here
            return False if e.code == 409 else None
        except (urllib.error.URLError, ValueError):
            return None

    def unload(self, file):
        return self._hot("unload", file)

    def load(self, file):
        return self._hot("load", file)


# ---- the catalogue ---------------------------------------------------------

def fetch(url, expect_size=None, expect_sha=None, to_file=None):
    """A URL or a local path, whole, checked. Returns bytes, or with to_file
    writes there (for a pack that should not sit in memory)."""
    h = hashlib.sha256()
    size = 0
    out = open(to_file, "wb") if to_file else io.BytesIO()
    try:
        if re.match(r"https?://", url):
            src = urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120)
        else:
            src = open(url, "rb")
        with src:
            for block in iter(lambda: src.read(1 << 20), b""):
                h.update(block)
                size += len(block)
                out.write(block)
    except (urllib.error.URLError, OSError) as e:
        raise Refused("could not get %s: %s" % (url, e))
    finally:
        if to_file:
            out.close()
    if expect_size is not None and size != expect_size:
        raise Refused("%s is %d bytes and the catalogue says %d" % (url, size, expect_size))
    if expect_sha and h.hexdigest() != expect_sha:
        raise Refused("%s is not the file the catalogue names (SHA-256 %s...)" % (url, h.hexdigest()[:12]))
    return None if to_file else out.getvalue()


class Catalog:
    def __init__(self, where):
        self.where = where
        lines = fetch(where).decode("utf-8").splitlines()
        if not lines:
            raise Refused("%s is empty" % where)
        self.head = json.loads(lines[0])
        if self.head.get("schema") != SCHEMA:
            raise Refused("%s is a catalogue of schema %s; this tool reads %d" % (where, self.head.get("schema"), SCHEMA))
        self.apps = [json.loads(l) for l in lines[1:] if l.strip()]
        self.base = self.head["base"]
        self.source = {"name": self.head["source"]["name"], "url": self.head["source"]["url"], "catalog": where}

    def find(self, key):
        for a in self.apps:
            if key in (a["file"], a["id"]):
                return a
        raise Refused("%s is not in the catalogue" % key)

    def url(self, rel):
        return rel if re.match(r"https?://", rel) else self.base + rel


# ---- receipts --------------------------------------------------------------

def receipt_path(file):
    return "%s/%s.json" % (RECEIPTS, file)


def receipts(t):
    out = {}
    for n in t.list(RECEIPTS):
        if n.endswith(".json"):
            data = t.read("%s/%s" % (RECEIPTS, n))
            try:
                out[n[:-5]] = json.loads(data)
            except (TypeError, ValueError):
                pass
    return out


# ---- a package -------------------------------------------------------------

def allowed(path, file):
    """Whether a package named 'file' may write this path of the card."""
    if path.startswith("/") or "\\" in path or ".." in path.split("/") or "//" in path or path.endswith("/"):
        return False
    p = path.split("/")
    if p[0] == "apps":
        # its own .so, or a module in a folder of its own
        return (len(p) == 2 and p[1] == file + ".so") or \
               (len(p) == 3 and not p[1].startswith(".") and p[2].endswith(".so"))
    if p[0] == "web":
        return len(p) == 2 and re.fullmatch(r"[A-Za-z0-9_-]+\.m?js", p[1]) is not None
    if p[0] == "lang":
        return len(p) == 3 and re.fullmatch(r"[a-z]{2}", p[1]) is not None and p[2].endswith(".lang")
    return False


def open_package(data, entry, others):
    """The files of a package, {path: bytes}, or Refused. 'others' is what
    the receipts of every other package list."""
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        raise Refused("%s.p4app is not a zip" % entry["file"])
    names = z.namelist()
    if "p4app.json" not in names:
        raise Refused("no p4app.json in the package")
    for zi in z.infolist():
        if zi.compress_type != zipfile.ZIP_STORED:
            raise Refused("%s is compressed: a package is a zip WITHOUT compression" % zi.filename)
    try:
        inside = json.loads(z.read("p4app.json"))
    except ValueError:
        raise Refused("p4app.json does not read")
    for k in ("file", "id", "abi"):
        if inside.get(k) != entry[k]:
            raise Refused("the package says %s %r and the catalogue %r" % (k, inside.get(k), entry[k]))
    listed = {f["path"]: f for f in inside.get("files", [])}
    if set(names) - {"p4app.json"} != set(listed):
        raise Refused("the package's files are not the ones its p4app.json lists")
    if "apps/%s.so" % entry["file"] not in listed:
        raise Refused("the package has no apps/%s.so" % entry["file"])
    files = {}
    for path, f in listed.items():
        if not allowed(path, entry["file"]):
            raise Refused("a package may not write %s" % path)
        if path in others:
            raise Refused("%s belongs to %s, installed already" % (path, others[path]))
        data = z.read(path)
        if len(data) != f.get("size") or sha256(data) != f.get("sha256"):
            raise Refused("%s is not what p4app.json says" % path)
        files[path] = data
    return files, inside


def check_fits(entry, fw):
    if entry["abi"] != ABI:
        raise Refused("%s is built for ABI %s; the board's is %d" % (entry["file"], entry["abi"], ABI))
    # a version with no number in it (the simulator's "sim") is not one to compare
    if fw and re.search(r"\d+\.\d+", fw) and vkey(entry["fw_min"]) > vkey(fw):
        raise Refused("%s needs P4OS %s or newer, and this is %s" % (entry["file"], entry["fw_min"], fw))


def want_packs(entry, choice):
    packs = entry.get("packs", [])
    if choice == "none":
        return []
    if choice == "all":
        return packs
    if choice == "need":
        return [p for p in packs if p["need"]]
    names = set(choice.split(","))
    unknown = names - {p["file"] for p in packs}
    if unknown and names & {p["file"] for p in packs} == set():
        pass                                # another app's extras, in an --all
    return [p for p in packs if p["need"] or p["file"] in names]


def install(t, cat, entry, a):
    file = entry["file"]
    check_fits(entry, a.fw or t.firmware())
    recs = receipts(t)
    old = recs.get(file)
    others = {f["path"]: other for other, r in recs.items() if other != file for f in r.get("files", [])}
    others.update({"apps/" + part["name"]: other for other, r in recs.items() if other != file
                   for p in r.get("packs", []) for part in p["parts"]})
    pkg = fetch(cat.url(entry["pkg"]["url"]), entry["pkg"]["size"], entry["pkg"]["sha256"])
    files, _inside = open_package(pkg, entry, others)

    packs = want_packs(entry, a.packs)
    if old:
        # an extra the person already had stays theirs across an update
        had = {p["file"] for p in old.get("packs", [])}
        packs += [p for p in entry.get("packs", []) if p["file"] in had and p not in packs]
    for p in packs:
        for i in range(len(p["parts"])):
            name = "apps/" + p["file"] + (".%d" % i if i else "")
            if name in others:
                raise Refused("%s belongs to %s, installed already" % (name, others[name]))

    hot = t.unload(file)
    for path in sorted(files):
        t.write(path, files[path])
    if old:
        for f in old.get("files", []):
            if f["path"] not in files:
                t.delete(f["path"])

    done_packs = []
    with tempfile.TemporaryDirectory() as tmp:
        for p in packs:
            parts = []
            for i, part in enumerate(p["parts"]):
                name = p["file"] + (".%d" % i if i else "")
                # the bytes the card has already, by an earlier install or by hand: not downloaded again
                if t.sha256("apps/" + name) != part["sha256"]:
                    local = os.path.join(tmp, name)
                    fetch(cat.url(part["url"]), part["size"], part["sha256"], to_file=local)
                    t.write("apps/" + name, local)
                    os.remove(local)
                parts.append({"name": name, "size": part["size"], "sha256": part["sha256"]})
            # parts of the old pack beyond the new one's would be read as its end
            for n in t.list("apps"):
                m = re.fullmatch(re.escape(p["file"]) + r"\.(\d+)", n)
                if m and int(m.group(1)) >= len(p["parts"]):
                    t.delete("apps/" + n)
            done_packs.append({"file": p["file"], "parts": parts})
    if old:
        for p in old.get("packs", []):
            if p["file"] not in {x["file"] for x in done_packs}:
                for part in p["parts"]:
                    t.delete("apps/" + part["name"])

    receipt = {"schema": SCHEMA, "file": file, "id": entry["id"], "name": entry["name"], "ver": entry["ver"],
               "pkg_sha256": entry["pkg"]["sha256"], "source": cat.source, "installed": int(time.time()),
               "files": [{"path": p, "size": len(files[p]), "sha256": sha256(files[p])} for p in sorted(files)],
               "packs": done_packs}
    t.write(receipt_path(file), (json.dumps(receipt, ensure_ascii=False, indent=1) + "\n").encode())

    back = t.load(file)
    how = "registered, no restart" if back else \
          "a restart registers it" if hot is None or back is None else "it did not load: see the board's log"
    size = sum(len(v) for v in files.values()) + sum(part["size"] for p in done_packs for part in p["parts"])
    print("   %-12s %s  %d files%s, %.1f MB  (%s)" % (file, entry["ver"], len(files),
          ", %d packs" % len(done_packs) if done_packs else "", size / 1e6,
          how if isinstance(t, Board) else "updated" if old else "installed"))


def remove(t, file):
    recs = receipts(t)
    r = recs.get(file) or next((x for x in recs.values() if x.get("id") == file), None)
    if not r:
        raise Refused("%s was not installed by the store (no receipt): delete its files by hand" % file)
    t.unload(r["file"])
    for f in r.get("files", []):
        t.delete(f["path"])
    for p in r.get("packs", []):
        for part in p["parts"]:
            t.delete("apps/" + part["name"])
    t.delete(receipt_path(r["file"]))
    print("   %-12s removed: %d files, %d packs. What the app saved of its own stays on the card." %
          (r["file"], len(r.get("files", [])), len(r.get("packs", []))))


# ---- the commands ----------------------------------------------------------

def target(a):
    if a.card:
        return Card(a.card)
    if a.board:
        return Board(a.board)
    return None


def cmd_list(a):
    cat = Catalog(a.catalog)
    t = target(a)
    recs = receipts(t) if t else {}
    names = {c["id"]: c["name"]["en"] for c in cat.head.get("cats", [])}
    print("%s, release %s: %d apps" % (cat.head["source"]["name"], cat.head["release"], len(cat.apps)))
    last = None
    for e in cat.apps:
        if e["cat"] != last:
            last = e["cat"]
            print("\n%s" % names.get(last, last))
        packs = e.get("packs", [])
        need = sum(p["size"] for p in packs if p["need"])
        extra = sum(1 for p in packs if not p["need"])
        r = recs.get(e["file"])
        state = "" if not r else "  [installed]" if r.get("pkg_sha256") == e["pkg"]["sha256"] else "  [update]"
        print("  %-11s %-18s %5.1f MB%s  %s%s" % (e["file"], e["name"].get("en", e["name"]["es"])[:18],
              (e["pkg"]["size"] + need) / 1e6, " +%d" % extra if extra else "   ", e["about"]["en"][:70], state))
    return 0


def cmd_status(a):
    cat = Catalog(a.catalog)
    t = target(a)
    recs = receipts(t)
    by_file = {e["file"]: e for e in cat.apps}
    fw = a.fw or t.firmware()
    print("%s%s: %d apps installed by the store" % (t.name, " (P4OS %s)" % fw if fw else "", len(recs)))
    for file, r in sorted(recs.items()):
        e = by_file.get(file)
        if not e:
            state = "not in this catalogue (from %s)" % r.get("source", {}).get("name", "?")
        elif e["pkg"]["sha256"] == r.get("pkg_sha256"):
            state = "up to date"
        else:
            state = "update: %s -> %s" % (r.get("ver"), e["ver"])
            if fw and re.search(r"\d+\.\d+", fw) and vkey(e["fw_min"]) > vkey(fw):
                state += " (needs P4OS %s)" % e["fw_min"]
        print("   %-12s %-9s %s" % (file, r.get("ver", "?"), state))
    by_hand = [n[:-3] for n in t.list("apps") if n.endswith(".so") and n[:-3] not in recs]
    if by_hand:
        print("copied by hand (no receipt): %s" % " ".join(by_hand))
        print("   'adopt' gives a receipt to the ones that are the catalogue's files")
    return 0


def cmd_install(a):
    cat = Catalog(a.catalog)
    t = target(a)
    todo = cat.apps if a.all else [cat.find(k) for k in a.apps]
    if not todo:
        raise Refused("which apps? (or --all)")
    print("%s -> %s" % (cat.head["source"]["name"], t.name))
    bad = 0
    for e in todo:
        try:
            install(t, cat, e, a)
        except Refused as x:
            bad += 1
            print("   %-12s REFUSED: %s" % (e["file"], x))
    return 1 if bad else 0


def cmd_adopt(a):
    cat = Catalog(a.catalog)
    t = target(a)
    recs = receipts(t)
    adopted, left = 0, []
    for e in cat.apps:
        file = e["file"]
        if file in recs or (file + ".so") not in t.list("apps"):
            continue
        if t.sha256("apps/%s.so" % file) != e["pkg"]["so_sha256"]:
            left.append(file)
            continue
        # it is this version's .so: the rest of the package is checked file by file
        pkg = fetch(cat.url(e["pkg"]["url"]), e["pkg"]["size"], e["pkg"]["sha256"])
        files, _ = open_package(pkg, e, {})
        for path in sorted(files):
            if t.sha256(path) != sha256(files[path]):
                t.write(path, files[path])      # a page or a language it lacked, or an older one
        packs = []
        for p in e.get("packs", []):
            parts = [{"name": p["file"] + (".%d" % i if i else ""), "size": part["size"], "sha256": part["sha256"]}
                     for i, part in enumerate(p["parts"])]
            if all(t.sha256("apps/" + x["name"]) == x["sha256"] for x in parts):
                packs.append({"file": p["file"], "parts": parts})
        receipt = {"schema": SCHEMA, "file": file, "id": e["id"], "name": e["name"], "ver": e["ver"],
                   "pkg_sha256": e["pkg"]["sha256"], "source": cat.source, "installed": int(time.time()),
                   "adopted": True,
                   "files": [{"path": p, "size": len(files[p]), "sha256": sha256(files[p])} for p in sorted(files)],
                   "packs": packs}
        t.write(receipt_path(file), (json.dumps(receipt, ensure_ascii=False, indent=1) + "\n").encode())
        adopted += 1
        print("   %-12s %s  adopted%s" % (file, e["ver"], ", with %d packs" % len(packs) if packs else ""))
    print("%d adopted" % adopted)
    if left:
        print("not the catalogue's version, left alone: %s" % " ".join(left))
    return 0


def cmd_remove(a):
    t = target(a)
    bad = 0
    for k in a.apps:
        try:
            remove(t, k)
        except Refused as x:
            bad += 1
            print("   %-12s %s" % (k, x))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, need_target):
        g = p.add_mutually_exclusive_group(required=need_target)
        g.add_argument("--card", help="a folder that is the card's root")
        g.add_argument("--board", help="a board or the simulator, by its portal")
        p.add_argument("--catalog", default=OFFICIAL, help="a catalogue's address or file (default: the official one)")
        p.add_argument("--fw", help="the firmware's version, for a card (0.31.0): what fw_min is checked against")

    p = sub.add_parser("list"); common(p, False); p.set_defaults(fn=cmd_list)
    p = sub.add_parser("status"); common(p, True); p.set_defaults(fn=cmd_status)
    p = sub.add_parser("install"); common(p, True)
    p.add_argument("apps", nargs="*")
    p.add_argument("--all", action="store_true")
    p.add_argument("--packs", default="need", help="need (default), all, none, or the extras by file name: a.pak,b.pak")
    p.set_defaults(fn=cmd_install)
    p = sub.add_parser("adopt"); common(p, True); p.set_defaults(fn=cmd_adopt)
    p = sub.add_parser("remove"); common(p, True)
    p.add_argument("apps", nargs="+")
    p.set_defaults(fn=cmd_remove)
    a = ap.parse_args()
    try:
        return a.fn(a)
    except Refused as e:
        print("store: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
