"""
TRADUCTOR - the pack format (main/td_pak.h), shared by the tools.

    "TRPK" u16 version u16 count
    count x entry (48 bytes):
        char name[24]  u32 type  u32 off  u32 len  u16 rows  u16 cols
        f32 scale  f32 scale_a
    data: each entry 16-byte aligned, stored as is

The pack is one logical file cut in parts of PART bytes (<x>.pak, .pak.1 ...)
like Bastion's, since the store keeps a file under 8 MB.
"""
import struct

PART = 7_500_000

T_BYTES, T_MAT8, T_VECF, T_SPM, T_LEX, T_DICT, T_PHRASE, T_INFO = 0, 1, 2, 3, 4, 5, 6, 7


class Writer:
    def __init__(self):
        self.entries = []   # (name, type, data, rows, cols, scale, scale_a)

    def add(self, name, typ, data, rows=0, cols=0, scale=0.0, scale_a=0.0):
        assert len(name.encode()) < 24, name
        assert all(e[0] != name for e in self.entries), name
        self.entries.append((name, typ, bytes(data), rows, cols, scale, scale_a))

    def blob(self):
        n = len(self.entries)
        head = 8 + 48 * n
        off = (head + 15) & ~15
        dirs, datas = [], []
        for name, typ, data, rows, cols, scale, scale_a in self.entries:
            dirs.append(struct.pack('<24sIIIHHff', name.encode(), typ, off, len(data),
                                    rows, cols, scale, scale_a))
            datas.append((off, data))
            off = (off + len(data) + 15) & ~15
        out = bytearray(off)
        out[0:8] = b'TRPK' + struct.pack('<HH', 1, n)
        out[8:head] = b''.join(dirs)
        for o, d in datas:
            out[o:o + len(d)] = d
        return bytes(out)

    def write(self, path):
        """path is the .pak; parts beyond the first get .1, .2 ..."""
        import os
        blob = self.blob()
        parts = max(1, (len(blob) + PART - 1) // PART)
        for i in range(parts):
            name = path if i == 0 else '%s.%d' % (path, i)
            with open(name, 'wb') as f:
                f.write(blob[i * PART:(i + 1) * PART])
        i = parts
        while os.path.exists('%s.%d' % (path, i)):     # stale parts of a bigger one
            os.remove('%s.%d' % (path, i))
            i += 1
        return len(blob), parts


def read(path):
    """-> {name: (type, bytes, rows, cols, scale, scale_a)}"""
    import os
    blob = bytearray()
    i = 0
    while True:
        name = path if i == 0 else '%s.%d' % (path, i)
        if not os.path.exists(name):
            break
        blob += open(name, 'rb').read()
        i += 1
    assert blob[:4] == b'TRPK'
    ver, n = struct.unpack_from('<HH', blob, 4)
    out = {}
    for k in range(n):
        name, typ, off, ln, rows, cols, sc, sa = struct.unpack_from('<24sIIIHHff', blob, 8 + 48 * k)
        out[name.rstrip(b'\0').decode()] = (typ, bytes(blob[off:off + ln]), rows, cols, sc, sa)
    return out
