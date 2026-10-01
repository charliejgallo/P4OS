#!/usr/bin/env python3
"""Who holds the board's internal RAM: GET /api/heap?trace=1, with names.

    tools/heap_owners.py p4os.local [build/rev1_3/p4os.elf]
    tools/heap_owners.py saved.json build/elf/<sha>.elf

Needs a diagnostic build (docs/MEMORY.md, "Internal RAM audit"): heap
tracing on (CONFIG_HEAP_TRACING_STANDALONE) and, on RISC-V, the frame pointer
(CONFIG_ESP_SYSTEM_USE_FRAME_POINTER), so each record carries its callers.
The board groups the live internal allocations by caller; this turns the
addresses into functions and lines with addr2line, skips the heap's own
frames (malloc, calloc, heap_caps_*...), and prints the owners by bytes.
The ELF has to be the one of the firmware that answered.
"""
import glob
import json
import os
import subprocess
import sys
import urllib.request

SKIP = ("record_allocation", "heap_caps_", "malloc", "calloc", "realloc", "_malloc_r", "_calloc_r", "_realloc_r",
        "trace_malloc", "trace_calloc", "trace_realloc", "__wrap_", "lv_malloc", "aligned_alloc",
        "memalign", "multi_heap", "tlsf_", "esp_heap_", "heap_trace", "_heap_")


def addr2line():
    found = glob.glob(os.path.expanduser("~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line"))
    if not found:
        sys.exit("no riscv32-esp-elf-addr2line under ~/.espressif")
    return found[0]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else "build/rev1_3/p4os.elf"
    if os.path.exists(src):
        data = json.load(open(src))
    else:
        with urllib.request.urlopen("http://%s/api/heap?trace=1" % src, timeout=30) as r:
            data = json.load(r)
    owners = data.get("owners", [])
    addrs = sorted({a for o in owners for a in o["by"] if a != "0x00000000"})
    out = subprocess.run([addr2line(), "-e", elf, "-f", "-C"] + addrs, capture_output=True, text=True).stdout.split("\n")
    names = {}
    for i, a in enumerate(addrs):
        fn, line = out[2 * i], out[2 * i + 1]
        names[a] = (fn, os.path.basename(line.split(" ")[0]))
    groups = {}
    for o in owners:
        frames = [names.get(a, ("?", "")) for a in o["by"] if a != "0x00000000"]
        mine = [f for f in frames if not f[0].startswith(SKIP)]
        key = " < ".join("%s (%s)" % f for f in (mine or frames[-1:]))
        g = groups.setdefault(key, [0, 0])
        g[0] += o["blocks"]
        g[1] += o["bytes"]
    total = sum(g[1] for g in groups.values())
    print("%d records on the board; live internal allocations: %d bytes in %d blocks"
          % (data.get("records", 0), total, sum(g[0] for g in groups.values())))
    if data.get("ungrouped_bytes"):
        print("(plus %d bytes in %d blocks the board could not group)" % (data["ungrouped_bytes"], data["ungrouped_blocks"]))
    for key, (n, b) in sorted(groups.items(), key=lambda kv: -kv[1][1]):
        print("%8d B %5d  %s" % (b, n, key))


if __name__ == "__main__":
    main()
