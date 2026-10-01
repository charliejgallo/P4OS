#!/bin/bash
# coredump.sh [host]: the last panic, decoded on the Mac.
#
# Downloads the core dump the board keeps in flash (GET /api/coredump/elf),
# finds the ELF of the firmware that crashed among build/elf/ (kept by
# build_fw.sh and ota.sh when they put an image on the board) and prints the
# tasks, the registers and the backtraces (esp_coredump info_corefile).
#   ERASE=1 tools/coredump.sh ...     and forgets it on the board afterwards
set -e
cd "$(dirname "$0")/.."
HOST=${1:-p4os.local}
B="http://$HOST"
tmp=$(mktemp -d)
curl -4 -s --max-time 5 "$B/api/coredump"; echo
curl -sf --max-time 60 -o "$tmp/core.bin" "$B/api/coredump/elf" || { echo "no core dump on the board"; exit 0; }
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
A2L=$(ls ~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line | tail -1)
for elf in build/rev1_3/p4os.elf $(ls -t build/elf/*.elf 2>/dev/null); do
    out=$(python -m esp_coredump info_corefile -t raw -c "$tmp/core.bin" "$elf" 2>&1) || true
    echo "$out" | grep -q "Invalid application image" && continue
    echo "== the firmware that crashed: $elf"
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
    rm -rf "$tmp"
    exit 0
done
echo "$out" | tail -2
echo "the ELF of the firmware that crashed is not in build/elf/: it cannot be decoded"
rm -rf "$tmp"
exit 1
