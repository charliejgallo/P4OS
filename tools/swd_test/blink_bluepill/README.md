# Two programs for a Blue Pill, to try the Programador's STM32F1

- `rapido`: PC13 - the board's green LED, lit when the pin is low -
  changes every 100 ms, so the eye tells it from the half-second blink
  most Blue Pills come with. Nothing else.
- `charla`: the LED every 200 ms, and USART1 at 115200 8N1 (TX on PA9, RX on
  PA10): a greeting at start, a line at every change (`vuelta N: LED
  prendido`), and each line that comes in answered as `recibido: ...`. The
  same as the Nano's `charla` (`tools/avr_test/blink_nano`), so the
  Terminal sees the same thing from either.

Each one is written three ways, all the same bytes from 0x08000000:
`.bin` (a plain image; the Programador puts it at the flash's start),
`.hex` (Intel HEX, carrying its address) and `.uf2` (family STM32F1).

    tools/swd_test/blink_bluepill/build.sh

It builds into `build/` with the Mac's own clang for a Cortex-M3
(`-mno-movt`, so addresses come from literal pools, the one relocation
`link.py` knows besides the Thumb calls). There is no ARM toolchain:
`link.py` lays the vector table at 0x08000000 and the code after it, with
`blink_pico/link.py`'s relocating.

- `blink.c`: registers by address, on the 8 MHz HSI the chip starts on (no
  crystal, no PLL), so it behaves the same on an STM32F103 and on the
  CKS32, GD32 and APM32 clones Blue Pills often carry. Time is SysTick's
  COUNTFLAG; the UART is polled between ticks, faster than a byte comes at
  115200. Nothing in `.data` or `.bss`, no library calls, no interrupts:
  the vector table is only the 16 Cortex-M entries.
- A fault leaves the LED lit and still.

## Checked before the board (2026-11-05)

- **Run on an emulated Cortex-M3** (unicorn, with RCC, GPIO, USART1 and
  SysTick modelled): the LED changes every 100 ms in `rapido` and every
  200 ms in `charla`; `charla` greets, counts and answers
  `recibido: hola P4OS`; the registers end as intended (USART1 BRR 69,
  115942 baud; SysTick reload 7999).
- **Written by SWD in the simulator** (`P4_SIM_SWD=stm32f103`): the `.hex`,
  `.bin` and `.uf2` of both, each verified byte by byte, then a dump of the
  64 KB and an erase of all.

## On the board

`docs/plan/PRUEBAS-PLACA.md` has the two tests (SWD, and ST's serial
bootloader with BOOT0 at 1): the wiring, and what each step should show.
Copy the files to the card's `firmware/` folder.

- **By SWD:** the Blue Pill's 4-pin header (3V3, DIO, CLK, GND). CLK to pin
  12 of the header (`uart.b` TX), DIO to pin 14 (`uart.b` RX), GND to GND;
  R to EN (pin 11) for the reset, if wanted. The Blue Pill powered from its
  own micro-USB.
- **By the serial bootloader:** BOOT0 jumper at 1, `uart.b` TX to PA10 and
  RX to PA9; or EN to R and BOOT to BOOT0's middle pin, and P4OS works the
  two lines.
- **`charla` in the Terminal:** `uart.b` at 115200, TX to PA10, RX to PA9.

Done on 2026-11-05 with a Blue Pill whose chip is a clone (DPIDR
`2BA01477`, 128 KB): both programs by SWD and by the serial bootloader,
running as they should, and `charla` talking in the Terminal. Its HSI runs
about 1 % slow (202 ms turns), well inside what the UART tolerates.
