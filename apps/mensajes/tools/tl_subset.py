#!/usr/bin/env python3
"""
P4OS - Mensajes: the subset of the schema the app uses.

    tl_subset.py api.tl mtproto.tl wanted.txt > schema/layer234.tl

Takes the published schema of one layer (api.tl and mtproto.tl as they
come; the layer is the "// LAYER n" line at the end of api.tl) and writes
only the functions of wanted.txt and the constructors their arguments and
answers can carry, the closure tl_gen.py turns into C. Run it again, with
a newer api.tl, to move to another layer: the C tables and the fake server
follow, and MJ_LAYER in the generated header says which one it is.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tl_schema  # noqa: E402


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    api, mtp, wanted = sys.argv[1:]
    text = open(api, encoding='utf-8').read()
    m = re.search(r'//\s*LAYER\s+(\d+)', text)
    layer = int(m.group(1)) if m else 0
    sch = tl_schema.Schema.load(mtp, api)
    names = [w.strip() for w in open(wanted) if w.strip() and not w.startswith('#')]
    missing = [n for n in names if n not in sch.by_name]
    if missing:
        sys.exit('not in the schema: ' + ', '.join(missing))
    keep = sch.closure(names)
    out = sys.stdout
    out.write(f'// The subset of the Telegram API schema, layer {layer}, that Mensajes uses:\n')
    out.write('// written by tools/tl_subset.py from api.tl and mtproto.tl. Do not edit by hand.\n')
    out.write(f'// LAYER {layer}\n\n')
    for funcs in (False, True):
        out.write('---functions---\n' if funcs else '---types---\n')
        for c in keep:
            if c.is_func == funcs:
                out.write(c.line + '\n')
        out.write('\n')
    print(f'{len(keep)} combinators ({sum(1 for c in keep if c.is_func)} functions), layer {layer}',
          file=sys.stderr)


if __name__ == '__main__':
    main()
