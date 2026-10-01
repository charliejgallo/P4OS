#!/bin/bash
# elf_keep.sh <profile>: keeps build/<profile>/p4os.elf as build/elf/<sha256[:16]>.elf,
# the name a core dump asks for (tools/coredump.sh), and the last 10 only.
# Called by build_fw.sh when it flashes and by ota.sh when it uploads.
set -e
cd "$(dirname "$0")/.."
elf="build/${1:-rev1_3}/p4os.elf"
[ -f "$elf" ] || exit 0
mkdir -p build/elf
sha=$(shasum -a 256 "$elf" | cut -c1-16)
cp -p "$elf" "build/elf/$sha.elf"
ls -t build/elf/*.elf | tail -n +11 | xargs rm -f 2>/dev/null || true
echo "elf kept as build/elf/$sha.elf"
