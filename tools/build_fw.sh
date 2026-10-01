#!/bin/bash
# build_fw.sh [rev3_x|rev1_3] [idf.py args...]     (default: rev3_x build)
# One build directory per chip profile (docs/hw/waveshare/CI.md): a stale
# sdkconfig of one profile must never leak into the other.
set -e
cd "$(dirname "$0")/.."
profile=${1:-rev3_x}; shift || true
# an environment with ESP-IDF already exported (the CI's container) keeps it
command -v idf.py >/dev/null || source ~/esp/esp-idf/export.sh >/dev/null 2>&1
dir="build/$profile"
# app-flash writes the app into ota_0, but after an update over Wi-Fi
# (tools/ota.sh) otadata may point at ota_1, and the board would go on
# booting the old image from there: an app-flash also erases otadata, so the
# board boots ota_0, the one just written.
args=()
for a in "$@"; do
  args+=("$a")
  [ "$a" = "app-flash" ] && args+=("erase-otadata")
done
idf.py -B "$dir" -D "SDKCONFIG=$PWD/$dir/sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.$profile" \
  "${args[@]:-build}"
# an image going onto the board: keep its ELF for its core dumps
for a in "$@"; do case "$a" in flash|app-flash) tools/elf_keep.sh "$profile" ;; esac; done
