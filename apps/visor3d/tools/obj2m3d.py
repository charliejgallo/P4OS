"""OBJ (+MTL, Kd as sRGB 0..1) -> M3D1 with a RGB565 colour per face."""
import struct, sys, os
def orient(v, f):
    """Turns every loose piece to face outwards. The viewer takes a model whose
    signed volume says 'solid' as closed and skips its back faces, so a
    shell laid on it wound inwards (Tommy's eyes and mouth) vanishes. A
    closed piece is judged by its signed volume; an open one by its normals
    against the direction from the model's centre."""
    par = list(range(len(v)))
    def find(a):
        while par[a] != a:
            par[a] = par[par[a]]; a = par[a]
        return a
    for t in f:
        for k in (1, 2):
            ra, rb = find(t[0]), find(t[k])
            if ra != rb: par[ra] = rb
    lo = [min(p[k] for p in v) for k in range(3)]; hi = [max(p[k] for p in v) for k in range(3)]
    c = [(lo[k] + hi[k]) / 2 for k in range(3)]
    vol, dirn, edges = {}, {}, {}
    for i, (a, b, d) in enumerate(f):
        A, B, D = v[a], v[b], v[d]
        r = find(a)
        vol[r] = vol.get(r, 0) + (A[0] * (B[1] * D[2] - B[2] * D[1]) - A[1] * (B[0] * D[2] - B[2] * D[0])
                                  + A[2] * (B[0] * D[1] - B[1] * D[0]))
        u = [B[k] - A[k] for k in range(3)]; w = [D[k] - A[k] for k in range(3)]
        n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]]
        g = [(A[k] + B[k] + D[k]) / 3 - c[k] for k in range(3)]
        dirn[r] = dirn.get(r, 0) + sum(n[k] * g[k] for k in range(3))
        for e in ((a, b), (b, d), (d, a)):
            key = (min(e), max(e)); edges[key] = edges.get(key, 0) + 1
    open_ = set(find(e[0]) for e, n in edges.items() if n == 1)
    flipped = 0
    for i, (a, b, d) in enumerate(f):
        r = find(a)
        score = dirn[r] if r in open_ else vol[r]
        if score < 0:
            f[i] = (a, d, b); flipped += 1
    print('orient: %d pieces, %d open, %d triangles turned' % (len(vol), len(open_), flipped))


def main(src, dst):
    kd, cur, v, f, col = {}, None, [], [], []
    mtl = os.path.splitext(src)[0] + '.mtl'
    if os.path.exists(mtl):
        for l in open(mtl):
            p = l.split()
            if p and p[0] == 'newmtl': cur = p[1]
            elif p and p[0] == 'Kd': kd[cur] = tuple(min(255, round(float(x) * 255)) for x in p[1:4])
    c565 = lambda r, g, b: ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    cc = c565(200, 200, 200)
    for l in open(src):
        p = l.split()
        if not p: continue
        if p[0] == 'v': v.append(tuple(float(x) for x in p[1:4]))
        elif p[0] == 'usemtl': cc = c565(*kd.get(p[1], (200, 200, 200)))
        elif p[0] == 'f':
            ix = [int(x.split('/')[0]) - 1 for x in p[1:]]
            for k in range(1, len(ix) - 1):
                f.append((ix[0], ix[k], ix[k + 1])); col.append(cc)
    orient(v, f)
    with open(dst, 'wb') as o:
        o.write(b'M3D1' + struct.pack('<III', len(v), len(f), 1))
        o.write(b''.join(struct.pack('<3f', *p) for p in v))
        o.write(b''.join(struct.pack('<3I', *t) for t in f))
        o.write(struct.pack('<%dH' % len(col), *col))
    print('%s: %d vertices, %d triangles, %d KB' % (dst, len(v), len(f), os.path.getsize(dst) // 1024))
main(sys.argv[1], sys.argv[2])
