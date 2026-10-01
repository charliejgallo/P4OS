# USB on the P4

The board's "OTG" connector is the P4's own USB 2.0 High-Speed port
(480 Mbit/s). The console lives on the other connector (the CH343 UART), so
the OTG port is free for whatever it is asked to be. It is chosen in
**Settings → USB**, which says what each mode does:

| Mode | What the computer sees | The card |
|---|---|---|
| Off | nothing | on the board |
| Keyboard and mouse | a HID keyboard, mouse and media keys, a gamepad, a MIDI keyboard and a network (the Macro pad, `docs/MACROPAD.md`) | on the board |
| Disk | the microSD as a USB drive | the computer's, until it ejects it |
| Host | - | not on this board: the OTG port gives no 5 V |

The mode is remembered across restarts (pref `usb_mode`,
`aos_hal_usb_restore()` once the boot has read the card): after an OTA the
port is back as a keyboard 4 s after boot. Disk mode is not remembered: the
boot reads the card, so a restart in disk mode comes back in the mode it had
before.

Opening the Macro pad switches an idle port to keyboard and mouse. It does
not take the port away from disk mode: the computer has the card then, and
taking it back is for Settings to do.

The code is `components/aos_hal/aos_usb_p4.c`. Every step goes to the log,
which the portal shows (**Registro**, filter `usb`).

## Keyboard mode

One composite device (VID 0x303A, PID 0x402C, class MISC/IAD), six
interfaces, all at High Speed:

| Interface | What | Notes |
|---|---|---|
| HID | keyboard, media keys, mouse | report ids 1-3 |
| HID | gamepad: one stick, hat, 32 buttons | an interface of its own, see below |
| CDC-NCM (two) | a network over the cable | the board is 192.168.7.1 |
| MIDI (two) | board → computer only | one IN endpoint, see below |

The Macro pad drives all of it: its pages, the trackpad, and two faces for
the gamepad (**Mando**) and the piano (**MIDI**).

Measured on a Mac on 2026-09-30:

| | |
|---|---|
| Network | the Mac gets 192.168.7.2 by DHCP (`en9`), ping 0.5 ms; the portal downloads at **6.75 MB/s** (1.7 over Wi-Fi; 7.4 while TinyUSB's buffers were internal) |
| Internet | stays on the Mac's Wi-Fi: the DHCP offer has no router and no DNS |
| mDNS | `p4os.local` resolves to 192.168.7.1 first. `curl` without `-4` waits 5 s for an IPv6 address the board does not have; browsers do not |
| MIDI | 14 notes, a two-finger chord, 48 bends and 43 mod wheel (CC 1) messages, all received in order, none stuck; CC 123 on leaving the face |
| Gamepad | seen by the browser's Gamepad API as a gamepad |

Three things the first plug-in taught:

- **The notification interval at High Speed.** TinyUSB's NCM template sets
  the notification endpoint's interval to 50, which is frames at Full Speed
  but an exponent (1..16) at High Speed. The Mac never configured the
  device. It is 9 now (32 ms).
- **MIDI in one direction.** A High Speed bulk endpoint is 512 bytes, and
  esp_tinyusb fixes the MIDI receive buffer at 64 (`CFG_TUD_MIDI_RX_BUFSIZE`,
  not a Kconfig option): opening the OUT endpoint failed, and the Mac's
  SET_CONFIGURATION got a stall. The pad only sends, so the port has one
  IN endpoint (`MIDI_OUT_DESCRIPTOR`). `tud_midi_mounted()` wants both
  directions, so readiness is the keyboard's.
- **The gamepad on its own interface.** As a fourth report of the keyboard,
  macOS saw a keyboard with a gamepad inside: nothing listed it, and opening
  a keyboard needs the Input Monitoring permission. Alone it is a plain
  gamepad (`CONFIG_TINYUSB_HID_COUNT=2`).

The NCM buffers are TinyUSB's, four each way, in PSRAM.

## Disk mode

Choosing it:

1. closes every other app, so nothing has a file open on the card;
2. leaves two files on the card for macOS (below);
3. lets go of the card: the player stops, FAT is unmounted, the SDMMC host
   shut (`aos_hal_sd_release`);
4. initialises the card again with no filesystem (`aos_p4_sd_card_open`) and
   hands its sectors to esp_tinyusb's MSC storage.

While the computer has it, the card's apps do not open (a toast says why)
and `aos_hal_sd_present()` is false.

**Ejecting it on the computer is the way back.** esp_tinyusb mounts the card
back for the board when the computer ejects it (or goes away), and that ends
disk mode: the port goes back by itself to the mode it had before, and the
card is mounted the usual way. The card is never on both sides at once.
Choosing another mode in Settings also ends it, but without the eject the
computer loses the disk under its feet (it warns so).

Measured on a Mac on 2026-09-30, 16 GB card:

| | |
|---|---|
| Enumeration | High Speed, "SDCARD" 15.9 GB; macOS asks to allow the accessory once (another product id than the keyboard: 0x4002) |
| Read | 8.2 MB/s (a 37 MB pack in 4.5 s) |
| Write | 4.8 MB/s (64 MB) with the 32 KB buffer in PSRAM; 3.3 with 8 KB internal |
| Integrity | 64 MB written by the Mac, read back by the board through the portal: same SHA-256 |
| Eject → card back on the board | at once; the board mounts it again in ~60 ms |

The transfer buffer is TinyUSB's, 32 KB in PSRAM
(`CONFIG_TINYUSB_MSC_BUFSIZE`; TinyUSB's buffers live in PSRAM since the
internal RAM audit, `docs/MEMORY.md`).

### macOS and the card

- **Spotlight.** A Mac indexes every volume it is given. The first time,
  the eject was refused for minutes ("dissented by mds"). Entering disk mode
  now leaves `.metadata_never_index` and `.fseventsd/no_log` at the card's
  root, and the eject went through at once.
- **AppleDouble files.** Copying from a Mac leaves `._name` next to each
  file. Everything on the board that lists the card skips names starting
  with a dot (apps, icons, music, photos, maps, videos, Lua); the portal's
  file browser shows them, so they can be deleted.
- **VBUS is not monitored,** so pulling the cable without ejecting looks to
  the board like a suspended bus, not an unplug: the card stays on the USB
  side until a mode is chosen in Settings.
