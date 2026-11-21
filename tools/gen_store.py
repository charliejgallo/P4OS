#!/usr/bin/env python3
"""
Makes what the Tienda reads (docs/plan/TIENDA.md): one package per app and
the catalogue that lists them.

    tools/gen_store.py check
        Every apps/<app>/store.json, store/source.json and store/packs.json
        as they are in the tree: nothing built is needed. For the CI's lint.

    tools/gen_store.py build --release v0.31.0 --out out/store
                             [--previous p4os-store.ndjson] [--built <root>]
                             [--packs-dir <dir>]... [--only app,app]
                             [--missing-apps skip] [--missing-packs skip]
        For every app with a store.json and a built .so: out/<file>.p4app and
        a line in out/p4os-store.ndjson.

    tools/gen_store.py packs --tag v0.28.0 [--repo owner/name]
        Brings store/packs.json up to date with that release's files on
        GitHub (their sizes and SHA-256, which GitHub gives: nothing is
        downloaded). For the art packs whose sources are not in this
        repository: a pack has to be in a published release BEFORE the tag
        whose catalogue names it, and a pack whose bytes did not change
        stays pointing at the release it was already in. With
        --missing-packs skip, an app whose needed pack is in no release is
        left out of the catalogue, with a warning, instead of failing.

    tools/gen_store.py fw --catalog out/p4os-store.ndjson p4os-rev1_3.bin p4os-rev3_x.bin
        Writes into the catalogue which firmware its release brings: the
        size and the SHA-256 of each image, by the profile in its name
        (p4os-<profile>.bin), as files of that same release. The board
        offers it in Settings when it is newer than the one it runs. It is
        a step of its own because the images of the two profiles are built
        by different jobs: it runs where they meet, and needs nothing of the
        tree but this file.

Parts of the system - what the system itself reads from the card and is not
an app: its translations and the portal's, the colour emoji, the radio's
firmware - are in store/system.json, and go into the catalogue as lines with
"sys": true and no package: a list of files, each one a file of a release
with its size and SHA-256 and the place on the card it goes to. A file "from"
the tree (a language pack) is published with this release under a flat name
(lang-en-_portal.lang); an "asset" (emoji.pak, c6.bin) is one a release
already has, by store/packs.json. Only the official source's are read by the
board, and only into lang/<code>/_*.lang, fonts/*.pak and firmware/*.bin.

A package, <file>.p4app, is a zip WITHOUT compression whose paths are the
card's: apps/<file>.so, the modules the app loads itself in apps/<app>/, its
portal pages in web/, its languages in lang/<code>/, and a p4app.json that
says what it is. It is made the same from the same files, byte for byte
(sorted, one fixed date), so its SHA-256 is its identity: the CI's builds are
repeatable (measured over v0.26.0..v0.28.0, 2026-11-20), and a package that
did not change keeps the version and the address it had in --previous
instead of being uploaded again. "ver" is the release it first appeared in.

What each app says of itself is in apps/<app>/store.json:

    {
      "cat": "juegos",                      one of store/source.json's
      "about": {"es": "...", "en": "..."},  one or two sentences
      "desc": {"es": "...", "en": "..."},   a few paragraphs for its page, plain
                                            text, a blank line between them
      "packs": [{"file": "x_p4.pak", "need": true},
                {"file": "x_luna_p4.pak", "need": false,
                 "name": {"es": "Mundo: Luna", "en": "World: Moon"}}],
      "shots": ["app-x.png", "land-x.jpg"], pictures of it, files of docs/img,
                                            four at most: the store gets a
                                            copy of each at its own size
      "tags": ["3d"],                       what it is that the code does not say
      "needs": ["usb-host"],                what a board may not have
      "user_files": {"es": "...", "en": "..."},   what the person brings
      "license": "Apache-2.0",              the default
      "publish": false                      built and checked, not listed
    }

The id, the name, the colours and the icon come from the app's own code and
its .so; "id", "name" and "colors" in store.json say them for a module that
declares its apps while it runs (lua, python), and "glyph" the text or
character its icon is when it has no drawing of its own.

Worked out too, and shown on the app's page: "langs" (the languages its
package brings, Spanish always) and "feat", a list of what it is and does -
"packs" (art packs), "pad" (plays with a joystick), "portal" (a page of its
own in the portal), "landscape" or "portrait" (it only lives one way), "net",
"sound", "bt" - read from its code, plus the "tags" of store.json.

A picture goes out as <file>-<n>-<hash>.jpg, a baseline JPEG no longer than
SHOT_SIDE on its long side and with both sides a multiple of 16 (the board's
decoder takes no progressive ones, and does not finish a block cut short):
the same picture keeps its name, and so its address in --previous. Making
them needs Pillow; without it the catalogue goes out with no pictures, and
says so.

"fw_min" is worked out, not written: the newest release in which any symbol
the .so needs first appeared, from the symbol tables of this repository's
tags (git show <tag>:components/aos_dynapp/aos_symbols.c). A symbol in no
tag yet is of the release being made. "fw_min" in store.json raises it by
hand, for a change of behaviour that no symbol shows.

The standard library and git only.
"""
import argparse
import base64
import glob
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

SCHEMA = 1
# Before this release there was no store to ask: every symbol it has counts
# as always there.
BASELINE = "v0.29.0"
# What the loader's own table brings, outside aos_symbols.c (build_apps.sh)
LOADER_BUILTIN = {"printf", "fprintf", "vfprintf"}
ZIP_DATE = (2026, 1, 1, 0, 0, 0)
SHOT_SIDE = 560                 # a picture's long side in the store, px
SHOTS_MAX = 4
DESC_MAX = 1200                 # an app's long description, characters in each language
TAGS = {"3d"}                   # what store.json may say in "tags"
# what the code tells about an app: a word, and what gives it away
FEATURES = (
    ("pad",   r"aos_pad\.h|aos_hal_hid_gamepad"),
    ("net",   r"aos_hal_http_|aos_hal_tcp_|aos_hal_ws_|aos_tls_"),
    ("sound", r"aos_hal_spk_|aos_hal_beep|aos_hal_tone|aos_hal_player_|aos_hal_radio_"),
    ("bt",    r"aos_hal_bt_|aos_hal_ble_"),
)
LANGS_TEXT = ("es", "en")       # what a name or a description must have


class Bad(Exception):
    pass


def warn(text):
    """Seen in the run's summary on GitHub, and plain anywhere else."""
    print(("::warning::" if os.environ.get("GITHUB_ACTIONS") else "   ") + text)


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except ValueError as e:
        raise Bad("%s: %s" % (os.path.relpath(path, ROOT), e))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True).stdout


def vkey(tag):
    """v0.29.0 -> (0, 29, 0), to compare releases."""
    return tuple(int(x) for x in re.findall(r"\d+", tag)[:3])


# ---- the manifests ---------------------------------------------------------

def text2(d, what, where):
    if not isinstance(d, dict) or any(not isinstance(d.get(l), str) or not d[l].strip() for l in LANGS_TEXT):
        raise Bad("%s: \"%s\" needs \"es\" and \"en\"" % (where, what))


def load_source():
    path = os.path.join(ROOT, "store", "source.json")
    src = read_json(path)
    for k in ("name", "by", "url", "repo"):
        if not isinstance(src.get(k), str) or not src[k]:
            raise Bad("store/source.json: \"%s\" is missing" % k)
    text2(src.get("about"), "about", "store/source.json")
    cats = src.get("cats")
    if not isinstance(cats, list) or not cats:
        raise Bad("store/source.json: \"cats\" is missing")
    for c in cats:
        if not re.fullmatch(r"[a-z]+", c.get("id", "")):
            raise Bad("store/source.json: a category's id is lower-case letters")
        text2(c.get("name"), "name", "store/source.json, category %s" % c.get("id"))
    return src


def load_packs():
    path = os.path.join(ROOT, "store", "packs.json")
    if not os.path.exists(path):
        return {}
    packs = read_json(path)
    for name, p in packs.items():
        if (not re.fullmatch(r"[A-Za-z0-9._-]+", p.get("tag", "")) or not isinstance(p.get("size"), int)
                or not re.fullmatch(r"[0-9a-f]{64}", p.get("sha256", ""))):
            raise Bad("store/packs.json: %s needs tag, size and sha256" % name)
    return packs


SYS_PATH = re.compile(r"lang/[a-z]{2}/_[a-z0-9_]+\.lang|fonts/[a-z0-9_]+\.pak|firmware/[a-z0-9_]+\.bin")
SYS_FW_MIN = "0.30.0"           # the first firmware that knows what a part of the system is


def version_key(tag):
    """v0.29.0 as something to compare: (0, 29, 0)."""
    return tuple(int(x) for x in re.findall(r"\d+", tag)[:3])


def load_system(cats):
    """store/system.json, checked: the parts of the system the store offers."""
    path = os.path.join(ROOT, "store", "system.json")
    if not os.path.exists(path):
        return []
    parts = read_json(path)
    seen = set()
    for p in parts:
        where = "store/system.json, %s" % p.get("file")
        if not re.fullmatch(r"sys-[a-z0-9-]{1,26}", p.get("file", "")) or p["file"] in seen:
            raise Bad("store/system.json: \"file\" is sys-<name>, each one once (%s)" % p.get("file"))
        seen.add(p["file"])
        if "sistema" not in cats:
            raise Bad("store/source.json has no category \"sistema\" for the parts of the system")
        for k in ("name", "about", "desc"):
            text2(p.get(k), k, where)
        if len(p["about"]["es"]) > 240 or len(p["about"]["en"]) > 240 or max(len(p["desc"][l]) for l in LANGS_TEXT) > DESC_MAX:
            raise Bad("%s: \"about\" is 240 characters at most and \"desc\" %d" % (where, DESC_MAX))
        if not glyph_of(p.get("glyph", "")):
            raise Bad("%s: \"glyph\" is the name of a symbol (AOS_SYM_...)" % where)
        files = p.get("files") or []
        if not 1 <= len(files) <= 8:
            raise Bad("%s: one to eight files" % where)
        for f in files:
            if not SYS_PATH.fullmatch(f.get("path", "")):
                raise Bad("%s: %s is not a place a part of the system may write" % (where, f.get("path")))
            if ("from" in f) == ("asset" in f):
                raise Bad("%s: %s comes \"from\" the tree or is an \"asset\" of a release, one of the two" % (where, f["path"]))
            if "from" in f and not os.path.isfile(os.path.join(ROOT, f["from"])):
                raise Bad("%s: %s is not in the tree" % (where, f["from"]))
            if "asset" in f and not re.fullmatch(r"[a-z0-9_]+\.[a-z0-9]+", f["asset"]):
                raise Bad("%s: an asset is a plain file name" % where)
    return parts


def system_entry(p, a, packs_json, old):
    """A part of the system as a line of the catalogue; None, with a warning,
    when one of its files is in no release yet. What it publishes itself is
    written to a.out."""
    files, tags, lacking = [], [], []
    was = {f["path"]: f for f in (old or {}).get("files", [])}
    for f in p["files"]:
        if "from" in f:
            data = open(os.path.join(ROOT, f["from"]), "rb").read()
            digest, name = sha256(data), f["from"].replace("/", "-")
            before = was.get(f["path"])
            if before and before["sha256"] == digest:
                url = before["url"]                 # the same bytes: where they already are
            else:
                url = "%s/%s" % (a.release, name)
                with open(os.path.join(a.out, name), "wb") as out:
                    out.write(data)
                tags.append(a.release)
            files.append({"path": f["path"], "url": url, "size": len(data), "sha256": digest})
        else:
            local = next((os.path.join(d, f["asset"]) for d in a.packs_dir if os.path.isfile(os.path.join(d, f["asset"]))), None)
            if local:
                data = open(local, "rb").read()
                files.append({"path": f["path"], "url": "%s/%s" % (a.release, f["asset"]), "size": len(data), "sha256": sha256(data)})
                tags.append(a.release)
            elif f["asset"] in packs_json:
                j = packs_json[f["asset"]]
                files.append({"path": f["path"], "url": "%s/%s" % (j["tag"], f["asset"]), "size": j["size"], "sha256": j["sha256"]})
                tags.append(j["tag"])
            else:
                lacking.append(f["asset"])
    if lacking:
        if a.missing_packs != "skip":
            raise Bad("store/system.json, %s: %s is in no release (tools/gen_store.py packs)" % (p["file"], ", ".join(lacking)))
        warn("%s is left out of the catalogue: %s is in no release yet (tools/gen_store.py packs)" % (p["file"], ", ".join(lacking)))
        return None
    # what it is, whole: the places and the bytes of its files
    digest = sha256("".join("%s:%s\n" % (f["path"], f["sha256"]) for f in files).encode())
    total = sum(f["size"] for f in files)
    entry = {
        "file": p["file"], "id": "", "sys": True, "name": p["name"], "about": p["about"], "desc": p["desc"], "cat": "sistema",
        "ver": a.release if a.release in tags or not tags else max(tags, key=version_key),
        "fw_min": SYS_FW_MIN, "pkg": {"size": total, "sha256": digest, "installed": total},
        "colors": p.get("colors", ["3A3A3C", "1C1C1E"]), "glyph": glyph_of(p["glyph"]), "files": files,
        "license": p.get("license", "Apache-2.0"),
    }
    if old and old.get("pkg", {}).get("sha256") == digest:
        entry["ver"] = old["ver"]
    return entry


def app_dirs():
    return sorted(d for d in glob.glob(os.path.join(ROOT, "apps", "*")) if os.path.isdir(os.path.join(d, "main")))


def load_manifest(app_dir, cats):
    """apps/<app>/store.json, checked; None when the app has none."""
    app = os.path.basename(app_dir)
    path = os.path.join(app_dir, "store.json")
    where = "apps/%s/store.json" % app
    if not os.path.exists(path):
        return None
    m = read_json(path)
    known = {"cat", "about", "desc", "packs", "needs", "user_files", "license", "publish", "id", "name", "colors", "glyph",
             "fw_min", "src", "shots", "tags"}
    extra = set(m) - known
    if extra:
        raise Bad("%s: unknown %s" % (where, ", ".join(sorted(extra))))
    if m.get("cat") not in cats:
        raise Bad("%s: \"cat\" is one of %s" % (where, ", ".join(sorted(cats))))
    text2(m.get("about"), "about", where)
    for l in LANGS_TEXT:
        if len(m["about"][l]) > 240:
            raise Bad("%s: \"about\" in %s is %d characters, 240 at most" % (where, l, len(m["about"][l])))
    if "desc" in m:
        text2(m["desc"], "desc", where)
        for l in LANGS_TEXT:
            if len(m["desc"][l]) > DESC_MAX:
                raise Bad("%s: \"desc\" in %s is %d characters, %d at most" % (where, l, len(m["desc"][l]), DESC_MAX))
    if "name" in m:
        text2(m["name"], "name", where)
    if "user_files" in m:
        text2(m["user_files"], "user_files", where)
    if "fw_min" in m and not re.fullmatch(r"\d+\.\d+\.\d+", m["fw_min"]):
        raise Bad("%s: \"fw_min\" is like 0.31.0" % where)
    shots = m.get("shots", [])
    if not isinstance(shots, list) or len(shots) > SHOTS_MAX:
        raise Bad("%s: \"shots\" is a list of %d pictures at most" % (where, SHOTS_MAX))
    for f in shots:
        if not re.fullmatch(r"[A-Za-z0-9_-]+\.(png|jpg)", f) or not os.path.isfile(os.path.join(ROOT, "docs", "img", f)):
            raise Bad("%s: the picture %s is not in docs/img" % (where, f))
    if set(m.get("tags", [])) - TAGS:
        raise Bad("%s: \"tags\" may hold %s" % (where, ", ".join(sorted(TAGS))))
    for p in m.get("packs", []):
        if not re.fullmatch(r"[a-z0-9_]+\.pak", p.get("file", "")) or not isinstance(p.get("need"), bool):
            raise Bad("%s: a pack needs \"file\" (x_p4.pak) and \"need\" (true or false)" % where)
        if not p["need"]:
            text2(p.get("name"), "name", "%s, pack %s" % (where, p["file"]))
    return m


# ---- what the app's code says ----------------------------------------------

def icon_names():
    """AOS_ICON_* -> its number, from the header the firmware is built with."""
    text = open(os.path.join(ROOT, "components", "aos_ui", "include", "aos_app.h"), encoding="utf-8").read()
    body = re.search(r"typedef enum \{\s*AOS_ICON_NONE = 0,(.*?)\} aos_icon_id_t;", text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    names = ["AOS_ICON_NONE"] + re.findall(r"\b(AOS_ICON_[A-Z0-9_]+)\b", body)
    return {n: i for i, n in enumerate(names)}


def abi_version():
    text = open(os.path.join(ROOT, "components", "aos_ui", "include", "aos_app.h"), encoding="utf-8").read()
    return int(re.search(r"#define AOS_ABI_VERSION\s+(\d+)", text).group(1))


def describe(app_dir):
    """id, name, colours and built-in icon as the app's init sets them."""
    out = {}
    for path in sorted(glob.glob(os.path.join(app_dir, "main", "*.c"))):
        text = open(path, encoding="utf-8", errors="replace").read()
        m = re.search(r'desc\.id\s*=\s*"([a-z0-9]+\.[a-z0-9_]+)"', text) or \
            re.search(r'^\s*\.id\s*=\s*"([a-z0-9]+\.[a-z0-9_]+)"', text, re.M)
        if not m:
            continue
        near = text[max(0, m.start() - 400):m.end() + 900]
        out["id"] = m.group(1)
        n = re.search(r'\.name\s*=\s*(?:N_\(|_\()?"([^"]+)"', near)
        if n:
            out["name"] = n.group(1)
        a = re.search(r"\.color_a\s*=\s*0x([0-9A-Fa-f]{6})", near)
        b = re.search(r"\.color_b\s*=\s*0x([0-9A-Fa-f]{6})", near)
        if a and b:
            out["colors"] = [a.group(1).upper(), b.group(1).upper()]
        v = re.search(r"\.icon_vec\s*=\s*(AOS_ICON_[A-Z0-9_]+)", near)
        if v:
            out["icon_vec"] = v.group(1)
        g = re.search(r'\.icon\s*=\s*(?:"([^"]{1,8})"|([A-Z][A-Z0-9_]+))\s*;', near)
        if g:
            out["glyph"] = g.group(1) if g.group(1) is not None else glyph_of(g.group(2))
        break
    return out


_GLYPHS = None


def glyph_of(macro):
    """AOS_SYM_CCTV or LV_SYMBOL_OK as the character it draws, or None."""
    global _GLYPHS
    if _GLYPHS is None:
        _GLYPHS = {}
        for path in (os.path.join(ROOT, "components", "aos_ui", "include", "aos_sys_glyphs.h"),
                     os.path.join(ROOT, "managed_components", "lvgl__lvgl", "src", "font", "lv_symbol_def.h")):
            if not os.path.exists(path):
                continue
            for name, lit in re.findall(r'#define\s+((?:AOS_SYM|LV_SYMBOL)_\w+)\s+"((?:\\x[0-9A-Fa-f]{2})+)"',
                                        open(path, encoding="utf-8", errors="replace").read()):
                try:
                    _GLYPHS[name] = bytes(int(h, 16) for h in re.findall(r"\\x([0-9A-Fa-f]{2})", lit)).decode()
                except UnicodeDecodeError:
                    pass
    return _GLYPHS.get(macro)


def features(app_dir):
    """What the app's own code tells: the words of FEATURES it gives away,
    and "landscape" or "portrait" when its descriptor asks for one only."""
    text = ""
    for path in sorted(glob.glob(os.path.join(app_dir, "main", "*.c")) + glob.glob(os.path.join(app_dir, "main", "*.h"))):
        text += open(path, encoding="utf-8", errors="replace").read()
    out = [name for name, what in FEATURES if re.search(what, text)]
    flags = " ".join(re.findall(r"\.flags\s*=([^;]*);", text))
    land, port = "AOS_APP_FLAG_LANDSCAPE" in flags, "AOS_APP_FLAG_PORTRAIT" in flags
    if land != port:
        out.append("landscape" if land else "portrait")
    return out


def make_shot(path):
    """A picture of docs/img as the store shows it: (jpeg bytes, w, h), or
    None without Pillow."""
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("RGB")
    w, h = im.size
    k = min(1.0, SHOT_SIDE / max(w, h))
    # Both sides a multiple of 16, the size of a block of a 4:2:0 JPEG: the
    # board's decoder leaves the columns of a block that is cut short as they
    # come, and a picture 315 wide showed three of them as stray dots. It is a
    # stretch of a few pixels at most, which nobody sees.
    fit = (max(16, round(w * k / 16) * 16), max(16, round(h * k / 16) * 16))
    if fit != (w, h):
        im = im.resize(fit, Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=82, optimize=True, progressive=False, subsampling="4:2:0")
    return buf.getvalue(), im.size[0], im.size[1]


def lang_lookup(code, lang_id, text):
    """The translation of one string in lang/<code>/<id>.lang, or None."""
    path = os.path.join(ROOT, "lang", code, lang_id + ".lang")
    if not os.path.exists(path):
        return None
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            continue
        es, tab, tr = line.rstrip("\n").partition("\t")
        if tab and es == text and tr:
            return tr
    return None


# ---- the .so ---------------------------------------------------------------

def elf_symbols(data, where):
    """(undefined globals, defined names) of a 32-bit RISC-V shared object."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise Bad("%s: not a 32-bit little-endian ELF" % where)
    machine, = struct.unpack_from("<H", data, 18)
    flags, = struct.unpack_from("<I", data, 36)
    if machine != 243:
        raise Bad("%s: not for the ESP32-P4 (machine %d)" % (where, machine))
    if flags & 0x6 != 0x2:
        raise Bad("%s: not the firmware's float ABI (ilp32f)" % where)
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    secs = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    dyn = next((s for s in secs if s[1] == 11), None)
    if not dyn:
        raise Bad("%s: no dynamic symbols" % where)
    stroff, strsz = secs[dyn[6]][4], secs[dyn[6]][5]
    strtab = data[stroff:stroff + strsz]
    undef, defined = set(), set()
    for i in range(1, dyn[5] // 16):
        name, _value, _size, info, _other, shndx = struct.unpack_from("<IIIBBH", data, dyn[4] + i * 16)
        s = strtab[name:strtab.index(b"\0", name)].decode()
        if not s:
            continue
        if shndx:
            defined.add(s)
        elif info >> 4 == 1:
            undef.add(s)
    return undef, defined


def find_icon(data):
    """The app's AIC blob inside its .so, or None: the first "AIC" header
    that reads as a whole icon (tools/aic.py's parser, the board's rules)."""
    import aic
    cap = 512                       # what the scan cache keeps (aos_dynapp.c)
    old, aic.MAX_BYTES = aic.MAX_BYTES, cap
    try:
        at = data.find(b"AIC\x01")
        while at >= 0:
            window = data[at:at + cap]
            try:
                end = None
                for off, name, _fields in aic.parse(window):
                    if name == "END":
                        end = off + 1
                if end:
                    return window[:end]
            except aic.Bad:
                pass
            at = data.find(b"AIC\x01", at + 1)
    finally:
        aic.MAX_BYTES = old
    return None


class Since:
    """In which release each symbol first appeared, from the tags' tables."""

    def __init__(self, release):
        self.release = release
        self.first = {}
        tags = [t for t in git("tag", "--list", "v*", "--sort=version:refname").split()
                if re.fullmatch(r"v\d+\.\d+\.\d+", t) and vkey(t) >= vkey(BASELINE)]
        for tag in tags:
            if vkey(tag) >= vkey(release):
                continue            # the one being made is read from the tree
            text = git("show", "%s:components/aos_dynapp/aos_symbols.c" % tag)
            for s in re.findall(r"ESP_ELFSYM_EXPORT\((\w+)\)", text):
                self.first.setdefault(s, BASELINE if tag == BASELINE else tag)
        tree = open(os.path.join(ROOT, "components", "aos_dynapp", "aos_symbols.c"), encoding="utf-8").read()
        self.now = set(re.findall(r"ESP_ELFSYM_EXPORT\((\w+)\)", tree))
        if not self.first:
            # no tag to read (a shallow checkout): what is here is the baseline
            self.first = {s: BASELINE for s in self.now}

    def fw_min(self, undef, where):
        need = BASELINE
        for s in sorted(undef - LOADER_BUILTIN):
            if s not in self.now:
                raise Bad("%s needs %s, which the firmware does not lend (tools/gen_symbols.py)" % (where, s))
            t = self.first.get(s, self.release)
            if vkey(t) > vkey(need):
                need = t
        return need.lstrip("v")


# ---- a package -------------------------------------------------------------

def make_zip(files):
    """{card path: bytes} -> the bytes of a stored zip, always the same."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for path in sorted(files, key=lambda p: (p != "p4app.json", p)):
            zi = zipfile.ZipInfo(path, ZIP_DATE)
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o644 << 16
            zi.create_system = 3
            z.writestr(zi, files[path])
    return buf.getvalue()


def build_app(app_dir, m, a, since, icons, abi):
    app = os.path.basename(app_dir)
    built = os.path.join(a.built or ROOT, "apps", app, "build")
    sos = sorted(glob.glob(os.path.join(built, "*.so")))
    if len(sos) != 1:
        raise Bad("apps/%s: %d .so in its build folder (tools/build_apps.sh %s)" % (app, len(sos), app))
    file = os.path.basename(sos[0])[:-3]
    so = open(sos[0], "rb").read()
    undef, defined = elf_symbols(so, file + ".so")
    if not {"aos_app_abi", "aos_app_init"} <= defined:
        raise Bad("%s.so does not export aos_app_abi/aos_app_init" % file)

    d = describe(app_dir)
    app_id = m.get("id") or d.get("id")
    if not app_id:
        raise Bad("apps/%s: no id found in its code; say it in store.json" % app)
    name = m.get("name")
    if not name:
        if "name" not in d:
            raise Bad("apps/%s: no name found in its code; say it in store.json" % app)
        name = {"es": d["name"]}
        for code in sorted(os.listdir(os.path.join(ROOT, "lang"))):
            if os.path.isdir(os.path.join(ROOT, "lang", code)) and code != "xx":
                name[code] = lang_lookup(code, app_id, d["name"]) or d["name"]
    colors = m.get("colors") or d.get("colors") or ["3A3A3C", "1C1C1E"]

    files = {"apps/%s.so" % file: so}
    fw_need = [since.fw_min(undef, file + ".so")]
    for mod in sorted(glob.glob(os.path.join(built, "modules", "*.so"))):
        data = open(mod, "rb").read()
        mu, _ = elf_symbols(data, os.path.basename(mod))
        fw_need.append(since.fw_min(mu, os.path.basename(mod)))
        files["apps/%s/%s" % (app, os.path.basename(mod))] = data
    pages = []
    for page in sorted(glob.glob(os.path.join(app_dir, "web", "*.js")) + glob.glob(os.path.join(app_dir, "web", "*.mjs"))):
        files["web/" + os.path.basename(page)] = open(page, "rb").read()
        pages.append(os.path.splitext(os.path.basename(page))[0])
    for code in sorted(os.listdir(os.path.join(ROOT, "lang"))):
        for lang_id in [app_id] + ["web." + p for p in pages]:
            path = os.path.join(ROOT, "lang", code, lang_id + ".lang")
            if os.path.isfile(path):
                files["lang/%s/%s.lang" % (code, lang_id)] = open(path, "rb").read()
    if m.get("fw_min"):
        fw_need.append(m["fw_min"])
    fw_min = max(fw_need, key=vkey)

    inside = {
        "schema": SCHEMA, "file": file, "id": app_id, "name": name, "abi": abi, "fw_min": fw_min,
        "files": [{"path": p, "size": len(files[p]), "sha256": sha256(files[p])} for p in sorted(files)],
    }
    files["p4app.json"] = (json.dumps(inside, ensure_ascii=False, indent=1) + "\n").encode()
    pkg = make_zip(files)

    entry = {
        "id": app_id, "file": file, "name": name, "about": m["about"], "cat": m["cat"],
        "ver": a.release, "abi": abi, "fw_min": fw_min,
        "pkg": {"url": "%s/%s.p4app" % (a.release, file), "size": len(pkg), "sha256": sha256(pkg),
                "so_sha256": sha256(so), "installed": sum(len(v) for k, v in files.items() if k != "p4app.json")},
        "colors": colors,
    }
    icon = find_icon(so)
    if icon:
        entry["icon"] = base64.b64encode(icon).decode()
    elif d.get("icon_vec") and icons.get(d["icon_vec"]):
        entry["icon_vec"] = icons[d["icon_vec"]]
    elif m.get("glyph") or d.get("glyph"):
        # a character of the system's fonts over the colours, as the launcher draws it
        entry["glyph"] = m.get("glyph") or d["glyph"]
    # "desc" is of the catalogue and not of the package: rewording it must not
    # make a new package of an app whose bytes did not change
    for k in ("needs", "user_files", "desc"):
        if m.get(k):
            entry[k] = m[k]
    entry["langs"] = ["es"] + sorted({p.split("/")[1] for p in files if p.startswith("lang/")})
    feat = list(m.get("tags", [])) + (["packs"] if m.get("packs") else []) + features(app_dir) + (["portal"] if pages else [])
    if feat:
        entry["feat"] = feat
    shots = []
    for n, f in enumerate(m.get("shots", []), 1):
        made = make_shot(os.path.join(ROOT, "docs", "img", f))
        if made is None:
            a.no_pillow = True
            break
        data, w, h = made
        name = "%s-%d-%s.jpg" % (file, n, sha256(data)[:8])
        shots.append({"name": name, "data": data, "w": w, "h": h})
    entry["_shots"] = shots
    entry["license"] = m.get("license", "Apache-2.0")
    entry["src"] = m.get("src", "%s/tree/main/apps/%s" % (a.source["url"], app))
    return file, pkg, entry


def pack_entry(p, a, packs_json, where):
    """One pack of an app, with its parts: from a file built here, or from
    store/packs.json (a release that already has it)."""
    parts = []
    for i in range(100):
        name = p["file"] + (".%d" % i if i else "")
        local = next((os.path.join(d, name) for d in a.packs_dir if os.path.isfile(os.path.join(d, name))), None)
        if local:
            h = hashlib.sha256()
            size = 0
            with open(local, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
                    size += len(block)
            parts.append({"name": name, "url": "%s/%s" % (a.release, name), "size": size, "sha256": h.hexdigest()})
        elif name in packs_json:
            j = packs_json[name]
            parts.append({"name": name, "url": "%s/%s" % (j["tag"], name), "size": j["size"], "sha256": j["sha256"]})
        else:
            break
    if not parts:
        if a.missing_packs == "skip":
            return None
        raise Bad("%s: pack %s is nowhere: build it and pass --packs-dir, or tools/gen_store.py packs" %
                  (where, p["file"]))
    out = {"file": p["file"], "need": p["need"], "size": sum(x["size"] for x in parts),
           "parts": [{"url": x["url"], "size": x["size"], "sha256": x["sha256"]} for x in parts]}
    if "name" in p:
        out["name"] = p["name"]
    return out


# ---- the commands ----------------------------------------------------------

def cmd_check(a):
    src = load_source()
    cats = {c["id"] for c in src["cats"]}
    packs = load_packs()
    seen, n, hidden, without = {}, 0, 0, []
    for d in app_dirs():
        app = os.path.basename(d)
        m = load_manifest(d, cats)
        if m is None:
            without.append(app)
            continue
        n += 1
        hidden += m.get("publish") is False
        app_id = m.get("id") or describe(d).get("id")
        if not app_id:
            raise Bad("apps/%s: no id found in its code; say it in store.json" % app)
        if app_id in seen:
            raise Bad("apps/%s and apps/%s are both %s" % (seen[app_id], app, app_id))
        seen[app_id] = app
    parts = load_system(cats)
    print("%d apps with a store.json (%d not published), %d packs in store/packs.json, %d parts of the system" %
          (n, hidden, len(packs), len(parts)))
    if without:
        print("without one, so not in the store: %s" % " ".join(without))
    return 0


def cmd_build(a):
    if not re.fullmatch(r"v\d+\.\d+\.\d+(-[A-Za-z0-9.]+)?", a.release):
        raise Bad("--release is a tag: v0.31.0")
    a.source = load_source()
    cats = {c["id"] for c in a.source["cats"]}
    packs_json = load_packs()
    previous = {}
    if a.previous and os.path.exists(a.previous):
        for line in open(a.previous, encoding="utf-8").read().splitlines()[1:]:
            e = json.loads(line)
            previous[e["file"]] = e
    since = Since(a.release)
    a.no_pillow = False
    icons = icon_names()
    abi = abi_version()
    only = set(a.only.split(",")) if a.only else None
    os.makedirs(a.out, exist_ok=True)

    lines, kept, new = [], 0, 0
    files_seen, ids_seen = {}, {}
    for d in app_dirs():
        app = os.path.basename(d)
        if only and app not in only:
            continue
        m = load_manifest(d, cats)
        if m is None or m.get("publish") is False:
            continue
        built = os.path.join(a.built or ROOT, "apps", app, "build")
        if not glob.glob(os.path.join(built, "*.so")):
            if a.missing_apps == "skip":
                print("   apps/%s is not built: left out" % app)
                continue
            raise Bad("apps/%s is not built (tools/build_apps.sh %s)" % (app, app))
        file, pkg, entry = build_app(d, m, a, since, icons, abi)
        if file in files_seen or entry["id"] in ids_seen:
            raise Bad("apps/%s: its file or id is also apps/%s's" % (app, files_seen.get(file) or ids_seen[entry["id"]]))
        files_seen[file] = ids_seen[entry["id"]] = app
        packs, lacking = [], []
        for p in m.get("packs", []):
            e = pack_entry(p, a, packs_json, "apps/" + app)
            if e:
                packs.append(e)
            else:
                lacking.append(p)
        if any(p["need"] for p in lacking):
            # without it the app would install and not start: better not listed
            warn("apps/%s is left out of the catalogue: its pack %s is in no release yet (tools/gen_store.py packs)"
                 % (app, ", ".join(p["file"] for p in lacking if p["need"])))
            continue
        for p in lacking:
            warn("apps/%s: its extra %s is in no release yet: not offered" % (app, p["file"]))
        if packs:
            entry["packs"] = packs
        old = previous.get(file)
        was = {u["url"].rsplit("/", 1)[-1]: u["url"] for u in (old or {}).get("shots", [])}
        pics = []
        for sh in entry.pop("_shots"):
            if sh["name"] in was:
                url = was[sh["name"]]           # the same picture: where it already is
            else:
                url = "%s/%s" % (a.release, sh["name"])
                with open(os.path.join(a.out, sh["name"]), "wb") as f:
                    f.write(sh["data"])
            pics.append({"url": url, "w": sh["w"], "h": sh["h"]})
        if pics:
            entry["shots"] = pics
        if old and old["pkg"]["sha256"] == entry["pkg"]["sha256"]:
            # the same package as the last release's: it stays where it is
            entry["ver"] = old["ver"]
            entry["pkg"]["url"] = old["pkg"]["url"]
            kept += 1
        else:
            with open(os.path.join(a.out, file + ".p4app"), "wb") as f:
                f.write(pkg)
            new += 1
        lines.append(entry)

    # the parts of the system, with a whole catalogue: a few apps picked by
    # hand (--only) are a test's
    nsys = 0
    for p in ([] if only else load_system(cats)):
        entry = system_entry(p, a, packs_json, previous.get(p["file"]))
        if entry:
            lines.append(entry)
            nsys += 1
    head = {"schema": SCHEMA, "source": {k: a.source[k] for k in ("name", "by", "url", "about")},
            "cats": a.source["cats"], "release": a.release,
            "base": a.base or "https://github.com/%s/releases/download/" % (a.repo or a.source["repo"])}
    # in the order the source lists its categories: what "everything" shows first
    order = {c["id"]: i for i, c in enumerate(a.source["cats"])}
    lines.sort(key=lambda e: (order[e["cat"]], e["name"]["es"].lower()))
    path = os.path.join(a.out, "p4os-store.ndjson")
    with open(path, "w", encoding="utf-8") as f:
        for o in [head] + lines:
            f.write(json.dumps(o, ensure_ascii=False, separators=(",", ":")) + "\n")
    if a.no_pillow:
        warn("no Pillow here (pip install pillow): the catalogue goes out without pictures")
    total = sum(e["pkg"]["size"] for e in lines if not e.get("sys"))
    print("%d apps in %s (%d bytes): %d packages written, %d as in the previous release; %.1f MB of packages, "
          "%.1f MB of packs; %d parts of the system" % (len(lines) - nsys, path, os.path.getsize(path), new, kept, total / 1e6,
                                sum(p["size"] for e in lines for p in e.get("packs", [])) / 1e6, nsys))
    return 0


def cmd_packs(a):
    src = load_source()
    out = subprocess.run(["gh", "release", "view", a.tag, "--repo", a.repo or src["repo"], "--json", "assets"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise Bad("gh: %s" % out.stderr.strip())
    packs = {} if a.fresh else load_packs()
    # and what the parts of the system name: a firmware for the radio is no .pak
    wanted = {f["asset"] for p in load_system({c["id"] for c in src["cats"]}) for f in p["files"] if "asset" in f}
    n = 0
    for asset in json.loads(out.stdout)["assets"]:
        if not re.fullmatch(r"[a-z0-9_]+\.pak(\.\d+)?", asset["name"]) and asset["name"] not in wanted:
            continue
        digest = asset.get("digest") or ""
        if not digest.startswith("sha256:"):
            raise Bad("%s has no SHA-256 on GitHub" % asset["name"])
        new = {"tag": a.tag, "size": asset["size"], "sha256": digest[7:]}
        old = packs.get(asset["name"])
        # the same bytes in an older release: leave it pointing there
        if not old or old["sha256"] != new["sha256"]:
            packs[asset["name"]] = new
            n += 1
    path = os.path.join(ROOT, "store", "packs.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dict(sorted(packs.items())), f, indent=1)
        f.write("\n")
    print("%d packs in store/packs.json, %d new or changed in %s" % (len(packs), n, a.tag))
    return 0


def cmd_fw(a):
    lines = open(a.catalog, encoding="utf-8").read().splitlines()
    try:
        head = json.loads(lines[0])
    except (IndexError, ValueError):
        raise Bad("%s: not a catalogue" % a.catalog)
    if "source" not in head or not head.get("release"):
        raise Bad("%s: its first line is not a source's" % a.catalog)
    images = {}
    for path in a.images:
        m = re.fullmatch(r"p4os-([a-z0-9_]+)\.bin", os.path.basename(path))
        if not m:
            raise Bad("%s: an image is named p4os-<profile>.bin" % path)
        data = open(path, "rb").read()
        if len(data) < 1024 * 1024 or data[0] != 0xE9:
            raise Bad("%s: not a firmware image" % path)
        images[m.group(1)] = {"url": "%s/%s" % (head["release"], os.path.basename(path)), "size": len(data),
                              "sha256": sha256(data)}
    head["fw"] = {"ver": head["release"], "images": images}
    lines[0] = json.dumps(head, ensure_ascii=False, separators=(",", ":"))
    with open(a.catalog, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("the firmware of %s in the catalogue: %s" % (head["release"], ", ".join(
        "%s (%.1f MB)" % (k, v["size"] / 1e6) for k, v in sorted(images.items()))))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check").set_defaults(fn=cmd_check)
    b = sub.add_parser("build")
    b.add_argument("--release", required=True, help="the tag being made: v0.31.0")
    b.add_argument("--out", required=True)
    b.add_argument("--previous", help="the last published catalogue, to keep what did not change")
    b.add_argument("--built", help="where apps/<app>/build is, when not in this tree")
    b.add_argument("--packs-dir", action="append", default=[], help="a folder with packs built for this release")
    b.add_argument("--only", help="just these apps (folders of apps/, comma separated)")
    b.add_argument("--repo", help="owner/name, when not store/source.json's")
    b.add_argument("--base", help="where the releases' files are, when not GitHub (a test server): "
                                  ".../releases/download/")
    b.add_argument("--missing-apps", choices=("fail", "skip"), default="fail")
    b.add_argument("--missing-packs", choices=("fail", "skip"), default="fail")
    b.set_defaults(fn=cmd_build)
    p = sub.add_parser("packs")
    p.add_argument("--tag", required=True)
    p.add_argument("--repo")
    p.add_argument("--fresh", action="store_true", help="forget what store/packs.json had")
    p.set_defaults(fn=cmd_packs)
    f = sub.add_parser("fw")
    f.add_argument("--catalog", required=True, help="the p4os-store.ndjson to write it into")
    f.add_argument("images", nargs="+", help="p4os-<profile>.bin, as published in the release")
    f.set_defaults(fn=cmd_fw)
    a = ap.parse_args()
    try:
        return a.fn(a)
    except Bad as e:
        print("gen_store: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
