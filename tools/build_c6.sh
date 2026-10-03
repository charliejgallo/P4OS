#!/bin/bash
# build_c6.sh [idf.py args...]      the ESP32-C6's firmware (c6/, docs/C6.md)
# Leaves c6/build/p4os_c6.bin, which the P4 installs into the C6 from the
# card (/firmware/c6.bin) or the portal. A serial flash through J7 is only
# for a C6 that does not come back (docs/C6.md, "Recovery").
set -e
cd "$(dirname "$0")/../c6"
command -v idf.py >/dev/null || source ~/esp/esp-idf/export.sh >/dev/null 2>&1
[ -f build/CMakeCache.txt ] || idf.py set-target esp32c6
idf.py "${@:-build}"
