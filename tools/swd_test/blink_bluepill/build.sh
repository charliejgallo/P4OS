#!/bin/bash
# Builds rapido and charla (.bin, .hex, .uf2) for a Blue Pill with the Mac's own clang:
# no ARM toolchain. README.md says what they are for.
set -e
cd "$(dirname "$0")"
mkdir -p build
CF="--target=thumbv7m-none-eabi -mcpu=cortex-m3 -mno-movt -Os -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Werror"
clang $CF -c blink.c -o build/rapido.o
clang $CF -DCHARLA -c blink.c -o build/charla.o
python3 link.py build/rapido.o build rapido
python3 link.py build/charla.o build charla
