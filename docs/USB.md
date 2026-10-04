# USB on the P4

The board's "OTG" connector is the P4's own USB 2.0 High-Speed port
(480 Mbit/s). The console lives on the other connector (the CH343 UART), so
the OTG port is free for whatever it is asked to be. It is chosen in
**Settings → USB**, which says what each mode does (the pendrive host is a
switch of its own, at the end):

| Mode | What the computer sees | The card |
|---|---|---|
| Off | nothing | on the board |
| Keyboard and mouse | a HID keyboard, mouse and media keys, a gamepad, a MIDI keyboard and a network (the Macro pad, `docs/MACROPAD.md`) | on the board |
| Disk | the microSD as a USB drive | the computer's, until it ejects it |

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

## The pendrive host

The board can also be the host of pendrives, with a switch of its own in
Settings, USB, "Pendrives" (`POST /api/usb {"host": true}`), remembered
across restarts (pref `usb_host`). The OTG connector gives no 5 V, so a
device plugged into it gets no power: a pendrive goes on the 40-pin header
instead, next to 5 V (a USB-A socket on wires, or a cut USB cable), on one
of two ports, chosen in Settings, USB, "Pendrive data lines" (or
`POST /api/usb {"pins": "21/23"}`, pref `usb_hport`):

| Pendrive | Pins 21/23 (default) | Pins 25/27 |
|---|---|---|
| VBUS (red) | pin 1 or 3, 5 V | the same |
| D- (white) | pin 21, GPIO24 | pin 25, USBD_N |
| D+ (green) | pin 23, GPIO25 | pin 27, USBD_P |
| GND (black) | pin 5 | the same |
| Controller | the P4's second one, Full Speed (USB 1.1, 12 Mbit/s) | the High-Speed one (480 Mbit/s), the OTG connector's |
| The OTG connector meanwhile | keeps its mode (off, keyboard and mouse, disk) | unplugged: its lines are the same wires; its mode is off while the host is on |

**Both work, and the wires decide** (2026-10-04, a Kingston DataTraveler
2.0 of 8 GB, FAT32). With wires of about 70 cm, the High-Speed controller
on 25/27 saw the pendrive connect and every port reset failed ("HUB: Root
port reset failed"), at High Speed and forced to Full Speed (`FSLSSupp`)
alike, while 21/23, at Full Speed, mounted it at once. With the same wires
cut under 15 cm, 25/27 mounted it at High Speed: **7.4 MB/s** reading a
1.5 MB photo on the board (`/api/fs/bench`). Through the portal over Wi-Fi
both give about 450 KB/s: there the Wi-Fi is the limit. Full Speed tops at
about 1 MB/s. So: 25/27 with short wires (D+ and D- twisted together is
better still) when speed matters, 21/23 when the OTG connector is busy or
the wires are long.

ESP-IDF's host library drives one root port, so the two ports are not
used at once; behind a hub, though, there can be several pendrives (below).

**Pins 21/23 and the backlight.** The P4 has two FSLS PHYs: PHY 0 on
GPIO24/25, PHY 1 on GPIO26/27. At power-on the USB-Serial-JTAG is on PHY 0
and the Full-Speed controller on PHY 1, and ESP-IDF's host library leaves
it there - but **GPIO26 is this board's backlight PWM**. So the host, on
this port, switches the USB-Serial-JTAG's pads off and swaps the two
(`usb_wrap_ll_phy_select(&USB_WRAP, 0)`, in `LP_SYS`, before
`usb_host_install`, whose reset of the wrap does not touch it), and puts
back the 40 mA drive the PHY driver gave GPIO26/27 thinking the pads were
there. Stopping turns the controller's pads off and swaps back; as
`LP_SYS` survives a software restart, every boot swaps back too (a
constructor in `aos_usb_p4.c`). The USB-Serial-JTAG is not used on this
board (the console is the CH343 UART) and reaches no connector.

The pads go off only after the host library is down. Cut first, with a
pendrive on the port, the port saw a sudden disconnection and the
library's port power-off failed its assert (`hub_root_stop`, hub.c), which
restarted the board (seen on 2026-10-04 moving the host from 21/23 to
25/27 with a pendrive mounted).

On 25/27 the controller also gets VBUS and the A-session valid by
override: this board's VBUS (the OTG connector's and the header's 5 V)
reaches no pin of the P4. (On 21/23 ESP-IDF gives them through the GPIO
matrix.)

The log says the root port's state when it changes (`host port:
connected, enabled, speed, power, A-session...`), for a pendrive that does
not come up. A couple of failed enumerations
(`ENUM: CHECK_SHORT_DEV_DESC FAILED`) while the plug goes in are normal:
the contacts bounce, and the next try works. A pendrive already there when
the host starts (at boot, say) is found too.

The host is ESP-IDF's USB Host Library on the chosen controller, with
Espressif's `usb_host_msc` as its client (`aos_usb_p4.c`); on 25/27,
TinyUSB is uninstalled first. A pendrive that answers as mass storage is
mounted at `/usb` through FATFS: **FAT32 only**, as ESP-IDF has no exFAT,
and many pendrives over 32 GB come in exFAT. Settings says what it found,
or why it did not mount.

**Hubs.** The library's external hub support is on
(`CONFIG_USB_HOST_HUBS_SUPPORTED`): behind a hub there can be up to three
pendrives, at `/usb`, `/usb2` and `/usb3` (FATFS has four volumes: the
card and these). A hub with its own power supply also gives the pendrives
their 5 V. On 25/27 a High-Speed hub runs at High Speed; a Full-Speed
device behind a High-Speed hub needs split transactions, which ESP-IDF
does not do.

The portal sees each pendrive as a folder at the card's root (`usb`,
`usb2`, `usb3`): every file call (list, get with ranges, put, delete,
bench) on `/usb/...` goes to the pendrive. `GET /api/usb` says the OTG's
`mode` (`host` while the host holds the OTG controller, on 25/27),
`host_on`, `pins` and `pendrives`, each with `id`, `vendor`, `product`,
`bytes`, `mounted`, `path` and `error` (and `host`, the first one, as
before). `{"mode": "host"}` still turns the host on.
