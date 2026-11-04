#!/bin/bash
# Builds rapido.hex and charla.hex for an ATmega328P at 16 MHz (a Nano or
# an Uno) with the avr-gcc the Arduino IDE installs. README.md says what
# each is for.
set -e
cd "$(dirname "$0")"
G=$(ls -d ~/Library/Arduino15/packages/arduino/tools/avr-gcc/*/bin 2>/dev/null | head -1)
[ -n "$G" ] || { echo "no avr-gcc: install the Arduino IDE and its AVR boards"; exit 1; }
mkdir -p build
for p in rapido charla; do
    "$G/avr-gcc" -mmcu=atmega328p -Os -Wall -o build/$p.elf $p.c
    "$G/avr-objcopy" -O ihex build/$p.elf build/$p.hex
    "$G/avr-size" build/$p.elf | tail -1
done
