# Changelog

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
