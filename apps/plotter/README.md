# Monitor serie (`aos.plotter`)

Plots live the numbers that come in on a serial port: a microcontroller
printing readings, a multimeter's cable, a scale, anything that sends lines
with numbers in them. In English its name is Serial Plotter.

## Where it reads from

Not from the port itself: from a channel of the firmware's capture service
(`aos_serial_*` in `components/aos_io/include/aos_io.h`), the same A and B
the Terminal shows. Both can look at one device at once, and the capture
goes on with the app closed: opening the app plots the last 3000 lines the
service kept. The connection sheet is the Terminal's: a speed from 300 baud
up or any typed in (50 to 5000000), 5 to 8 data bits, parity, stop bits,
and the USB adapter's DTR and RTS (`aos_serial_start_ex`, firmware of
2026-11-23 or later).

A timer every 40 ms takes the new lines. Measured in the simulator against
`tools/fake_serial.py fast`: 800 lines a second (about 28 KB/s, more than
twice what 115200 baud carries) for 20 s went to the CSV with no line
missing.

## How lines become numbers (`main/pl_parse.c`)

- **Automatic:** numbers separated by spaces, commas, semicolons or tabs
  (one series per column, "Value 1", "Value 2"...); `name:value` or
  `name=value` pairs (`temp:21.5,hum:40`); a word just before a number
  (`Temp 21.5`); a CSV header (two words or more between commas, semicolons
  or tabs) whose names go to the columns at the same places; with
  semicolons and no point, the comma is the decimal mark. A unit stuck to
  a number (`3.3V`, `21.5°C`) is kept. Lines without numbers are skipped,
  and a new set of names becomes series only when the same shape comes
  twice in a row, so a board's boot log makes none.
- **Profiles,** for the rest, kept in `<data>/plotter.txt` (the portal's
  page edits the same file; the app reads it again when its date changes).
  The line is cut into fields at spaces, tabs, commas, semicolons, `:` and
  `=`; each series is a field by number, times a scale plus an offset, and
  the decimal point can come from another field through a map of codes:
  the multimeter's `+0265 4` with `dp=2 map="0:0,1:3,2:2,4:1"` is 26.5. The
  first start writes that profile as an example.
- The Format sheet shows the last lines and what came out of each; the
  profile's sheet numbers the last line's fields.

## The data (`main/pl_data.c`)

Two rings in PSRAM, 5.25 MB in all: every row (2 MB, ~58 000 rows: three
minutes at the most a 115200 line carries, hours at a sensor's pace) and
one bucket a second with each series' low, high, sum and count (3.25 MB,
~8 hours). The chart
takes columns from the rows when the window fits in them and from the
buckets otherwise; each column keeps the low, the high and the last value,
so a spike in a crowded pixel still shows.

## The screen

The last value of one series big, with the window's minimum, maximum and
average; the chart below with a legend (a tap makes a series big, a second
one hides it), dotted lines at the alarms' limits. One finger reads the
point under it; two fingers zoom and drag back in time (that pauses the
view; Live comes back). Windows from 10 s to 8 h, an automatic or fixed
value axis, and XY (one series against another). Lying down, the chart
takes the left and the tools and the big value go on the right.

Sheets: connection, format and profiles, series and alarms (a minimum, a
maximum and a beep per series, at most an alert every 10 s, shown over any
app), view, and send (a line with CR LF, LF, CR or nothing, with the last
lines that came in).

The app stays alive hidden (`AOS_APP_FLAG_BACKGROUND`): reading, alarms,
the CSV and the portal go on; only the drawing stops. It speaks hidden, so
it reads its own catalogue (`main/pl_i18n.c`, as Datalog does).

## Recording

The record button writes `<sd>/plotter/plot-<date>-<time>.csv`: the date
and time with milliseconds, the seconds since it started and a column per
series. A series that appears in the middle starts a new file (`-2`,
`-3`...) with its header.

## The portal's page (`web/plotter.js`)

Through `aos_hal_live_*`, while a page looked in the last five seconds: the
chart's columns as binary float32 four times a second, and a JSON state
every second (connection, counts, series with their last value and the
window's statistics, the last lines). The page draws the chart in a canvas
with a reading under the mouse, downloads it as PNG or what is shown as
CSV, sends lines, pauses, changes the window, records, lists and deletes
the CSVs, and edits the profiles and alarms.

## Trying it

```
apps/plotter/tools/fake_serial.py mix        # prints its pty
cd sim && cmake -B build-plotter -DP4OS_SIM_APPS="hello_app;plotter" && cmake --build build-plotter -j8
P4_SIM_UART_B=/dev/ttysNNN P4_SIM_PREFS=/tmp/p.txt P4_SIM_PORTAL_PORT=8156 ./build-plotter/p4os_sim
```

Modes: `sine`, `arduino` (named values with a boot log in front), `csv`
(semicolons and decimal commas), `meter` (`+0265 4`, changing range), `fast`
(a counter to check nothing is lost), `mix`. A pty keeps what nobody read
and hands it over at once: `--delay 3` keeps the device quiet until the
simulator has connected.
