#!/usr/bin/env python3
"""The least a core dump says without GDB: each task's pc and ra, as
function and line of the firmware's ELF (addr2line). Used by coredump.sh
when riscv32-esp-elf-gdb is not installed.

    coredump_min.py core.bin p4os.elf addr2line
"""
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile


def main():
    core, elf, a2l = sys.argv[1:4]
    raw = open(core, 'rb').read()
    at = raw.find(b'\x7fELF')
    if at < 0:
        print('no ELF inside the dump')
        return 1
    import io
    e = ELFFile(io.BytesIO(raw[at:]))
    threads = []
    for seg in e.iter_segments():
        if seg['p_type'] != 'PT_NOTE':
            continue
        for n in seg.iter_notes():
            if n['n_type'] == 'NT_PRSTATUS' and n['n_name'] == 'CORE':
                desc = n['n_desc'] if isinstance(n['n_desc'], bytes) else bytes(n['n_desc'], 'latin1')
                regs = struct.unpack_from('<32I', desc, 72)     # RISC-V prstatus: pc, ra, sp, ...
                threads.append(regs)
    if not threads:
        print('no task registers in the dump')
        return 1
    addrs = []
    for r in threads:
        addrs += ['0x%08x' % r[0], '0x%08x' % r[1]]
    out = subprocess.run([a2l, '-pfaC', '-e', elf] + addrs, capture_output=True, text=True).stdout.splitlines()
    print('%d tasks; the first is the one that crashed' % len(threads))
    for i, r in enumerate(threads[:12]):
        pc = out[2 * i] if 2 * i < len(out) else '?'
        ra = out[2 * i + 1] if 2 * i + 1 < len(out) else '?'
        print('task %2d  sp 0x%08x\n   pc %s\n   ra %s' % (i, r[2], pc, ra))
    return 0


if __name__ == '__main__':
    sys.exit(main())
