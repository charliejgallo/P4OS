#!/bin/bash
# coredump.sh [host]: the last panic, decoded on the Mac.
#
# Downloads the core dump the board keeps in flash (GET /api/coredump/elf)
# and decodes it (esp_coredump info_corefile: the tasks, the registers, the
# backtraces) against the ELF of THE FIRMWARE THAT MADE IT, and no other.
# Which one that is comes from the dump itself: its notes carry the ELF's
# sha, which is the name build_fw.sh and ota.sh keep each ELF under in
# build/elf/ (tools/elf_keep.sh). A dump stays in flash through any number of
# restarts until it is erased, and read against the wrong ELF an old dump
# looks like a new bug with wrong lines (2026-10-08: the BLE panic of 0.12's
# first OTA, read a day later as a new one). So with no ELF of that sha here
# it stops and says so, instead of trying the others.
#
#   ERASE=1 tools/coredump.sh ...    and forgets it on the board afterwards
#   ELF=path/to/p4os.elf tools/...   that ELF, whatever its sha (at your risk)
#
# Downloading the whole dump counts as reading it: the board stops saying
# there is an unread one (GET /api/coredump, "unread").
# The portal's token, for a board that asks for its password here
# (Settings, Portal web; docs/SECURITY.md): P4OS_TOKEN=<token> tools/...
[ -n "$P4OS_TOKEN" ] && curl() { command curl -H "Authorization: Bearer $P4OS_TOKEN" "$@"; }

set -e
cd "$(dirname "$0")/.."
HOST=${1:-p4os.local}
B="http://$HOST"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

info=$(curl -4 -s --max-time 5 "$B/api/coredump")
echo "$info" | python3 -c '
import json, sys, time
d = json.load(sys.stdin)
if not d.get("present"):
    print("no core dump on the board"); sys.exit(0)
seen = d.get("seen") or 0
when = time.strftime("%Y-%m-%d %H:%M", time.localtime(seen)) if seen else "?"
where = {"running": "the firmware running now", "other": "the other OTA slot"}.get(d.get("slot"), "neither slot (an older image)")
print("dump: task %s, pc %s, mcause %s, %s bytes%s" % (d.get("task") or "?", d.get("pc"), d.get("mcause"), d.get("size"),
      "" if d.get("valid") else ", CHECKSUM BAD"))
print("made by firmware %s (%s%s), first seen %s%s" % (d.get("elf_sha") or "?", where,
      ", " + d["version"] if d.get("version") else "", when, ", unread" if d.get("unread") else ""))
'
echo "$info" | grep -q '"present":true' || exit 0
curl -4 -sf --max-time 60 -o "$tmp/core.bin" "$B/api/coredump/elf" || { echo "the dump could not be downloaded"; exit 1; }

# the sha from the dump's own notes (a board on an older firmware does not
# say it in /api/coredump)
sha=$(python3 - "$tmp/core.bin" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
elf = d[24:]                                   # after core_dump_header_t
if elf[:4] != b"\x7fELF":
    sys.exit(0)
phoff, = struct.unpack_from("<I", elf, 0x1C)
phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
for i in range(phnum):
    ptype, off, _, _, filesz = struct.unpack_from("<IIIII", elf, phoff + i * phentsize)
    if ptype != 4:                              # PT_NOTE
        continue
    at = off
    while at + 12 <= off + filesz:
        namesz, descsz, ntype = struct.unpack_from("<III", elf, at)
        desc = at + 12 + ((namesz + 3) & ~3)
        if ntype == 8266:                       # version + the app ELF's sha, as text
            s = elf[desc + 4:desc + 4 + 16].decode("ascii", "replace")
            # 16 characters, or 9 from firmware built before
            # CONFIG_APP_RETRIEVE_LEN_ELF_SHA was raised: the hex part
            print("".join(c for c in s.split("\0")[0] if c in "0123456789abcdef"))
            sys.exit(0)
        at = desc + ((descsz + 3) & ~3)
EOF
)

if [ -n "$ELF" ]; then
    elf=$ELF
    echo "== using $elf as asked (the dump says firmware ${sha:-?})"
elif [ -z "$sha" ]; then
    echo "the dump does not say which firmware made it: not decoding it against a guess"
    echo "(ELF=build/elf/<sha>.elf $0 $HOST to force one)"
    exit 1
elif [ "$(ls build/elf/"$sha"*.elf 2>/dev/null | wc -l)" -eq 1 ]; then
    # by prefix: a dump from older firmware carries only 9 characters
    elf=$(ls build/elf/"$sha"*.elf)
    echo "== the firmware that crashed: $elf"
elif [ -f build/rev1_3/p4os.elf ] && [ "$(shasum -a 256 build/rev1_3/p4os.elf | cut -c1-${#sha})" = "$sha" ]; then
    elf=build/rev1_3/p4os.elf
    echo "== the firmware that crashed: $elf (the one built last)"
else
    echo "the ELF of firmware $sha is not in build/elf/ (it keeps the last 10): this dump cannot be"
    echo "decoded here, and decoding it against another firmware would point at the wrong lines."
    echo "If it is old, erase it: curl -4 -X POST $B/api/coredump/erase"
    exit 1
fi

source ~/esp/esp-idf/export.sh >/dev/null 2>&1
A2L=$(ls ~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line | tail -1)
out=$(python -m esp_coredump info_corefile -t raw -c "$tmp/core.bin" "$elf" 2>&1) || true
if echo "$out" | grep -q "GDB executable not found"; then
    # the whole decode needs riscv32-esp-elf-gdb:
    #   python ~/esp/esp-idf/tools/idf_tools.py install riscv32-esp-elf-gdb
    echo "(no riscv32-esp-elf-gdb: only each task's pc and ra; install it with"
    echo " python ~/esp/esp-idf/tools/idf_tools.py install riscv32-esp-elf-gdb)"
    python tools/coredump_min.py "$tmp/core.bin" "$elf" "$A2L"
else
    echo "$out"
fi
[ -n "$ERASE" ] && curl -4 -s -X POST "$B/api/coredump/erase" && echo
exit 0
