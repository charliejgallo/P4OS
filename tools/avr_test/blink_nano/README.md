# Two programs for an ATmega328P (and a Mega 2560), to try the Programador's Arduino family

- `rapido.hex`: the "L" LED (PB5, D13) toggling every 100 ms, so the eye
  tells it from the usual one-second Blink. Nothing on the serial port.
- `charla.hex`: the LED every 200 ms, and the serial port at 115200: a line
  at each change (`vuelta N: LED prendido`), a greeting at start, and what
  comes in answered a line at a time (`recibido: ...`), received by
  interrupt (polled every millisecond, most of a line was lost).

    tools/avr_test/blink_nano/build.sh

Built with the avr-gcc the Arduino IDE installs (no Arduino core: registers
by address), into `build/`. The same two sources also build as
`mega-rapido.hex` and `mega-charla.hex` for a Mega 2560: its "L" LED is
on PB7 and its first serial port's interrupt has another name, and the
greeting says which board it is. Written to a Nano with its old
bootloader through the board's USB host on 2026-11-03 and through the
header on 2026-11-05, and to a Mega 2560 through the USB host the same
day (`docs/plan/PRUEBAS-PLACA.md`).
