# Changelog

## Unreleased

- **A crash's core dump says which firmware made it.** The board reads the
  dump's own notes (IDF's summary fails here: it maps the whole partition)
  and gives the firmware's ELF sha, whether that is the firmware running now
  or the other OTA slot's, and its version, plus the crashed task, pc, ra and
  cause. The sha is 16 characters now (it was cut at 9).
  `tools/coredump.sh` decodes only against `build/elf/<sha>.elf` and refuses
  when that ELF is not there, instead of trying others: an old dump read
  against today's code pointed at the wrong lines.
- A new dump is noted at boot as unread, with the time it was first seen:
  the log and the screen say so once, and the portal's Registro and Firmware
  pages until it is downloaded, marked read or erased.
- `POST /api/coredump/test`: a panic on purpose, to try all of it.

## 0.13.0 — 2026-10-08

**RF reads the Meshtastic message, not just measures the packet.** Only the
RF app changed; no firmware update needed over 0.12.

- **Tap a LoRa packet to read its Meshtastic frame** (apps/rf/README.md):
  the decoder demodulates the packet (sync, dechirp, Hamming, CRC) and reads
  the Meshtastic header that travels in the clear - who to whom, hops. With
  the channel key it decrypts the payload (AES-CTR, Meshtastic's open
  scheme) and shows a text message. The key is the user's to give - the
  public default, or one typed on the board or pasted in the portal
  (`rf/mesh_key.txt` on the card); none are built in or kept in the sources.
  Direct messages encrypted to another node's public key are not read (that
  needs the node's private key). Closed the loop on the board between two of
  the user's own nodes.
- **Decode on the board fixed:** the same decoder worked on the Mac but
  always failed on the board. Two 32-bit/embedded traps: a sample count
  times the bandwidth overflowed a 32-bit `long` (it is 64-bit on the Mac),
  truncating the signal so sync never caught the preamble; and `cosf`/`sinf`
  were called with large arguments, where the board's single-precision libm
  loses accuracy and smeared the reference chirp. Fixed with 64-bit maths and
  by keeping those angles small.

## 0.12.0 — 2026-10-08

**BLE, a Bluetooth LE scanner on the card,** with the firmware it needs
(`aos_hal_ble_*`) and fonts that draw more than Latin-1; and RF measures
LoRa. Update the firmware first: BLE uses functions 0.11 does not have.

- **BLE, a Bluetooth LE scanner and analyser on the card**
  (apps/ble/README.md): who is near and what each one is, a radar by
  estimated distance with a finder that beeps, the readings thermometers
  broadcast (BTHome, pvvx/ATC, MiBeacon, Govee, Ruuvi, SwitchBot, Qingping,
  Inkbird, Eddystone TLM) with two hours of history, CSV and MQTT, the
  statistics of the air, a device's advertisement explained structure by
  structure, a GATT explorer, favourites and names, and its page in the
  portal. Encrypted sensors (stock Xiaomi MiBeacon v4/v5, BTHome v2) are
  read with their key, kept in `ble/claves.txt`. If a link drops while it
  scans, it says so. Needs this firmware.
- **Bluetooth LE for apps** (docs/BLUETOOTH.md): `aos_hal_ble_*`, a raw
  scanner into a PSRAM ring and one GATT connection of the app's own,
  next to the phone and the computer (NimBLE now allows 3 connections).
- **Fonts:** typographic punctuation, symbols (currencies, arrows, maths,
  check marks...), Latin Extended-A and Greek in the text sizes; a name
  with a typographic apostrophe, as a Mac's own, no longer shows a box.
  `aos_text_safe` asks the font what it can draw, and the phone's and the
  computer's names go through it.
- **RF measures LoRa** (apps/rf/README.md): each packet in the band - when,
  where, how long, how strong - with the spreading factor and bandwidth its
  preamble shows, named after a Meshtastic preset when it is one, and the
  channel's busy share over the last minute. Found on synthetic packets of
  five presets at three sample rates, a weak one too, and on the board with
  two Meshtastic nodes: every message and its acknowledgement as MediumFast,
  the band's hopping neighbours kept off the list. Bands has Meshtastic's
  default channel of a region and preset (ANZ and MediumFast: 926.125 MHz).
- **RF's controls are one bar:** mode, bands, settings, speaker and keeping
  things, each a sheet; the spectrum and waterfall got the room.

## 0.11.1 — 2026-10-07

RF, after trying it with real remotes on the board. Only the app changed:
`rf.so`, its page and its language packs.

- **Real remotes decode whole.** With a copier remote the codes came out
  cut anywhere (21, 23 or 28 bits of a 24-bit code). The detector's noise
  floor sat at the noise's low quantiles, so a real stick's noise opened
  trains by itself and kept them open, and a remote that came in the middle
  was cut. The floor is now the noise's mean, and a train that does not
  stand 4 dB over it is let go. Two remotes, three codes and eight buttons
  came out right on the board; weak signals are found better too (18 dB
  down: 6/6, 4/4 and 3/3 repeats, from 2/6, 3/4 and 3/3).
- A code valid as both **EV1527 and PT2262** is named by its data pins, and
  its details show the other reading.
- Two buttons of one remote pressed within 2 s are two lines, and the tail
  of the last repeat is no longer listed as something unknown.
- **Tapping the spectrum** tunes to the signal near the finger, and the
  scale's frequencies can be tapped.
- **Volume and mute** in the app (a speaker button) and on its page; the
  frequency keypad is dark, as the system's others.

## 0.11.0 — 2026-10-07

**RF, a software radio on the card** (apps/rf/README.md). Needs this
firmware: it uses the raw USB, live data and esp-dsp it brings.

- **RF, a new app: the spectrum and waterfall of an RTL-SDR** (an RTL2832U
  stick) on the USB host. Drag or tap to tune, type a frequency, bands,
  gain, sample rate and step. At 2.4 Msps on pins 25/27 it moves 4.8 MB/s
  without losing a sample, 25 frames a second. Its driver is librtlsdr,
  inside the app (GPL, as Doom's engine): `apps/rf/README.md`.
- **Raw USB devices for apps** (`aos_hal_usb_raw_*`): an app can drive a
  device none of the board's drivers takes - control and bulk transfers,
  and a stream into a PSRAM ring. In the simulator, the Mac's own device
  with `P4_SIM_USB=1` (`docs/USB.md`, "Raw devices").
- **RF listens:** broadcast FM, AM (the airband) and narrow FM (amateurs,
  PMR, marine), with a squelch, through the speaker or a USB sound card,
  and on with the screen locked or another app in front. 36-40 % of core 0
  at 240 ksps with the spectrum running.
- **RF reads 433 and 868 MHz:** the Data mode lists the remotes and
  sensors around (EV1527, PT2262, Nexus, Prologue by name; anything else by
  its modulation, widths and bits), keeps them in a CSV of the day and can
  publish them over MQTT.
- **RF keeps things:** a screenshot, the audio as WAV, the raw signal (I/Q)
  on the card, and plays a recording back as if it were the radio.
- **RF in the portal** (`#rf`): the spectrum and its waterfall in the
  browser, tuning by clicking, the modes and settings, Data mode's list, the
  recordings to play, download or delete.
- **Live data for the apps' pages** (`/api/live`, `aos_hal_live_*`): an app
  puts values and takes messages in memory, for what changes too often for
  a file on the card.
- **esp-dsp for the apps:** the firmware carries it and lends its 16-bit
  SIMD routines (dot products, decimating FIRs, FFTs) and biquads; on the
  P4 a float multiply-add costs ~5 cycles whatever the code
  (`docs/APPS-P4.md`, "DSP").
- Apps that draw straight to the panel (Video, Doom, Mapas...) no longer
  draw over the lock screen when they are open under it
  (`AOS_UI_OVER_LOCK`).

## 0.10.1 — 2026-10-06

- **A crash while booting, about one boot in 18, is gone.** It showed only
  after an OTA (the trial image was rolled back), but any restart could fall
  over during the card's app scan and start again by itself. The app loader
  synced the whole cache after each `.so` with the other core drawing the
  boot screen; it now syncs the module's own block by address range. 4 bad
  boots in 70 before, none in 107 after (docs/MEMORY.md).
- Network, Wi-Fi: the channel graphs and the live signal are drawn once into
  a picture when their data change, so the page scrolls smoothly over them.
- `tools/boot_loop.sh`: restarts the board over and over and keeps the log
  and the core dump of every boot that falls over.
- When the network does not come back after a restart, the log is saved to
  the card (`/logs/sin-red-N.txt`) before the board tries again: the cure
  that works, a power cycle, wipes the log kept in PSRAM.

## 0.10.0 — 2026-10-06

**The workshop on the card** (docs/MODULES.md): four apps that use the
drivers of 0.9.2, each with its wiring drawing on the board and its page in
the portal.
- **EEPROM:** reads, writes and edits serial memories: 24xx (I2C), 25xx
  and 25Qxx flash (SPI), 93xx (Microwire). Finds the chip and measures its
  size by writing, a hex editor with find and undo, versions with CRC32 and
  MD5, a compare, and the protection pins explained per family.
- **PWM:** seven channels on any free pins, 1 Hz to 20 MHz, a knob,
  patterns (ramp, breathing, strobe, sine), servo pulses in microseconds and
  an analog level through an RC filter.
- **Infrarrojo (Infrared):** a learning remote: NEC, Samsung, Sony, JVC,
  Panasonic, RC5, RC6 or raw, remotes drawn from their buttons, and
  SmartIR's library of TVs, air conditioners, fans and lights in a pack
  (`infrarrojo_p4.pak`, MIT).
- **CAN:** a bus analyser on the TWAI controller: frames, by id, sending,
  signals decoded from a DBC, recording to the card. Opens in listen only.
- Tried on the bench on 2026-10-05, with drawings and photos in
  docs/MODULES.md: PWM at 998 Hz, 5 kHz and 50 Hz at once, a servo end to
  end, the analog level at 1.34 V asked and 1.313 V measured; a 12-LED
  WS2812B strip on 3.3 V data; a monitor's remote learned (Samsung 0x0707)
  and sent back, and SmartIR's "volume up"; a 24LC256 found, sized, read,
  edited, written and restored, its CRC32s matching on a computer; CAN in
  its self test (the real bus waits for the transceivers).

**Two more apps from the card**
- **Dibujo (Drawing):** up to eight layers, brushes, shapes, text, fill,
  selections, a grid with a magnet, rulers, symmetry, deep undo; by finger,
  USB mouse or joystick; PNG out, pictures in to trace over.
- **VNC:** a computer's screen on the board. RFB 3.3 to 3.8 (and Apple's
  3.889), VNC passwords, Tight with JPEG on the P4's engine, ZRLE, Hextile,
  CopyRect. Touch, trackpad, or a USB mouse and keyboard; a keyboard bubble
  that floats over the remote screen and is dragged anywhere; one monitor of
  several, from the server's list (ExtendedDesktopSize) or marked with a
  finger, which then costs only its own pixels. Tried against a MacBook Air
  with two monitors.

**The system**
- Settings: **Restart** at the bottom of the list.
- A hang now ends in a panic instead of a plain restart, so it leaves a
  core dump to read.
- `aos_ui_hwmouse_handler()`: an app takes the USB mouse's own reports
  (its buttons and wheel), as VNC, Dibujo and PWM's knob do.
- A header pin let go rests at its off level through its pull; a servo
  switched off no longer moves on a line left weakly high.
- The new apps' names in the system's packs (Drawing, Infrared), and their
  strings in the catalogs built into the firmware.

## 0.9.2 — 2026-10-05

**The Programmer over the USB host** (docs/USB.md)
- The host's USB serial ports (`usb0`...) are in the Programmer's list,
  in the app and the portal, and an ESP32 dev board goes into its
  bootloader by itself through RTS and DTR, as esptool does. A C3, C6, S3
  or H2 on its own USB (USB-Serial-JTAG) gets the sequence its USB logic
  decodes. Tried with an ESP32 on a CP2102 (6.6 s) and an ESP32-C3 (1.5 s).
- The Programmer waits up to 3 s for a USB port that is away (a chip that
  restarts comes back as a new device).
- Espressif's USB-Serial-JTAG no longer shows a "?" after its name.
- docs/SECURITY.md: reaching the portal from outside home through a
  Tailscale subnet router (Home Assistant's add-on), with nothing on the
  board.

**The workshop's drivers** (docs/MODULES.md), for the apps of 0.10
- `aos_io_pwm_*`: PWM on any header pin by the LEDC, seven channels, servo
  pulses, hardware fades.
- `aos_io_dac_*`: an analog level through an RC filter, by the
  sigma-delta modulator (the P4 has no DAC).
- `aos_io_ir_*`: raw infrared in (a demodulating receiver) and out (an IR
  LED and a transistor, the carrier settable), by the RMT.
- `aos_io_can_*`: CAN by the TWAI controllers, normal, listen only and a
  self test that needs no transceiver.
- `POST /api/expansion/selftest` tries them all on a free pin.
- The simulator emulates serial EEPROMs (24xx, 25xx, 93xx), loops IR back
  with a remote in the room, and has a car on its CAN bus.

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
