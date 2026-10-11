# BLE

A Bluetooth LE scanner and analyser for P4OS: who is around, how strong,
what each one says in its advertisements, the readings thermometers and
the like broadcast, and, for the devices that take a connection, their
GATT services. The id is `aos.ble`; its page in the portal is `#ble`.

It needs the firmware's scanner and GATT client (`aos_hal_ble_*`,
`components/aos_ble/aos_ble_scan.c`, docs/BLUETOOTH.md "The apps' scanner"),
which came with it. Bluetooth has to be on; the app offers to switch it on.

## The tabs

- **Cerca** (near): a row per device, sorted by signal, name or newest, and
  filtered (named, favourites, connectable, sensors, beacons, Apple). Each
  row: what it seems to be (a glyph in its class's colour), its name or a
  label made from what it advertises ("AirPods", "Mouse (Swift Pair)",
  "Termómetro Govee"), the company, the kind of address, the last 30
  seconds of signal and the signal now. A sensor shows its readings instead.
- **Radar**: everyone heard in the last 30 seconds at an estimated
  distance, on a logarithmic scale from 30 cm to 30 m. The angle means
  nothing (Bluetooth has no direction): it is the address's hash, so a dot
  keeps its place. A tap picks one; from there, the finder.
- **Sensores**: a card per device broadcasting readings in a known format,
  with the last two hours of temperature.
- **Aire**: packets a second and devices over two minutes, what kinds of
  things, which companies, which kinds of address (public, random static,
  private resolvable, private non resolvable) and of advertisement, and how
  the signals spread.

A device opens its **detail**: name, class, company, address and its kind;
the signal (now, min, mean, max, two minutes as bars) and the rhythm (the
interval between advertisements, how many were heard, how often the bytes
changed); a sensor's or a beacon's values; and every AD structure of the
advertisement and of the scan response explained line by line, with the
raw bytes. From there:

- **Buscar** (the finder): the signal in big numbers, smoothed over the
  strongest of every two seconds, whether it is getting stronger, the last
  minute as bars, and a beep that comes faster the closer it is.
- **Conectar** (the GATT explorer), when it advertises as connectable: its
  services and characteristics with their names, what each allows, values
  read (in words for the known UUIDs, as text, and in hex), writes (text,
  or bytes as `0x01 A0`), notifications and indications as they come, and
  "Leer todo", which reads every readable attribute in turn. No pairing:
  the store of bonds is the phone's, and a characteristic that wants an
  encrypted link says so.
- A **star** and a **name** of our own, kept in `ble/nombres.txt` on the
  card (`AA:BB:CC:DD:EE:FF|*|Heladera`), which the portal edits too.

## What it decodes

`main/bl_decode.c` and `main/bl_names.c`, pure C, tested on the Mac by
`test/run.sh` (packets written by hand from each format's spec, the
simulator's, and a fuzzer under ASan/UBSan):

- Every AD type of the Core Specification Supplement, flags spelled out.
- Companies (the SIG's identifiers people meet), 16-bit UUIDs (services,
  characteristics, descriptors, members), well known 128-bit ones (Nordic's
  UART...), appearances.
- Sensors: BTHome v1 and v2, pvvx and ATC1441 (Xiaomi thermometers with
  custom firmware), Xiaomi's MiBeacon when not encrypted, Govee, Ruuvi
  (RAWv1 and RAWv2), SwitchBot, Qingping, Inkbird, Eddystone TLM.
  Encrypted payloads are said to be so, never guessed.
- Beacons: iBeacon, AltBeacon, Eddystone UID, URL, TLM and EID.
- Apple's Continuity messages (Nearby Info, Handoff, AirPods, Find My...),
  Microsoft's Swift Pair, Google's Fast Pair.
- GATT values of the known characteristics (battery, temperature,
  humidity, pressure, heart rate, Device Information, PnP ID...).

### Sensors that encrypt

A stock Xiaomi thermometer (LYWSD03MMC and its kin, MiBeacon v4/v5) and
BTHome v2 devices set up with a key advertise their readings encrypted with
AES-CCM. With the device's 16-byte key the app decrypts them
(`main/bl_crypt.c`, plain C: the firmware's mbedTLS is not in the apps'
symbol table) and they read like any other sensor: the readings, the
history, the CSV and MQTT.

- **The key** goes in from the device's detail ("Cargar la clave", 32 hex
  digits) or from the portal, and is kept in `ble/claves.txt` on the card
  (`AA:BB:CC:DD:EE:FF|<32 hex>`). The detail says whether it matches.
- **Xiaomi's bindkey** comes from the Mi Home account the sensor is paired
  with (tools such as "Xiaomi Cloud Tokens Extractor"), or from Home
  Assistant if it already reads the sensor. MiBeacon v2/v3 (12-byte keys)
  is not supported, and the detail says so.
- **Checked** against FIPS-197 and NIST SP 800-38C (AES, CCM with 7, 8 and
  12-byte nonces), bthome.io's encryption example, and a MiBeacon v5
  packet made with pycryptodome and Home Assistant's nonce; the simulator
  has a stock Xiaomi that encrypts with the test key 00 01 02 .. 0F.

The distance is the log-distance path loss model: the power at a metre is
the beacon's calibrated one when it says it, the advertised TX power minus
41 dB when it says that, and -59 dBm otherwise; the exponent depends on the
surroundings chosen in the settings (2.0 outdoors, 2.7 a house, 3.3 an
office). Walls and bodies move it a lot: it is an estimate, and the screens
say so.

## How many

The table keeps up to **2048 devices**: it starts with room for 256 and
doubles as it fills (~0.8 KB each, in PSRAM), and past the top the one not
heard for longest makes room (never a favourite). At a fair or a station
that happens fast: phones change their private address every few minutes,
and each new address is a new device. "Seen since launch" counts them all,
forgotten ones too, and Airwaves says how many were forgotten. Looking a
device up is a hash of its address, not a walk of the table. Two hours of
history are kept for 64 sensors at most. In the simulator,
`P4_SIM_BLE_CROWD=900` adds that many phones that change their address
every minute: ~550 packets a second, none lost.

The portal's live table carries what fits in one answer of the live
channel (64 KB, about 175 devices), the strongest first, and says so.

## Settings

The cog on every tab: active or passive scanning (active asks scannable
devices for their scan response, where many say their name; passive never
transmits), the share of time listening (10 to 100 %: the C6 has one radio
for Wi-Fi and Bluetooth, and listening all the time slows the Wi-Fi), the
surroundings for the distance, a minimum signal, hiding the ones gone, the
finder's sound, and keeping the sensors:

- **CSV**: a line a minute per sensor in `ble/sensores-<day>.csv`
  (`ble/sensores-sin-hora.csv` before the clock is set).
- **MQTT**: `<board>/ble/<address>` with the readings as JSON, once a
  minute, through the MQTT app's connection.

Both go on with the app in the background (`AOS_APP_FLAG_BACKGROUND`);
without them the scan stops when the app leaves the front.

## The portal

`web/ble.js` (`#ble`): the state and the same settings, the air's two
minutes, the table (search, filter by class, sort by any column, download
as CSV or JSON), the sensors, a device's detail with its packets explained
and raw, its signal and its temperature, a star and a name, and the CSVs on
the card. Live through `/api/live` while the app is open on the board.

The **GATT explorer** is on the page too, for a device that takes a
connection: connect, "read all", and per characteristic read, write (text,
or bytes as `0x01 A0`) and listen to notifications, on the board's own
connection (`gatt=<address>`, `gatt_readall`, `gatt_read`, `gatt_sub`,
`gatt_write` over the live channel; the app puts the table as `gatt`, three
times a second while it is open). It goes on with the board's screen
elsewhere or locked. Tried on 2026-10-10 with an HLK-LD2410 presence radar:
connected in ~4 s, its UART bridge listed (0xFFF2 to write, 0xFFF1 to
listen), and after the documented permission command (default password
`HiLink`) its report frames came in as notifications, ~11 a second.

## Building and trying it

    tools/build_apps.sh ble
    NORESTART=1 tools/install_apps.sh p4os.local ble

In the simulator, `sim/ble_sim.c` makes up a neighbourhood: one thermometer
of each format, beacons, phones, earbuds, a Mac, a mouse waiting to pair, a
watch, trackers, someone walking about and someone passing by, and two that
take a connection (a heart rate strap and an ESP32 with environmental
sensing and a UART that answers in capitals). Nothing there is anybody's
real device.

    cd sim && cmake -B build-ble -DP4OS_SIM_APPS="hello_app;ble" && cmake --build build-ble -j8
    P4_SIM_PREFS=<scratch>/prefs.txt P4_SIM_PORTAL_PORT=8093 \
      P4_SIM_SCRIPT="wait 1000; open aos.ble; wait 4000; shot <scratch>/a.png; quit" ./build-ble/p4os_sim

(The simulator's Bluetooth starts on, unlike the board's; `bt_on=0` in its
preferences tries the app's "Bluetooth apagado" card.)
