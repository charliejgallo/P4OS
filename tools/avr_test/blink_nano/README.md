# Two programs for an ATmega328P, to try the Programador's Arduino family

- `rapido.hex`: the "L" LED (PB5, D13) toggling every 100 ms, so the eye
  tells it from the usual one-second Blink. Nothing on the serial port.
- `charla.hex`: the LED every 200 ms, and the serial port at 115200: a line
  at each change (`vuelta N: LED prendido`), a greeting at start, and what
  comes in answered a line at a time (`recibido: ...`), received by
  interrupt (polled every millisecond, most of a line was lost).

    tools/avr_test/blink_nano/build.sh

Built with the avr-gcc the Arduino IDE installs (no Arduino core: registers
by address), into `build/`. Written to a Nano with its old bootloader
through the board's USB host on 2026-11-03 (`docs/plan/PRUEBAS-PLACA.md`).
