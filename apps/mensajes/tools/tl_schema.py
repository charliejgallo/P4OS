#!/usr/bin/env python3
"""
P4OS - Mensajes: the TL schema, read and written.

One reader of the schema for everything that speaks TL in this app: the
generator of the C tables (tl_gen.py), the tool that cuts the subset out of
the published schema (tl_subset.py) and the fake server
(tools/fake_telegram.py), which serialises and parses with the same
descriptions the board does. That way the two ends cannot disagree on a
field.

A line of the schema is

    name#crc32 field:type field:flags.N?type ... = Type;

with the types int, long, double, string, bytes, int128, int256, true
(a flag that is only its bit), # (a flags word), Bool, Vector<T> (boxed),
vector<T> or %T (bare), !X (any object, the query of invokeWithLayer) and
the boxed types of the schema itself. Constructors and functions share the
syntax; "---functions---" and "---types---" switch between them.

The codec at the bottom (TLCodec) turns bytes into dicts and back:
{'_': 'message', 'id': 5, 'message': 'hola', ...}. Absent optional fields
are absent keys; a `true` flag is True or absent.
"""

import re
import struct
import zlib

PRIMS = {'int', 'long', 'double', 'string', 'bytes', 'int128', 'int256', 'true', '#', 'Bool'}


class Field:
    __slots__ = ('name', 'type', 'flag_field', 'flag_bit')

    def __init__(self, name, type_, flag_field=None, flag_bit=None):
        self.name = name
        self.type = type_            # the type text without the condition: "Vector<Peer>"
        self.flag_field = flag_field  # "flags" / "flags2" or None
        self.flag_bit = flag_bit

    def __repr__(self):
        c = f'{self.flag_field}.{self.flag_bit}?' if self.flag_field else ''
        return f'{self.name}:{c}{self.type}'


class Combinator:
    __slots__ = ('name', 'id', 'fields', 'result', 'is_func', 'line', 'generic')

    def __init__(self, name, id_, fields, result, is_func, line, generic):
        self.name = name
        self.id = id_
        self.fields = fields
        self.result = result
        self.is_func = is_func
        self.line = line
        self.generic = generic        # the {X:Type} parameters' names

    def __repr__(self):
        return f'{self.name}#{self.id:08x}'


_LINE = re.compile(r'^([A-Za-z0-9_.]+)(?:#([0-9a-fA-F]+))?\s*(.*?)\s*=\s*([^;]+);\s*$')


def parse(text):
    """Every combinator of a .tl text, in order. Lines without an id (the
    core types of mtproto.tl, or the ones marked "parsed manually") are
    skipped: the C code and the fake handle those by hand."""
    out = []
    is_func = False
    for raw in text.splitlines():
        line = raw.split('//', 1)[0].strip()
        if not line:
            continue
        if line == '---functions---':
            is_func = True
            continue
        if line == '---types---':
            is_func = False
            continue
        m = _LINE.match(line)
        if not m:
            continue
        name, hid, params, result = m.group(1), m.group(2), m.group(3), m.group(4).strip()
        if hid is None:
            continue
        if name in ('vector', 'int', 'long', 'double', 'string', 'int128', 'int256'):
            continue
        fields = []
        generic = []
        for p in params.split():
            if p.startswith('{'):
                generic.append(p.strip('{}').split(':')[0])
                continue
            if p == '#' or p.startswith('['):
                continue
            fname, ftype = p.split(':', 1)
            ff = fb = None
            mm = re.match(r'^([a-z0-9_]+)\.(\d+)\?(.+)$', ftype)
            if mm:
                ff, fb, ftype = mm.group(1), int(mm.group(2)), mm.group(3)
            fields.append(Field(fname, ftype, ff, fb))
        out.append(Combinator(name, int(hid, 16), fields, result, is_func, line, generic))
    return out


def inner_types(t):
    """The schema types a field type refers to: Vector<Peer> -> ['Peer']."""
    t = t.lstrip('!%')
    m = re.match(r'^[Vv]ector<(.+)>$', t)
    if m:
        return inner_types(m.group(1))
    if t in PRIMS:
        return []
    return [t]


class Schema:
    def __init__(self, combinators):
        self.all = combinators
        self.by_name = {}
        self.by_id = {}
        self.ctors_of = {}            # boxed type -> its constructors
        for c in combinators:
            self.by_name[c.name] = c
            self.by_id[c.id] = c
            if not c.is_func:
                self.ctors_of.setdefault(c.result, []).append(c)

    @classmethod
    def load(cls, *paths):
        combs = []
        for p in paths:
            with open(p, encoding='utf-8') as f:
                combs += parse(f.read())
        return cls(combs)

    def closure(self, wanted):
        """The functions named in 'wanted' plus every constructor that can
        appear in what they take or answer, transitively: what a reader
        needs to walk any of their answers without meeting an unknown id."""
        types = set()
        keep = []
        seen = set()
        todo = []

        def add_type(t):
            for it in inner_types(t):
                if it in self.ctors_of and it not in types:
                    types.add(it)
                    todo.extend(self.ctors_of[it])
                elif it not in self.ctors_of and it in self.by_name and not self.by_name[it].is_func:
                    # a bare constructor named as a type: %Message, vector<future_salt>
                    todo.append(self.by_name[it])

        for w in wanted:
            c = self.by_name[w]
            todo.append(c)
        while todo:
            c = todo.pop()
            if c.name in seen:
                continue
            seen.add(c.name)
            keep.append(c)
            for f in c.fields:
                add_type(f.type)
            if c.is_func:
                add_type(c.result)
        order = {c.name: i for i, c in enumerate(self.all)}
        keep.sort(key=lambda c: order[c.name])
        return keep


# ---------------------------------------------------------------------------
# The codec: bytes <-> dicts, for the fake server and the tests.

class TLError(Exception):
    pass


class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def take(self, n):
        if self.p + n > len(self.d):
            raise TLError('short read')
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def int(self):
        return struct.unpack('<i', self.take(4))[0]

    def uint(self):
        return struct.unpack('<I', self.take(4))[0]

    def long(self):
        return struct.unpack('<q', self.take(8))[0]

    def double(self):
        return struct.unpack('<d', self.take(8))[0]

    def bytes(self):
        n = self.take(1)[0]
        if n == 254:
            n = int.from_bytes(self.take(3), 'little')
            body = self.take(n)
            pad = (-n) % 4
        else:
            body = self.take(n)
            pad = (-(n + 1)) % 4
        self.take(pad)
        return body


def write_bytes(b):
    if isinstance(b, str):
        b = b.encode('utf-8')
    n = len(b)
    if n < 254:
        out = bytes([n]) + b
    else:
        out = bytes([254]) + n.to_bytes(3, 'little') + b
    return out + b'\0' * ((-len(out)) % 4)


VECTOR_ID = 0x1cb5c415
BOOL_TRUE = 0x997275b5
BOOL_FALSE = 0xbc799737
GZIP_PACKED = 0x3072cfa1


class TLCodec:
    def __init__(self, schema):
        self.s = schema

    # -- writing
    def encode(self, obj):
        """A dict with '_' (the constructor's or the function's name) to its
        boxed bytes."""
        c = self.s.by_name[obj['_']]
        return struct.pack('<I', c.id) + self._fields(c, obj)

    def _fields(self, c, obj):
        out = b''
        flags = {}
        for f in c.fields:
            if f.flag_field:
                present = f.name in obj and obj[f.name] is not None and obj[f.name] is not False
                if present:
                    flags[f.flag_field] = flags.get(f.flag_field, 0) | (1 << f.flag_bit)
        for f in c.fields:
            if f.type == '#':
                out += struct.pack('<I', flags.get(f.name, 0) | obj.get(f.name, 0))
                continue
            if f.flag_field:
                if not (flags.get(f.flag_field, 0) >> f.flag_bit) & 1:
                    continue
                if f.name not in obj or obj[f.name] is None or obj[f.name] is False:
                    if f.type == 'true':
                        continue
                    # another field set the shared bit: this one must go too
                    raise TLError(f'{c.name}.{f.name}: the bit is set but there is no value')
            if f.type == 'true':
                continue
            if f.name not in obj:
                raise TLError(f'{c.name}: missing {f.name}')
            out += self._value(f.type, obj[f.name])
        return out

    def _value(self, t, v):
        if t == 'int':
            return struct.pack('<i', v if v < 2**31 else v - 2**32)
        if t == 'long':
            return struct.pack('<q', v if v < 2**63 else v - 2**64)
        if t == 'double':
            return struct.pack('<d', v)
        if t in ('string', 'bytes'):
            return write_bytes(v)
        if t == 'int128':
            return v if isinstance(v, bytes) else v.to_bytes(16, 'little')
        if t == 'int256':
            return v if isinstance(v, bytes) else v.to_bytes(32, 'little')
        if t == 'Bool':
            return struct.pack('<I', BOOL_TRUE if v else BOOL_FALSE)
        m = re.match(r'^([Vv])ector<(.+)>$', t)
        if m:
            head = struct.pack('<I', VECTOR_ID) if m.group(1) == 'V' else b''
            return head + struct.pack('<i', len(v)) + b''.join(self._value(m.group(2), x) for x in v)
        if t.startswith('%'):
            c = self.s.by_name[v['_']]
            return self._fields(c, v)
        if t.startswith('!') or t in ('X', 'Object'):
            if isinstance(v, (bytes, bytearray)):
                return bytes(v)
            return self.encode(v)
        if t in self.s.by_name and not self.s.by_name[t].is_func:
            return self._fields(self.s.by_name[t], v)   # a bare constructor
        if isinstance(v, (bytes, bytearray)):
            return bytes(v)                            # already serialised
        return self.encode(v)

    # -- reading
    def decode(self, data):
        r = Reader(data)
        v = self.read_obj(r)
        return v

    def read_obj(self, r):
        cid = r.uint()
        if cid == GZIP_PACKED:
            inner = zlib.decompress(r.bytes(), 47)
            return self.read_obj(Reader(inner))
        if cid == VECTOR_ID:
            n = r.int()
            return [self.read_obj(r) for _ in range(n)]
        if cid == BOOL_TRUE:
            return True
        if cid == BOOL_FALSE:
            return False
        c = self.s.by_id.get(cid)
        if c is None:
            raise TLError(f'unknown constructor {cid:08x}')
        return self._read_fields(c, r)

    def _read_fields(self, c, r):
        obj = {'_': c.name}
        flags = {}
        for f in c.fields:
            if f.type == '#':
                flags[f.name] = obj[f.name] = r.uint()
                continue
            if f.flag_field:
                if not (flags.get(f.flag_field, 0) >> f.flag_bit) & 1:
                    continue
                if f.type == 'true':
                    obj[f.name] = True
                    continue
            obj[f.name] = self._read_value(f.type, r)
        return obj

    def _read_value(self, t, r):
        if t == 'int':
            return r.int()
        if t == 'long':
            return r.long()
        if t == 'double':
            return r.double()
        if t == 'string':
            return r.bytes().decode('utf-8', 'replace')
        if t == 'bytes':
            return r.bytes()
        if t == 'int128':
            return r.take(16)
        if t == 'int256':
            return r.take(32)
        if t == 'Bool':
            return r.uint() == BOOL_TRUE
        m = re.match(r'^([Vv])ector<(.+)>$', t)
        if m:
            if m.group(1) == 'V':
                if r.uint() != VECTOR_ID:
                    raise TLError('expected a vector')
            n = r.int()
            return [self._read_value(m.group(2), r) for _ in range(n)]
        if t.startswith('%'):
            return self._read_fields(self.s.by_name[t[1:]], r)
        if t in self.s.by_name and not self.s.by_name[t].is_func:
            return self._read_fields(self.s.by_name[t], r)
        return self.read_obj(r)
