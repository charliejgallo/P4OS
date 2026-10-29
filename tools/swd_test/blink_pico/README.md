# Blink for an RP2040, to try the Programador by SWD

`build/blink.uf2` (and `build/blink.bin`, the same from 0x10000000):
GPIO25 - a Pico's LED - changes every half second, and every change is
told through SEGGER RTT, which the Programador's RTT console reads. A line
sent down the console (the portal's field) comes back as `recibido: ...`.
On a Pico W the LED is on the radio chip and stays off; RTT works the same.

    tools/swd_test/blink_pico/build.sh

It builds with the Mac's own clang (it compiles for a Cortex-M0+; there is
no ARM linker, so `link.py` does the little linking this needs) - no
pico-sdk, no ARM toolchain:

- `boot2.S`: the second stage the boot ROM checks and runs, pico-sdk's
  `boot2_generic_03h` (XIP with the plain 03h read), then the vector table.
- `blink.c`: registers by address, the RTT block built by hand at
  0x20001000, nothing in `.data`/`.bss`, no division; it runs on the ring
  oscillator the ROM leaves (~6 MHz), so the half second is a loop.
- `link.py`: boot2 with its CRC-32 (MPEG-2), the vector table at 0x100,
  the code at 0x140; `R_ARM_ABS32` and the Thumb calls, nothing else.

Two ways to check the file itself before SWD: copy it to the Pico in
BOOTSEL (the LED blinks), or flash it to `P4_SIM_SWD=rp2040` in the
simulator, where the Programador verifies it byte by byte (done on
2026-10-29).
