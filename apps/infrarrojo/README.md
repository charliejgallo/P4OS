# Infrarrojo (Infrared)

A learning remote for the bench, T3 of the workshop. Id `aos.infrarrojo`.
It needs the firmware with `aos_io_ir_*` (0.10, `docs/MODULES.md`, "PWM, an
analog level, infrared and CAN").

- **Learn.** The receiver listens while the Aprender tab is in front. Each
  capture is decoded - NEC, NEC extended and 32-bit, Samsung, Sony SIRC 12,
  15 and 20, JVC, Panasonic and Kaseikyo, RC5, RC6 mode 0 - or kept raw,
  and drawn. Pressing the same button again confirms it (three times for a
  button learned from a remote's sheet). The carrier shown is the
  protocol's: a demodulating receiver cannot measure it.
- **Remotes.** Devices in `/sdcard/ir`, each drawn as a real remote from
  its buttons' roles: power and source on top, the arrows round OK, the
  volume and channel rockers, transport keys, the four colours, the digits,
  then the rest by name. A key sends on press; held, it sends its
  protocol's repeat. Edit mode opens each key's sheet: name, role, learn
  again, a code typed by hand (protocol, address, command).
- **Air conditioners** from SmartIR's library: a thermostat with mode, fan,
  swing or preset (whatever that model's file has) and temperature; each
  change sends the whole state 0.6 s after the last tap.
- **Codes.** SmartIR's library by kind, brand and model; a command is tried
  with a tap before the device is added. "¿De qué control es?" searches the
  library for a captured button: exact protocol fields for a known
  protocol, SmartIR's duration matching for a raw frame.
- **Wiring** (Cableado): the pins (any free GPIO of the header; receiver
  GPIO28 and LED GPIO32 by default), the receiver and the LED drawn, and a
  loop test with the LED aimed at the receiver.
- **Its portal page** (`web/infrarrojo.js`, `#infrarrojo`): the devices and
  their buttons, sending, learning (the board listens and the captures
  appear in the browser), an air conditioner's state, a code by hand or
  pasted (Broadlink base64 or hex, Pronto, Tuya, raw), importing a SmartIR
  file of a television, fan or light, and downloading a device's JSON.

## Files

- `/sdcard/ir/<name>.json`, one per device; the format is at the top of
  `main/ir_store.c`. A button is a protocol code (`proto`, `addr`, `cmd`)
  or `raw` durations in microseconds, mark first, with its `freq`.
- `/data/ir_pedido.json` and `/data/ir_estado.json`: the page's requests
  and the app's answers (`main/ir_portal.c`). They work while the app is
  alive; it keeps running in the background for that, and lets go of the
  LED's pin after a minute there with nothing to send.
- `/apps/infrarrojo_p4.pak`: SmartIR's library (below).

## The code library

SmartIR (MIT licence, © 2019 Vassilis Panos, 2024 Li Tin O've Weedle;
https://github.com/litinoveweedle/SmartIR) travels as a pack, not in git:

```
python3 apps/infrarrojo/tools/pack_smartir.py --src <SmartIR checkout> [--sim]
```

`--src` is a checkout of SmartIR or of a fork with `codes/` (the user's
`smartir-universal`, whose `code_converter.py` reads every encoding).
399 devices and 76 000 codes come to 8.3 MB: each device's marks and spaces
are snapped to a few levels and a code is a list of indices into its table
of (mark, space) pairs, a few bits each. The format is in the script.
Without the pack the app learns and sends all the same.

## Building and trying it

```
tools/build_apps.sh infrarrojo
tools/install_apps.sh p4os.local infrarrojo     # the .so, the pack and the page
```

In the simulator what an IR output sends reaches every open input, and a
NEC remote (address 0x04, the command counting up) presses a key every 6 s
(`P4_SIM_IR_REMOTE=0` stops it). LG televisions use that address: the
library search finds them.

## Wiring

- **Receiver** (TSOP38238, VS1838B): OUT to the receiver's GPIO, VS to 3V3
  (pin 18), GND. Never 5 V: its output would put 5 V on the pin.
- **LED**: the GPIO through 1 kΩ to the base of an NPN (2N2222, BC337),
  emitter to GND, the LED from the collector to 5 V through 33 Ω (~100 mA
  pulses, 5-8 m). Never the LED straight on the pin.
