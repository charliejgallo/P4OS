# Changelog

## 0.6.0 — 2026-10-03

**The lock screen**
- The time, the date, the messages and the music. A free swipe up unlocks.
- When it locks: at boot, and after the screen has been off for the time
  chosen (as soon as it goes off, or 1 to 60 minutes later).
- An optional code of 4 or 6 digits, kept as a salted SHA-256. Wrong codes
  make the pad wait longer each time.
- A forgotten code is removed from the BOOT button's safe mode.
- Settings, Lock screen: when it locks, the code, and what it shows.

**The workshop**
- **LED Strips**, a new app: an addressable strip on any free pin
  (WS2812B, WS2811, SK6812, SK6812 RGBW), with 13 effects and WLED's power
  limiter. It runs as a service, with the app closed and after a restart.
- **1-Wire in Bus:** it searches the bus and reads every DS18B20 live, and
  sets its resolution.
- An RC522 example in Bus's SPI tab, and SPI tested on the board at 1, 10
  and 40 MHz.
- A real BME280 read on the board.

**System**
- **Languages:** English and German now also live in the firmware. The card's
  pack and the firmware's are joined string by string, so the languages work
  with no card and a card older than the firmware is still complete.
- **The link to the C6 is watched.** Twice, during an OTA, the Wi-Fi died with
  the rest of the board alive, until RESET. Now three Wi-Fi calls to the C6
  in a row with no answer (or one out for over 20 s) log every task and
  restart the board, which comes back with network in under 30 s. Tested by
  freezing the task that takes the C6's replies (`POST /api/wifi/linktest`).
- Diagnostics shows the C6's firmware version, when the C6 reports it.
- Folder miniatures fit glyph and text icons, and the folder-rename
  keyboard is dark like the others.
- The install scripts use IPv4 straight away: the language packs go up in
  seconds, not minutes.
- The CI checks each app with readelf, and a new push cancels the run still
  going.


## 0.5.0 — 2026-10-01

The first public version. Before this, P4OS was built in private from the
day the board arrived (2026-09-28). It starts at 0.5 because much of the base
is already done.

**System**
- Shell with icon pages, folders, a dock, the control centre and
  notifications, in portrait or landscape.
- Apps loaded from the microSD as `.so` files, with their code in PSRAM.
- The panel has three frame buffers that the apps can draw into and flip,
  with the work split across both cores.
- Internal RAM audit: about 290 KB free with everything running.
- OTA through two slots, with rollback and an on-trial boot. The previous
  boot's log and crash dumps can be read from the portal.
- The hang watchdog and the network watchdog.
- The BOOT button: home, a screenshot, and safe mode.

**Connectivity**
- Wi-Fi through the ESP32-C6 (esp_hosted 1.4), mDNS (`p4os.local`), and the
  web portal.
- The board's own access point, with a QR to join it and another for the
  portal.
- USB OTG at high speed: keyboard, media keys, mouse, gamepad, MIDI and a
  network over the cable; disk mode with the microSD.

**Settings**
- Wi-Fi, Bluetooth switch, USB, Display, Sound, Wallpaper, the expansion
  header, Language, Date and time.
- Storage: the card by kind of file, and eject.
- Update: both slots, and going back to the other one.
- Diagnostics: temperature, CPU, reset reason, safe mode, the crash dump.
- About: the portal's addresses, with a QR.

**Apps**
- Built in: Files, Photos, Music, Clock, Calendar, Calculator, Converter,
  Home Assistant, MQTT, Claude, Bus (I2C/SPI/GPIO), Modules, Terminal,
  Programmer, Modbus, Bench, Electronics, Monitor, Network tools and Macro
  pad.
- From the card: twenty-one games, Doom among them, and ten tools: Maps,
  Radio, Cameras, Video, Visor 3D, Pixel Art, Recorder, Tuner, Weather and
  Quotes, plus Lua.

**Languages**
- Spanish, English and German.

**Not yet**
- Bluetooth.
- Real I2C and SPI chips, and the programmer against another ESP32, are
  still to be tried on the bench.
- A rare hang after a software restart.
