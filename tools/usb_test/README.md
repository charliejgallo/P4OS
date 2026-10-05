# Devices for trying the USB host

What was plugged into the board's USB host to try it (docs/USB.md, "The
USB host"), and what to load on it.

## p4os_leonardo

An Arduino Leonardo (ATmega32U4, native USB) as a composite device: a
serial port (CDC-ACM) and a keyboard (HID) at once, each taken by its own
driver on the board. It prints `P4OS Leonardo <n>` once a second, echoes
what it gets in upper case, and types "hola desde el Leonardo" as a
keyboard only when it receives `!` over serial, so on a computer it types
nothing on its own.

Built and loaded from a Mac with the Arduino IDE 1.8's tools (no
arduino-cli needed):

    A=/Applications/Arduino.app/Contents/Java
    $A/arduino-builder -compile -hardware $A/hardware -hardware ~/Library/Arduino15/packages \
      -tools $A/tools-builder -tools ~/Library/Arduino15/packages \
      -built-in-libraries $A/libraries -fqbn arduino:avr:leonardo \
      -build-path /tmp/leo p4os_leonardo/p4os_leonardo.ino

Then the "1200 baud touch" on the Leonardo's port (`stty -f <port> 1200`)
puts it in its bootloader for 8 s, and avrdude writes it:

    avrdude -p atmega32u4 -c avr109 -P <bootloader port> -b 57600 -D \
      -U flash:w:/tmp/leo/p4os_leonardo.ino.hex:i

Find the Leonardo's port by its USB vendor (0x2341) or name, never by
"the usbmodem that is there": the P4 board's console (its CH343) is a
usbmodem too, and opening it restarts the board.

On the board, with the Terminal (or `/api/serial`) on `usb0`, sending `!`
types into the text field whose on-screen keyboard is open (Notas only
takes keys while its keyboard is up).
