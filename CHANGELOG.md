# Changelog

## 0.9.1 — 2026-10-05

**A gamepad in every game** (docs/GAMEPAD.md)
- A USB gamepad or joystick on the host plays every game from its title to
  its last panel: 2043, ARKANOS, Atasco, Blackjack, Buscaminas, Burbujas,
  Chatarra, Claude Jump, Claudito, Dados, Doom, Flappy, Gemas, Golf, Mila,
  Monster Hop, Neon Snakes, Simon, Topos, Truco, Turbo, and Atrapa in Lua.
  The same roles everywhere: A (buttons 1/2), B (3/4), L/R (5-8), START
  (9/10), the D-pad or the stick. On the panels the D-pad moves an outline
  and A presses; it appears only once the pad is used, so touch looks the
  same as before.
- The retro canvas presses its own buttons from the pad and lights them
  (docs/RETRO.md); it was written for 0.9.0 and went in after its tag.
- `aos_pad.h` and `aos_pad_menu.h` (header only, no new firmware) for the
  apps; `aos.pad()` for the Lua scripts.
- The simulator fakes a pad: `P4_SIM_PAD=1` (the keyboard) and the scripts'
  `pad` command.
- A d-pad that the pad reports as the X/Y axes (many cheap ones) is read
  as directions, not as a stick pushed to the end: Turbo turned at full
  lock at a touch.

**Retro canvas**
- The CPU scales it by default: the PPA left the first row of each small
  block wrong, and Atrapa left a trail behind every falling star.
  `retro_hw` in `/api/settings` turns the PPA back on; `/api/sysmon` shows
  the canvas's rate and its scaling cost.

## 0.9.0 — 2026-10-05

**The portal's security** (docs/SECURITY.md)
- Requests from other sites and DNS rebinding are refused: the API answers
  only the portal's own pages, and only to the board's IP or name.
- Trusted networks: on any other Wi-Fi the portal is closed (or asks for
  the password) and the board does not announce itself on mDNS. The USB
  cable always gets in; the board's own network counts as home. The network
  the board is on when this firmware first boots becomes trusted.
- A password for the portal, set only on the board (Settings, Portal web):
  PBKDF2, a growing wait after wrong ones, sessions as `HttpOnly`
  `SameSite=Strict` cookies, a login page and "Cerrar sesión". A token for
  the scripts in `tools/` (`P4OS_TOKEN`).
- `/api/auth`, `/api/login`, `/api/logout`.
- HTTPS on port 443, switched on in Settings, Portal web: the board makes
  its own authority (limited by `nameConstraints` to `.local` names and
  private addresses) and the portal's certificate signed by it, renewed by
  itself. Trusting the authority once on a Mac or an iPhone
  (`/api/tls/ca`, Settings, Security in the portal) takes the browser's
  warning away. TLS 1.2 with AES-GCM first. On untrusted networks plain
  HTTP redirects there; the session cookie is `Secure` over HTTPS.

**USB host**
- Settings, USB, USB host: a switch of its own, beside the OTG connector's
  mode and remembered across restarts. Devices go on the 40-pin header
  (5 V from pin 1), on pins 21/23 (the P4's second, Full-Speed controller,
  moved off GPIO26, the backlight, onto GPIO24/25: it works while the OTG
  connector is a keyboard or a disk), on 25/27 (the OTG connector's
  High-Speed lines: 7.4 MB/s from a pendrive with wires under 15 cm; with
  70 cm the port reset failed), or on both at once: P4OS carries its own
  copy of ESP-IDF's host library (`components/usb`) that drives the two
  controllers as two root ports. Hubs work too, though wiring the devices
  directly is recommended (a hub without its own supply can drop off for a
  moment when something is plugged into it).
- Every device is listed in Settings and `/api/usb`, with what the board
  does with it, also the ones nothing takes.
- Pendrives (FAT32) at `/usb`, `/usb2`, `/usb3`: Files and the portal.
- Keyboards (any, by their HID report descriptor; Latin American or US
  layout, dead-key accents, Caps Lock's LED) type into the open text
  field, and Notas; media keys work anywhere; mice move a pointer (the
  wheel scrolls, right is back); gamepads and joysticks for games
  (`aos_hal_hid_gamepad_get`).
- MIDI keyboards and controllers (`aos_hal_midi_read`/`_send`, and MIDI
  thru to the computer).
- USB serial: Arduinos and CH34x, CP210x, FTDI adapters as ports `usb0`,
  `usb1` of the Terminal.
- Webcams (MJPEG) as cameras of the Cameras app while plugged in.
- USB sound cards and headsets: the board's sound goes there instead of
  its speaker, at the board's volume, and their microphone records in place
  of the board's (Settings, USB, USB audio).
- A Raspberry Pi Pico in BOOTSEL mode is a drive at `/usb`: copying its
  `.uf2` there programs it.
- A device whose enumeration fails is tried again while it stays plugged
  in; serial devices stay open between uses of the port (an FTDI lost the
  first packet after every reopen).
- The host's DMA buffers stay in internal RAM: in PSRAM a serial port read
  garbage.

**Bluetooth**
- The board asks for encryption only when the phone or the computer has not
  started it: the "encryption failed (13)" at every boot is gone.
- `strncasecmp`, `strcspn` and `fprintf` for the apps.

**More apps with their own page**
- **Recorder:** the recordings on the card, to listen to, download or
  delete; the app sees what the page deletes (it looks at its folder every
  two seconds).
- **Weather:** the place chosen with a real keyboard, from Open-Meteo's
  search, and the week's forecast. The app keeps its place in
  `/data/clima_lugar.txt` too, and takes it from there.
- **Quotes:** which rates the board shows, with today's values beside them.
  The app reads its list from `/data/cotiz.txt`; until now there was no way
  to choose it on P4OS.
- The portal serves byte ranges (`Range`, 206): Safari plays audio and video
  from the card, a player seeks without downloading the whole file, and a
  page can read only a file's header.

## 0.8.0 — 2026-10-04

**Notes, a new app**
- Notes with styles (sizes, bold, italic, colours, highlighters, headings,
  quotes, eight kinds of bullets, numbering, checkboxes) and task lists
  (priorities, subtasks, drag to reorder), in an editor and a Spanish
  keyboard of its own. One Markdown file per note in `/sdcard/notas`; a QR
  or the computer's keyboard to share one. Its fonts travel in
  `notas_p4.pak`. apps/notas/README.md.

**The phone: music and actions**
- The control centre and the lock screen show and drive the iPhone's music
  (AMS) when the board plays nothing itself; Settings, Bluetooth, "Música del
  iPhone" turns it on.
- The notification centre has the phone's own actions: answer and reject a
  call, clear a message on the phone. Only the ones the phone declared.
- An incoming call takes the whole screen, over the lock screen too, with
  Reject and Answer, until the phone withdraws it.
- `/api/bt` gains `music`, `media` and what the phone plays.

**Red**
- Every LAN sweep is saved to the card's `/redes` as one NDJSON file, with
  the Wi-Fi networks around scanned at its end (the newest 40 are kept).
- The sweep keeps every host's MAC: lwIP's ARP table holds 10, so it is
  read after every batch of pings and during the port probes. A host that
  answers ARP and ignores everything else (phones, Windows) now counts, as
  "ARP only". `aos_hal_net_arp_table()`, `aos_hal_net_mac()`.
- A Red page in the portal: start a sweep and follow it (`GET`/`POST
  /api/net`), the saved ones as tables with each MAC's maker, what changed
  since the one before (hosts new, gone or moved to another address, ports
  opened and closed, networks), a graph of the channels with the least busy
  of 1, 6 and 11, CSV and the raw file. IEEE's register dropped in
  `/redes/oui.txt` names every maker.

**The apps' own pages in the portal**
- An app on the card can bring its page to the web portal: a JavaScript
  module in its `web/` folder, which `tools/install_apps.sh` puts in the
  card's `/web` and the portal loads at startup. No firmware, no restart; a
  broken one does not break the portal. docs/PORTAL-PAGES.md.
- Notes, Pixel Art, Lua, Maps and the 3D viewer moved their pages out of the
  firmware this way (`app.js` from 5300 lines to 2565).
- New pages: **Radio** (the nine keys by drag and drop, the list, a search
  of radio-browser.info, what plays on the board; `GET`/`POST /api/radio`)
  and **Cameras** (`/cameras.txt` edited by fields, with a preview).

**Settings, Developer**
- Show touches (a ring under every finger the GT911 reports) and fps
  (LVGL's renders plus the apps' flips), over everything; they stay on
  after a restart.
- The log's level: errors, warnings, or everything this build has.
- The drawing preferences of `/api/tune`, by name and with their measures:
  where LVGL draws and how many rows, the panel's buffers, the PPA's copies,
  the games' bands; and back to the factory values.
- Restart, and restart into safe mode without holding BOOT (one boot only).
- Settings keeps a page scrolled where it was when it rebuilds it.

**The release carries more of itself**
- The CI builds the C6's firmware (`c6.bin`, for the card's `/firmware`),
  the colour emoji pack (`emoji.pak`, from Noto's tag
  `v2026-09-24-unicode18_0`) and the Notes app's fonts (`notas_p4.pak`),
  the two packs the same files byte for byte as the ones built by hand, and a tag's
  release takes them along with the firmware, the apps, their portal pages
  (`web.zip`) and the languages. Only the games' art is still attached by
  hand.

## 0.7.0 — 2026-10-04

**Bluetooth and colour emoji**
- Bluetooth with the phone: NimBLE on the P4, the C6's controller through
  esp_hosted. From AmoledOS: pairing by numeric comparison, the iPhone's
  notifications (ANCS), its music (AMS), battery and time. The board shows
  up in the iPhone's Settings > Bluetooth (it advertises the HID service,
  which is what iOS lists there) and the pairing code comes up over
  everything. `GET`/`POST /api/bt`.
- Colour emoji in any text, from Noto's 4012 (sequences, skin tones, flags
  and keycaps included): the card's `/fonts/emoji.pak`, built by
  `tools/gen_emoji.py` and put there by `tools/install_emoji.sh`. Banners
  and the notification centre now go through the same text filter as the
  lock screen. docs/EMOJI.md.
- **Bluetooth keyboard mode:** the board as a computer's keyboard, mouse and
  media keys (HID over GATT), with the phone connected at the same time. The
  Macro pad, the portal and the apps send keys over Bluetooth when there is
  no USB cable, and the Macro pad says BLE. Settings has a Bluetooth page:
  the phone, its battery, forgetting it, the keyboard mode and the computer.
  docs/BLUETOOTH.md.

**The ESP32-C6, updated from the P4**
- esp_hosted 3.0.9 on the P4 (it was 1.4.7), with esp_wifi_remote 1.6.5.
  It still talks to the C6's factory firmware, and its own options put the
  SDIO buffers and its task stacks in PSRAM.
- `c6/` is the C6's firmware, esp_hosted 3.0.9 with Wi-Fi and BLE, built by
  `tools/build_c6.sh`. The P4 installs it from the card: Settings, Update,
  or `tools/install_c6.sh`. No cable, no programmer.
- The way back too: `tools/c6_app_from_flash.py` takes the factory app out
  of a backup of the C6's flash, and the P4 sends it the same way.
- docs/C6.md: versions, the update, the measured throughput, and the
  serial recovery through J7.
- The C6's Wi-Fi buffers are the P4's `CONFIG_WIFI_RMT_*`: 32 TX buffers,
  or downloads fall to 0.4 MB/s with a new C6.
- `POST /api/wifi/ap` switches the access point, `GET /api/fs/bench` times
  a read from the card, `GET`/`POST /api/c6` show and start a C6 update.

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
