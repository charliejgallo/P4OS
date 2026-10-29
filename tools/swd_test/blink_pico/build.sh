#!/bin/bash
# Builds blink.uf2 (and blink.bin) for an RP2040 with the Mac's own clang:
# no pico-sdk, no ARM toolchain. README.md says what it is for.
set -e
cd "$(dirname "$0")"
mkdir -p build
CF="--target=thumbv6m-none-eabi -mcpu=cortex-m0plus -Os -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Werror"
clang $CF -c boot2.S -o build/boot2.o
clang $CF -c blink.c -o build/blink.o
python3 link.py build/boot2.o build/blink.o build
