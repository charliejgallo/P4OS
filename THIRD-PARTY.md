# Third-party code, fonts and data

P4OS is MIT ([LICENSE](LICENSE)). This file lists what in the repository, or
in what its builds pull in, belongs to someone else, under which licence, and
what that asks for. The full texts are in [LICENSES/](LICENSES).

**Kinds of item:**
- **Vendored:** the source is in this repository.
- **Fetched:** the ESP-IDF component manager downloads it at build time into
  `managed_components/`, which is not tracked.
- **Runtime:** loaded from the internet by the board or the portal, never
  shipped.

## In the firmware

| What | How it gets here | Licence | Notes |
|---|---|---|---|
| ESP-IDF 5.5 (FreeRTOS, lwIP, mbedTLS, FatFs, newlib, cJSON…) | the SDK | Apache-2.0, with its third-party parts under their own licences | see ESP-IDF's `COPYRIGHT.rst` |
| LVGL 9.5.0 | fetched | MIT | [MIT-LVGL.txt](LICENSES/MIT-LVGL.txt) |
| qrcodegen (inside LVGL) | fetched | MIT, Project Nayuki | [MIT-qrcodegen.txt](LICENSES/MIT-qrcodegen.txt); the QR codes in Settings |
| stb_image (inside LVGL) | fetched | MIT or public domain | progressive JPEG in `aos_hal/aos_image_p4.c` |
| Waveshare BSP for this board, Espressif display, touch and codec helpers | fetched | Apache-2.0 | |
| esp_lcd_hx8394 (Waveshare) | fetched | MIT | its licence file has the MIT template with the name left blank |
| esp_hosted 1.4, esp_wifi_remote | fetched | Apache-2.0 | Wi-Fi through the ESP32-C6 |
| onewire_bus 1.1 (Espressif) | fetched | Apache-2.0 | 1-Wire over the RMT, for the DS18B20 |
| mdns, esp_tinyusb, esp-serial-flasher, esp_h264 | fetched | Apache-2.0 | esp_h264's decoder is tinyh264; the flasher's stubs are Apache-2.0 or MIT |
| esp-dsp 1.8 (Espressif) | fetched | Apache-2.0 | its 16-bit SIMD filters and FFTs, lent to the apps (RF's demodulators) |
| TinyUSB | fetched | MIT | [MIT-TinyUSB.txt](LICENSES/MIT-TinyUSB.txt) |
| USB host class drivers (Espressif esp-usb): usb_host_msc, usb_host_cdc_acm with its CH34x, CP210x and FTDI drivers, usb_host_uvc, usb_host_uac | fetched | Apache-2.0 | pendrives, serial ports, webcams and sound cards on the USB host |
| esp_new_jpeg | fetched | **Espressif MIT**: for use on Espressif products | [Espressif-MIT-esp_new_jpeg.txt](LICENSES/Espressif-MIT-esp_new_jpeg.txt) |
| esp_audio_codec (prebuilt, AAC for the radio) | fetched | **Espressif Modified MIT**: use and redistribution only with Espressif products | [Espressif-Modified-MIT-esp_audio_codec.txt](LICENSES/Espressif-Modified-MIT-esp_audio_codec.txt) |
| libpng, zlib | fetched | libpng licence, zlib licence | [libpng.txt](LICENSES/libpng.txt), [zlib.txt](LICENSES/zlib.txt) |
| **elf_loader 1.3.3** (Espressif, esp-iot-solution `5d75f3f`) | **vendored**, `components/elf_loader/` | Apache-2.0 | **modified**: the changes are marked `AmoledOS:` / `P4OS:` in the source; [Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| **minimp3** (lieff) | **vendored**, `components/aos_hal/minimp3/` | CC0-1.0 | one change, marked `AmoledOS:` (the caller's scratch buffer) |
| **ESP-IDF's USB host library** (`components/usb` of release/v5.5, `c94f345e`) | **vendored**, `components/usb/` | Apache-2.0 | **modified**: two root ports at once, retries of failed enumerations, a hub fix; the changes are in [components/usb/P4OS.md](components/usb/P4OS.md) and marked `P4OS:` in the source; [Apache-2.0.txt](LICENSES/Apache-2.0.txt) |

The Espressif MIT and Modified MIT licences cover Espressif hardware only.
P4OS's MIT licence does not extend to those two components. A port to other
hardware has to replace them.

## In the apps on the card

| What | Where | Licence | Notes |
|---|---|---|---|
| **doomgeneric** (`dcb7a8d`), Chocolate Doom 2.2.1's music player, DOSBox's DBOPL | **vendored**, `apps/doom/main/doomgeneric/` | **GPL-2.0-or-later** | modified, every change listed in [apps/doom/README.md](apps/doom/README.md). `doom.so` as a whole is GPL-2.0; the repository at the release's tag is its source. **No WAD is included.** |
| **librtlsdr 2.0.3** | **vendored**, `apps/rf/main/rtlsdr/` | **GPL-2.0-or-later** | two small changes, listed in [apps/rf/README.md](apps/rf/README.md). `rf.so` as a whole is GPL-2.0-or-later; the repository at the release's tag is its source. |
| **Lua 5.4.8** | **vendored**, `apps/lua/main/lua/` | MIT, Lua.org, PUC-Rio | one patch to `luaconf.h`, recorded in `VENDOR.md`; the notice stays in `lua.h` |
| compact AES (forward cipher), after **tiny-AES-c** (kokke) | **vendored**, `apps/rf/main/aes.c` | public domain (the Unlicense) | the standard AES S-box and schedule, generalised to a runtime key size, for `rf_mesh.c`'s AES-CTR (reading a Meshtastic channel the user has the key to) |

The firmware that loads `doom.so` and `rf.so` stays MIT. The GPL applies to
the Doom and RF apps and to any simulator build that compiles them in.

The simulator links the Mac's libusb (LGPL-2.1, from Homebrew, dynamically)
when it is installed, for `P4_SIM_USB=1`; it is not part of anything the
board runs.

The LED Strips app's Fire effect follows Mark Kriegsman's Fire2012 algorithm
(as published with FastLED, MIT), written again in `aos_io/aos_leds.c`.

## Fonts and icons

| What | Where | Licence | Notes |
|---|---|---|---|
| **Inter 4.1** (Medium, SemiBold) | converted to LVGL bitmaps in `components/aos_fonts/aos_inter_*.c` by `tools/gen_fonts.py` | SIL OFL 1.1 | [OFL-1.1-Inter.txt](LICENSES/OFL-1.1-Inter.txt) |
| Font Awesome 5 Free glyphs (LVGL's symbols) | merged into the Inter files, from LVGL's `built_in_font` | fonts under SIL OFL 1.1 | [FontAwesome5-Free.txt](LICENSES/FontAwesome5-Free.txt) |
| **Rubik Medium** (Google Fonts) | `apps/mila/tools/blender/fonts/`, the casita's name plate in Mila's art pack | SIL OFL 1.1 | its `OFL.txt` is next to it |
| **JetBrains Mono 2.304** | `components/aos_fonts/aos_mono_*.c` (`tools/gen_mono_font.py`) | SIL OFL 1.1 | [OFL-1.1-JetBrainsMono.txt](LICENSES/OFL-1.1-JetBrainsMono.txt) |
| **Noto Emoji** (Google, 2D colour set) | the card's `/fonts/emoji.pak`, built by `tools/gen_emoji.py` from the `2D/png/72` images; not in git, it goes out with the release | images under the Apache License 2.0 | [github.com/googlefonts/noto-emoji](https://github.com/googlefonts/noto-emoji); docs/EMOJI.md |
| Region flags (in Noto Emoji's `third_party/region-flags`) | the flags in the same pack | public domain, or exempt from copyright | its `LICENSE` in that folder |
| **Material Design Icons 7.4.47** (Pictogrammers) | `components/aos_ui/aos_sym_*.c` and the folder and settings fonts (bitmaps), `components/aos_web/glifos.js` (SVG paths) | Pictogrammers Free License: icons and fonts Apache-2.0 | brand and logo icons (such as Home Assistant's) are their owners' marks and are not covered by it |

## Data and services

Every 3D model, art pack, sprite, tune and translation in this project is
the author's own. The packs are rendered from the Blender scripts in
`apps/*/tools/blender/` and are not in git. No map tiles, sounds or pictures
from anyone else are tracked.

What the board fetches at runtime:

- **Map tiles:** OpenFreeMap (OpenMapTiles schema, OpenStreetMap data, ODbL).
  The attribution "© OpenFreeMap © OpenMapTiles © OpenStreetMap" is drawn on
  every map, on the board and in the portal. Offline zones are ODbL extracts.
- **Place search:** Photon (komoot), over OpenStreetMap data.
- **Weather:** Open-Meteo, named as the source in the app.
- **Exchange rates:** dolarapi.com.
- **Radio:** the stations' own streams. Search goes to radio-browser.info, and
  covers come from the iTunes Search API. Streams and logos belong to their
  stations.

## Names and marks

Doom is a trademark of id Software.

Claude and the Claude Code character are Anthropic's. The Claude app,
Claudito and Claude Jump are unofficial and not endorsed by Anthropic, and
their pixel art was drawn for this project. The Claude app signs in through
Claude Code's public OAuth client. Its tokens stay on the board and go only
to Anthropic, and using it is subject to Anthropic's terms
([docs/CLAUDE-APP.md](docs/CLAUDE-APP.md)).

Home Assistant and the other products named in the apps belong to their
owners. P4OS is not affiliated with any of them.
