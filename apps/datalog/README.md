# Datalog

A data logger for the header's sensors. Id `aos.datalog`. Each *line* (a
reading) is a value written to a CSV on the card with its date and time,
on a schedule, with alarms if you like. Up to 16 lines.

## The screen

- **The bar:** the time, the date, whether it is logging; ▶/⏸ starts and
  pauses every line, + adds one.
- **One card per line:** the latest value (red while a high alarm lasts,
  blue for a low one, with an ALTA/BAJA tag), a sparkline of the last 90
  readings, where it comes from, how often it reads and when it last did,
  or why it failed.
- **A line** (tap a card): its chart over 1 h, 6 h, 24 h, 7 or 30 days or
  the whole file (the range that shows some thirty readings is picked on
  opening), minimum, maximum and mean, the alarm limits drawn across, the
  point under a finger held on the chart, the alarms in words, the file and
  "Borrar los datos". The pencil edits it.
- **Settings**, under the cards: the CSV format, and Módulos for the
  sensors and ports.

## A line

The form asks, in this order:

1. **How often:** every N seconds, minutes or hours (presets 1 s to 1 h),
   or at up to eight times of day. "Every" reads at once when logging
   starts and then on the clock's grid (every 5 min is :00, :05...). The
   times need the clock from the network.
2. **Kind of sensor:** GPIO, I2C, SPI, 1-Wire, Pines (a DHT22, an HC-SR04,
   a scale, a flow meter, a PIR), Serie (an MH-Z19, a PMS5003, a PZEM-004T,
   a radar) or Otros.
3. **What that kind asks:**
   - **I2C, SPI, 1-Wire, Pines, Serie:** the bus, the sensor on it (BME280
     0x76, MAX6675 CS49, DS18B20 5A1E0F, DHT22) and which of its values
     (temperature, humidity, pressure... up to twelve), each with what it
     reads now and the unit the service gives it. These are the system's
     sensor service (`aos_sensors.h`, `docs/SENSORS.md`): the app never
     opens those buses, it takes the latest value of a sensor by name, so
     Módulos, Bus and the portal see the same thing. A kind with nothing
     on it says how to connect or declare one, with "Buscar de nuevo" and
     "Abrir Módulos".
   - **GPIO, digital:** a pin (the free ones first, then those a UART port
     or a module names; never the sensor buses' pins nor another app's),
     pull-up, pull-down or none, and normal or inverted logic. It logs 1/0.
   - **GPIO, analog (ADS1115):** the converter and its input A0-A3; the
     **range** (±6.144 V to ±0.256 V), which is the module's `gain=` in
     `modules.txt`, shared by its four inputs and written when picked
     (through `aos_modules_save()`, as Módulos does; a detected ADS1115
     gets its module line); and what it logs: volts, the raw counts, or an
     **estimate** through two points into a unit of your own (a
     0.5-4.5 V pressure sender as 0-10 bar).
   - **Otros:** the board itself: Wi-Fi signal, free PSRAM, free internal
     RAM, uptime.
4. **What it reads now**, under the choices, once a second.
5. **Alarms:** low (below) and high (above), a hysteresis (a high alarm
   ends below high - hysteresis), and how it tells: **a sound and a
   message on screen**, over whatever app is in front, or **only a note in
   the CSV**. Either way the CSV notes when it starts ("ALTA: más de
   80,0 °C"), each reading while it lasts ("alta") and when it ends.
   **An output:** a GPIO that goes to the level chosen (3V3 or GND) while
   the alarm lasts, and to the other one otherwise and while paused: a
   relay module, a light. Several lines may share one output.

The name sets itself from the source until you type one.

## Files

- **The lines:** `<card>/data/datalog.txt`, text, one `line` per reading
  (`dl_cfg.c` has the format). The app watches its date and reloads it when
  something else writes it.
- **The readings:** `<card>/datalog/<id>.csv`, one per line:

  ```
  fecha,hora,Horno (°C),nota
  2026-10-18,22:57:00,187.75,"ALTA: más de 150,0 °C"
  2026-10-18,22:57:02,190.75,alta
  ```

  Or with `;` and a decimal comma (Settings), for Excel in Spanish. A
  failed reading has an empty value and the reason as its note. Before the
  clock is set the date says `sin hora` and the time is the uptime (`+123`).
  Each reading opens, appends and closes the file: the FAT commits on
  close, and a logger has to survive a power cut. Archivos opens and charts
  them too.

## Running in the background

`AOS_APP_FLAG_BACKGROUND`: leaving the app hides it and the engine (a
thread of its own, `dl_engine.c`) goes on. While it logs, the app asks to
**start with the board** (`aos_ui_boot_start_set`), so a power cut or an
update does not stop a log; paused, it stops asking. Settings, Developer
lists it.

The charts never read a whole big file: up to 1 MB they do; above that
they find where the range starts by bisection and read lines at evenly
spread offsets (`dl_csv.c`).

## Portal page

`web/datalog.js`, the page `#datalog`: logging or paused with a button for
each, the CSV format, a table of the lines with their latest value, alarm
limits and errors (live), and each CSV charted over a range with min, max,
mean, the alarm notes and a reading under the pointer, to download or
delete. The live values come through `/api/live` (key `state`, a JSON the
app puts every second while the page looks); the page sends `run=0|1` and
`csv=comma|semicolon` the same way. A CSV over 8 MB is read by its last
8 MB.

## On the board

Tried on 2026-10-18 with the MAX6675 on `spi.a` and a DS18B20 on
`gpio.ow`: lines made with the form, logging with another app in front, a
high alarm over the clock and the home screen, a restart while logging
(it came back by itself) and pause and resume from the portal's live
channel. 262 KB of PSRAM, ~3 KB of internal RAM. Not yet: an alarm's GPIO
output on real hardware, an ADS1115 and a digital input.

**Its own catalog.** `aos_tr()` translates with the catalog of the app in
front, so a hidden app's texts (the CSV's notes, the alarm over another
app) came out in Spanish. The app reads `aos.datalog.lang` itself
(`dl_i18n.c`) and `_()` in its sources is `dl_tr()`.

## In the simulator

The emulated bench has a BME280, SHT31, BH1750, INA219 and ADS1115 on
`i2c.ext`. For the board's MAX6675 and DS18B20, a `modules.txt` with

```
port spi.a sck=52 mosi=50 miso=51 cs=49 freq=1000000
port gpio.ow pin=34
module max6675 spi.a
module ds18b20 gpio.ow
```

(plus the default profile's other ports) and `P4_SIM_SPI_A="49=max6675"`;
the two DS18B20s come by themselves (`P4_SIM_ONEWIRE`).

## Not done

- The lines are made on the board only; the page shows and charts them.
- If more than four apps are alive, the shell may close the oldest even
  with `BACKGROUND`, and logging stops until the app opens again (a
  request to the firmware in `docs/internal/HANDOFF-NUEVAS-APPS.md`).
