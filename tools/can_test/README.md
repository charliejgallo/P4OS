# A second node for trying CAN

What goes on the other end of the bus to try the CAN app on a real bus
(apps/can, docs/MODULES.md "PWM, an analog level, infrared and CAN"), wired
as in `docs/img/bench-can.svg`: the P4 and an ESP32 DevKit, each with an
SN65HVD230, on a twisted pair with 120 Ω at each end.

## auto_esp32

An ESP-IDF project for an ESP32 (any with TWAI: the 38-pin DevKit of the
bench) that plays a car on the bus. Its TWAI is on IO21 (TX, to the
transceiver's CTX) and IO22 (RX, from CRX), at 500 kbit/s in normal mode.

    cd tools/can_test/auto_esp32
    idf.py set-target esp32 && idf.py build

It loads like `tools/usb_test/hola_esp32` (its README): with the ESP32 on a
USB host socket of the P4, `build/flasher_args.json` and the three bins it
names go to a folder under `/firmware` on the card, keeping those paths, and
the Programmer writes it. The console is UART0 at 115200, so the Terminal on
`usb0` shows it.

### What it sends

It drives a loop of about a minute: idles 4 s, pulls away through five
gears to about 100 km/h, cruises, brakes to a stop. The ids and their
coding are the simulator's car (`sim/io_signal_sim.c`), so the example
`senales.dbc` the CAN app writes on its first run decodes it as it is:

| Id | Every | Bytes |
|---|---|---|
| 0x0C0 | 10 ms | engine speed x4 in 2-3 (Motorola), a counter in 7; throttle % in 0, gear in 4, brake in 5 |
| 0x1A0 | 20 ms | speed in km/h x100 in 0-1 (Motorola) |
| 0x3E8 | 500 ms | coolant and oil temperature +40 °C in 0 and 1 (it warms up over a few minutes) |
| 0x18FEF100 | 100 ms | an extended id, J1939 style: a counter in 1-2 |

About 160 frames a second, some 4 % of a 500 kbit/s bus.

Throttle, gear and brake are not in the example `.dbc`. Adding them under
`BO_ 192 Motor` in `<card>/can/senales.dbc` (or from the portal's CAN page)
is the exercise:

    SG_ Acelerador : 0|8@1+ (1,0) [0|100] "%" Vector__XXX
    SG_ Marcha : 32|8@1+ (1,0) [0|5] "" Vector__XXX
    SG_ Freno : 40|1@1+ (1,0) [0|1] "" Vector__XXX

Each line reads `name : start|length@order sign (scale,offset) [min|max]
"unit" receivers`. `32|8@1+` is eight bits from bit 32 (byte 4, bit 0),
Intel order (`@1`, the low byte first; `@0` is Motorola, whose start bit is
the top bit of the first byte, as the RPM's `23|16@0+`), unsigned (`+`).
The value is raw x scale + offset: the RPM's `(0.25,0)` undoes its x4, the
temperatures' `(1,-40)` their +40. The app reads the name, the bits, the
order, the sign, the scale, the offset and the unit; the range and the
receivers are only for other tools.

### OBD-II

It answers mode 01 (SAE J1979) as two units, like the simulator's car, so
the CAN app's OBD tab reads it as it reads a real car:

| Unit | Answers on | Asked at | PIDs |
|---|---|---|---|
| engine | 7E8 (18DAF110) | 7DF or 7E0 (18DB33F1 or 18DA10F1) | 01 04 05 0C 0D 0F 10 11 1C 1F 2F 33 42 46 51 5C 5E A6, and the maps 00 20 40 60 80 A0 |
| gearbox | 7E9 (18DAF118) | 7DF or 7E1 (18DB33F1 or 18DA18F1) | 01 0D, and the map 00 |

The values come from the car's own state: load and throttle, air flow (MAF),
coolant, oil, intake and ambient temperature, a 50 l tank that empties at
the rate the engine burns (PID 5E, about 0.8 l/h idling and 6-8 l/h on the
road) and an odometer that counts the loop's kilometres. A PID it does not
know gets no answer, as on most cars.

By hand, from the Send tab: byte 0 is the length, then the mode and the PID.

| Ask (id 7DF) | Answer | Means |
|---|---|---|
| `02 01 00 00 00 00 00 00` | 7E8 `06 41 00 98 1B 80 13 ..` and 7E9 `06 41 00 80 08 00 00 ..` | each unit's map of PIDs 01-20: bit 7 of A is PID 01 |
| `02 01 0C 00 00 00 00 00` | 7E8 `04 41 0C A B ..` | engine speed, (A x 256 + B) / 4 rpm |
| `02 01 0D 00 00 00 00 00` | 7E8 and 7E9 `03 41 0D A ..` | speed, A km/h: both units know it |
| `02 01 2F 00 00 00 00 00` | 7E8 `03 41 2F A ..` | fuel level, A x 100 / 255 % |

On the console, each question received says how many units answered it.

### The console

A dashboard line a second (speed, gear, rpm, throttle, temperatures, fuel,
odometer), the node's state and error counters, frames sent, received and
dropped, OBD questions and answers, and the bus errors by kind: bit, form,
stuff, ACK, and arbitration lost. The P4 does
not show the kind yet, so the ESP32's console is where to read it. Every
frame received is printed (up to 20 a second), and every state change.

One-key commands: `p` stops and starts the car (it sends nothing while
stopped), `1` `2` `5` `m` set the bus to 125k, 250k, 500k or 1 Mbit/s,
`r` recovers from bus off, `e` zeroes the counters, `h` lists them.

What it shows on purpose:

- **The P4 in Solo escucha** does not acknowledge: the car's first frame is
  retried forever, its TEC climbs to 128 (passive) and stops there, its
  queue fills and the rest is counted as dropped. That is why a node alone,
  or with only listeners, cannot talk. In Normal the P4 acknowledges and it
  all flows.
- **Two speeds** (`2` on the ESP32, 500k on the P4): each sees the other's
  frames as garbage, stuff and form errors pile up.
- **CANL off, or no terminator**: errors on both sides; the scope shows why.
