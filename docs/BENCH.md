# The Banco app

The Banco (bench) puts the bench's instruments on one screen: the Rigol
oscilloscope, the Riden power supply, the UNI-T function generator and a
logger that writes them all to a CSV file on the card.

A strip of four tiles is at once the tabs and a dashboard. It is a row on top
in portrait and a column on the left in landscape. Each tile says what its
instrument is doing right now, whichever tab is open: the scope's trigger
state, the supply's output and CV/CC, and the logger's time and rows.

## Oscilloscope: Rigol DS1000Z

It covers the DS1054Z, DS1074Z, DS1104Z and the "+" models, over their raw
SCPI socket (TCP port 5555).

1. On the scope, go to **Utility → IO Setting → LAN Conf.** and turn the LAN
   on (DHCP is fine).
2. In the tab, tap the IP field, type the scope's address and tap
   **Conectar**. P4OS remembers it and reconnects at start.

What the tab offers:

- **Traces:** the channels that are on, in the Rigol's colours, with the trigger
  point and level. There are about 10 sets a second on a LAN. Dragging on
  the grid moves the chosen channel.
- **Controls:**
  - Run/Stop, Única (single), Auto and Captura. Captura shows the scope's own
    screen, as a PNG.
  - Each channel: V/div and time/div go in 1-2-5 steps, plus position,
    DC/AC/GND and ×1/×10.
  - The trigger: source, slope and level. Tapping the level puts it at 50 %.
- **Measurements:** Vpp, frequency, mean and rms of each channel that is on,
  refreshed every second. They are read even while nobody looks, so the
  logger can use them.

Every command is shown at once and read back from the scope a moment later.
If the scope disagrees, its value wins.

## Power supply: Riden RD60xx

The supply connects through its TTL port (3.3 V; TX, RX and GND) to a UART
port of the header. The link is the Modbus app's: set the port, speed and
unit in **Modbus → Riden**.

The tab shows:

- the output, big, with CV/CC and OVP/OCP;
- the switch to turn the output on and off;
- the setpoints, which you can type in or nudge by 0.1 V / 1 V and
  0.01 A / 0.1 A;
- the input voltage and the ceiling it sets (a buck: input minus about 1 V),
  the temperature, Ah and Wh;
- a trace of V and A over the last two minutes.

Every write is read back, because the Riden rejects what it does not like
without saying so. If a setting does not stick, a notice says it was not
accepted.

The Modbus app and the Banco cannot hold the port at the same time. The one
that asks second says who has it, and the port is given back two seconds after
the app that held it closes.

## Generator: UNI-T UTG932E

The generator talks USBTMC over USB. The board's USB OTG port does not supply
5 V, so the generator needs a powered USB hub between the two. Until there is
one, the tab only explains this.

## Logger

The logger writes to `logs/banco-<date>.csv` on the card, with one row at each
interval (0.5 s to 1 min).

You choose the columns:

- the supply's V, A and W;
- Vpp, mean and frequency of each scope channel.

When the instrument has no value at that moment, the cell is left empty,
never repeated from before. That happens with the supply unplugged, a channel
off, or an invalid measurement from the scope.

The log goes on with the app closed. The last 300 samples of one chosen column
are drawn live in the tab.

## From the browser

The portal's **Banco** page (`http://<name>.local/#banco`) shows all of this
on a computer:

- the live trace, drawn by the browser;
- the controls and the measurement table;
- the scope's screenshot;
- the supply, with its setpoints;
- the logger, with a link to download the CSV.

The API is listed at the top of `components/aos_portal/aos_portal_bench.c`.

## Developing without the instruments

These stand-ins run on the Mac:

```
tools/fake_rigol.py --port 5555       # a DS1104Z: sine, PWM, triangle, ramp
tools/fake_modbus.py --rtu            # a Riden RD6012 on a pty
```

To use them with the simulator:

- Run it with `P4_SIM_UART_A=` set to the pty that `fake_modbus.py` prints.
- Use `127.0.0.1` as the scope's address.

What the fake scope does:

- It follows timebase, V/div, offset, trigger and run/stop, so turning the
  knobs changes what you see.
- Its measurements are computed from the samples.
- `:DISP:DATA?` returns a real PNG.

## Not yet measured on the real instruments

- The trace rate of the real DS1000Z, and whether it needs more than 5 ms
  between writes.
- Whether `*OPC?` really waits for autoscale.
- The size of the PNG and how long it takes.
- What LVGL costs to push the trace canvas on the P4.
