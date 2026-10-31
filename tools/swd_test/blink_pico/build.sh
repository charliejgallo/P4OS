#!/bin/bash
# Builds blink.uf2, doble.uf2 and rapido.uf2 (and their .bin) for an RP2040 with the Mac's own clang:
# no pico-sdk, no ARM toolchain. README.md says what it is for.
set -e
cd "$(dirname "$0")"
mkdir -p build
CF="--target=thumbv6m-none-eabi -mcpu=cortex-m0plus -Os -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Werror"
clang $CF -c boot2.S -o build/boot2.o
clang $CF -DQUAD -c boot2.S -o build/boot2q.o
clang $CF -c blink.c -o build/blink.o
clang $CF -DDOUBLE -c blink.c -o build/doble.o
clang $CF -DFAST -c blink.c -o build/rapido.o
python3 link.py build/boot2.o build/blink.o build blink
python3 link.py build/boot2.o build/doble.o build doble
python3 link.py build/boot2q.o build/rapido.o build rapido
