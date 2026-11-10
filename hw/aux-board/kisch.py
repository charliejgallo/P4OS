"""Writes a KiCad schematic from a list of parts and the net on each pin.

No wires are drawn: every pin gets a net label at its end, which is how
KiCad connects pins that share a name. The symbols come from KiCad's own
libraries, copied into the file as KiCad itself does, so the result opens
and passes ERC with nothing else installed.

A part is Part(ref, "Lib:Symbol", value, footprint, pins), where pins maps a
pin's number or name to its net; a pin left out, or mapped to None, gets a
no-connect mark. gen.py holds the board's parts; this file knows nothing
about them.
"""
import os
import re
import uuid

KICAD = "/Applications/KiCad/KiCad.app/Contents/SharedSupport"
SYMBOLS = os.environ.get("KICAD_SYMBOL_DIR", KICAD + "/symbols")
FOOTPRINTS = os.environ.get("KICAD_FOOTPRINT_DIR", KICAD + "/footprints")
NS = uuid.UUID("5a9e1c0e-7b1f-4c64-9d0b-0f4b6f1e2a10")     # ids are derived from names: the file is stable


class Q(str):
    """A string that was quoted in the file, and is written quoted."""


def parse(text):
    """An s-expression as nested lists; quoted strings are Q, the rest plain str."""
    out, stack, i, n = None, [], 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "(":
            new = []
            if stack:
                stack[-1].append(new)
            stack.append(new)
            i += 1
        elif c == ")":
            out = stack.pop()
            i += 1
        elif c == '"':
            j, buf = i + 1, []
            while text[j] != '"':
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            stack[-1].append(Q("".join(buf)))
            i = j + 1
        else:
            j = i
            while text[j] not in ' \t\r\n()"':
                j += 1
            stack[-1].append(text[i:j])
            i = j
    return out


def dump(x, depth=0):
    if not isinstance(x, list):
        return '"%s"' % x if isinstance(x, Q) else str(x)
    flat = all(not isinstance(e, list) for e in x)
    if flat:
        return "(" + " ".join(dump(e) for e in x) + ")"
    pad = "\t" * (depth + 1)
    head = [dump(e) for e in x if not isinstance(e, list)]
    body = [pad + dump(e, depth + 1) for e in x if isinstance(e, list)]
    return "(" + " ".join(head) + "\n" + "\n".join(body) + "\n" + "\t" * depth + ")"


_lib_text = {}


def lib_symbol(lib, name):
    """The (symbol ...) of a library, as a tree, its parent merged in if it extends one."""
    if lib not in _lib_text:
        with open(os.path.join(SYMBOLS, lib + ".kicad_sym"), encoding="utf-8") as f:
            _lib_text[lib] = f.read()
    text = _lib_text[lib]
    start = text.find('\n\t(symbol "%s"\n' % name)
    if start < 0:
        raise KeyError("%s:%s is not in KiCad's libraries" % (lib, name))
    depth, i = 0, start + 2
    while True:
        if text[i] == '"':
            i = text.index('"', i + 1)
            while text[i - 1] == "\\":
                i = text.index('"', i + 1)
        elif text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    sym = parse(text[start + 2:i + 1])
    ext = [e for e in sym if isinstance(e, list) and e[0] == "extends"]
    if ext:
        parent = lib_symbol(lib, ext[0][1])
        pname = parent[1]
        mine = {e[1]: e for e in sym if isinstance(e, list) and e[0] == "property"}
        merged = ["symbol", Q(name)]
        for e in parent[2:]:
            if isinstance(e, list) and e[0] == "property":
                merged.append(mine.pop(e[1], e))
            elif isinstance(e, list) and e[0] == "symbol":
                unit = [e[0], Q(name + e[1][len(pname):])] + e[2:]
                merged.append(unit)
            else:
                merged.append(e)
        merged[2:2] = []                # the child's own extra properties go after the parent's
        idx = max(k for k, e in enumerate(merged) if isinstance(e, list) and e[0] == "property") + 1
        merged[idx:idx] = list(mine.values())
        sym = merged
    return sym


def symbol_pins(sym):
    """[(number, name, type, x, y, angle)] of unit 1 (and the shared unit 0)."""
    pins = []
    for e in sym:
        if isinstance(e, list) and e[0] == "symbol" and re.search(r"_[01]_\d+$", e[1]):
            for p in e:
                if isinstance(p, list) and p[0] == "pin":
                    at = next(a for a in p if isinstance(a, list) and a[0] == "at")
                    name = next(a for a in p if isinstance(a, list) and a[0] == "name")[1]
                    num = next(a for a in p if isinstance(a, list) and a[0] == "number")[1]
                    pins.append((str(num), str(name), p[1], float(at[1]), float(at[2]), int(float(at[3]))))
    return pins


def uid(*parts):
    return Q(str(uuid.uuid5(NS, "/".join(str(p) for p in parts))))


def font(size=1.27):
    return ["effects", ["font", ["size", size, size]]]


class Part:
    def __init__(self, ref, sym, value, footprint, pins, dnp=False, fields=None):
        self.ref, self.sym, self.value, self.footprint = ref, sym, value, footprint
        self.pins, self.dnp, self.fields = pins, dnp, fields or {}


class Sheet:
    def __init__(self, project, title, rev, paper="A2"):
        self.project, self.title, self.rev, self.paper = project, title, rev, paper
        self.root = uid(project, "root")
        self.libs, self.items, self.refs = {}, [], set()

    def text(self, x, y, s, size=2.54):
        self.items.append(["text", Q(s), ["exclude_from_sim", "no"], ["at", x, y, 0],
                           ["effects", ["font", ["size", size, size]], ["justify", "left", "bottom"]],
                           ["uuid", uid(self.project, "text", s, x, y)]])

    def place(self, part, x, y):
        """Puts the part with its origin at (x, y) mm and a label on every pin."""
        if part.ref in self.refs:
            raise ValueError("two parts called " + part.ref)
        self.refs.add(part.ref)
        lib, name = part.sym.split(":")
        if part.sym not in self.libs:
            sym = lib_symbol(lib, name)
            self.libs[part.sym] = ["symbol", Q(part.sym)] + sym[2:]
        sym = self.libs[part.sym]
        if part.footprint:
            flib, fname = part.footprint.split(":")
            if not os.path.exists(os.path.join(FOOTPRINTS, flib + ".pretty", fname + ".kicad_mod")):
                raise KeyError("no footprint " + part.footprint)
        pins = symbol_pins(sym)
        by_num = {p[0]: p for p in pins}
        wanted = {}
        for key, net in part.pins.items():
            key = str(key)
            if key in by_num:
                wanted[key] = net
            else:
                hits = [p[0] for p in pins if p[1] == key]
                if not hits:
                    raise KeyError("%s (%s) has no pin %s; it has %s" % (part.ref, part.sym, key, [(p[0], p[1]) for p in pins]))
                for h in hits:
                    wanted[h] = net
        xs, ys = [p[3] for p in pins] or [0], [p[4] for p in pins] or [0]
        if min(xs) == max(xs) == 0:         # a two-pin part standing up: its names go beside it
            rx, ry, vx, vy = x + 2.54, y - 1.27, x + 2.54, y + 1.27
        else:                               # above and below the body, from its left edge
            rx, ry, vx, vy = x + min(xs), y - max(ys) - 3.81, x + min(xs), y - min(ys) + 5.08
        props = [("Reference", part.ref, rx, ry, False), ("Value", part.value, vx, vy, False),
                 ("Footprint", part.footprint or "", x, y, True), ("Datasheet", "", x, y, True),
                 ("Description", "", x, y, True)] + [(k, v, x, y, True) for k, v in part.fields.items()]
        inst = ["symbol", ["lib_id", Q(part.sym)], ["at", x, y, 0], ["unit", 1],
                ["exclude_from_sim", "no"], ["in_bom", "yes"], ["on_board", "yes"],
                ["dnp", "yes" if part.dnp else "no"], ["uuid", uid(self.project, part.ref)]]
        for k, v, px, py, hide in props:
            p = ["property", Q(k), Q(v), ["at", round(px, 2), round(py, 2), 0]]
            if hide:
                p.append(["hide", "yes"])
            p.append(["effects", ["font", ["size", 1.27, 1.27]], ["justify", "left"]])
            inst.append(p)
        for p in pins:
            inst.append(["pin", Q(p[0]), ["uuid", uid(self.project, part.ref, "pin", p[0])]])
        inst.append(["instances", ["project", Q(self.project),
                                   ["path", Q("/" + self.root), ["reference", Q(part.ref)], ["unit", 1]]]])
        self.items.append(inst)
        for num, pname, ptype, px, py, ang in pins:
            ax, ay = round(x + px, 2), round(y - py, 2)
            net = wanted.get(num)
            if net is None:
                self.items.append(["no_connect", ["at", ax, ay], ["uuid", uid(self.project, part.ref, "nc", num)]])
                continue
            # the label leaves the pin's end, away from the body
            rot, just = {0: (180, "right"), 180: (0, "left"), 90: (270, "right"), 270: (90, "left")}[ang]
            self.items.append(["label", Q(net), ["at", ax, ay, rot],
                               ["effects", ["font", ["size", 1.27, 1.27]], ["justify", just, "bottom"]],
                               ["uuid", uid(self.project, part.ref, "label", num)]])

    def write(self, path, date):
        doc = ["kicad_sch", ["version", 20250610], ["generator", Q("p4os_aux_board")], ["generator_version", Q("1")],
               ["uuid", self.root], ["paper", Q(self.paper)],
               ["title_block", ["title", Q(self.title)], ["date", Q(date)], ["rev", Q(self.rev)],
                ["company", Q("P4OS")]],
               ["lib_symbols"] + list(self.libs.values())] + self.items + [
               ["sheet_instances", ["path", Q("/"), ["page", Q("1")]]], ["embedded_fonts", "no"]]
        with open(path, "w", encoding="utf-8") as f:
            f.write(dump(doc) + "\n")
