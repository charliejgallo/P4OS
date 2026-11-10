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
| **Monocypher 4.0.2** (Loup Vaillant, Michael Savage, Fabio Scotoni) | **vendored**, `components/monocypher/` | BSD-2-Clause or CC0-1.0, either | unchanged: `monocypher.c` and `optional/monocypher-ed25519.c` with their headers; X25519 and Ed25519 for SSH (`docs/SSH.md`); [BSD-2-Clause-or-CC0-Monocypher.txt](LICENSES/BSD-2-Clause-or-CC0-Monocypher.txt) |
| **minimp3** (lieff) | **vendored**, `components/aos_hal/minimp3/` | CC0-1.0 | one change, marked `AmoledOS:` (the caller's scratch buffer) |
| **ESP-IDF's USB host library** (`components/usb` of release/v5.5, `c94f345e`) | **vendored**, `components/usb/` | Apache-2.0 | **modified**: two root ports at once, retries of failed enumerations, a hub fix; the changes are in [components/usb/P4OS.md](components/usb/P4OS.md) and marked `P4OS:` in the source; [Apache-2.0.txt](LICENSES/Apache-2.0.txt) |

The Espressif MIT and Modified MIT licences cover Espressif hardware only.
P4OS's MIT licence does not extend to those two components. A port to other
hardware has to replace them.

## In the apps on the card

| What | Where | Licence | Notes |
|---|---|---|---|
| **doomgeneric** (`dcb7a8d`), Chocolate Doom 2.2.1's music player, DOSBox's DBOPL | **vendored**, `apps/doom/main/doomgeneric/` | **GPL-2.0-or-later** | modified, every change listed in [apps/doom/README.md](apps/doom/README.md). `doom.so` as a whole is GPL-2.0; the repository at the release's tag is its source. **No WAD is included.** |
| **Chocolate Duke3D** (`1cfa190`): 3D Realms' Duke Nukem 3D source release and the Apogee Sound System | **vendored**, `apps/duke3d/main/game/` | **GPL-2.0-or-later** | modified, every change listed in [apps/duke3d/README.md](apps/duke3d/README.md). **No GRP is included.** |
| **Build engine** (Ken Silverman), as Chocolate Duke3D carries it | **vendored**, `apps/duke3d/main/engine/` | **Build licence** (`BUILDLIC.TXT`, next to it) | modified, as above. Derived works only over the Internet, free of charge, never commercially, with that file and Ken Silverman's notice. |
| **nofrendo** (Matthew Conte, Neil Stevens), as **Retro-Go** (`4ced120`) carries it | **vendored**, `apps/emulador/cores/nes/nofrendo/` | **GPL-2.0** (LGPL-2 headers) | the NES core of the Emulador; changes listed in [apps/emulador/cores/nes/README.md](apps/emulador/cores/nes/README.md). |
| **gpSP** with the RV32 recompiler of **gpsp-esp32** (Midworld Kim, `296be4a`), its libretro glue and open replacement BIOS | **vendored**, `apps/emulador/cores/gba/gpsp/` | **GPL-2.0-or-later** (gpSP), **GPL-3.0-or-later** (the RV32 recompiler) | the Game Boy Advance core of the Emulador; changes listed in [apps/emulador/cores/gba/README.md](apps/emulador/cores/gba/README.md). since the cores became modules (`gba.so`), each module carries its own core's licence and the app (`emulador.so`, MIT code) none; the repository at the release's tag is their source. **No game or official BIOS is included.** |
| **gnuboy**, as Retro-Go (`4ced120`) carries it, with VBA-M's colorization tables | **vendored**, `apps/emulador/cores/gb/` | **GPL-2.0** | the Game Boy / Game Boy Color module (`gb.so`); changes in its README. |
| **Snes9x** (the snes9x2005 / CATSFC line, as Retro-Go `4ced120` carries it) | **vendored**, `apps/emulador/cores/snes/snes9x/` | **Snes9x licence: non-commercial**, not GPL-compatible | the Super Nintendo module `snes.so`, a file of its own that links nothing GPL; changes in its README. |
| **SMS Plus** (Charles MacDonald), as Retro-Go carries it, with a new Z80 and SN76489 written for P4OS | **vendored**, `apps/emulador/cores/sms/` | **GPL-2.0-or-later** | the Master System / Game Gear / ColecoVision module `sms.so`; MAME's non-commercial Z80 and the unlicensed PSG were replaced. No ColecoVision BIOS is included. |
| **pce-go** (HuExpress lineage), as Retro-Go carries it | **vendored**, `apps/emulador/cores/pce/` | **GPL-2.0** | the PC Engine module `pce.so`; changes in its README. |
| **Gwenesis** (bzhxx, `168e466`, as RetroESP32-P4 `339f17f` carries it), its Musashi 68000 and Marat Fayzullin's Z80 | **vendored**, `apps/emulador/cores/md/gwenesis/` | **AGPL-3.0** (Gwenesis); the Z80's notice forbids commercial distribution | the Mega Drive module `md.so`, a file of its own; changes in its README. |
| **gngeo** with the Generator 68000 (GPL-2), as RetroESP32-P4 carries it, superzazu's z80 (MIT, `d64fe10`), MAME's YM2610, NEO-CMC and PD4990A code (no licence in the files) and miniz's tinfl (MIT) | **vendored**, `apps/emulador/cores/neogeo/` | **GPL-2.0-or-later** (gngeo) and as listed | the Neo Geo module `neogeo.so`, a file of its own; MAME's non-commercial Z80 replaced; changes in its README. **No SNK BIOS is included.** |
| **Stella** (as RetroESP32-P4 carries it), **ProSystem** (as RetroESP32-P4 carries it), **Handy** (as Retro-Go carries it) | **vendored**, `apps/emulador/cores/a2600/`, `a7800/`, `lynx/` | **GPL-2.0-or-later** (Stella, ProSystem), Handy's own permissive licence (`handy/license.txt`) | the Atari 2600, 7800 and Lynx modules; changes in each README. No BIOS is included. |
| **librtlsdr 2.0.3** | **vendored**, `apps/rf/main/rtlsdr/` | **GPL-2.0-or-later** | two small changes, listed in [apps/rf/README.md](apps/rf/README.md). `rf.so` as a whole is GPL-2.0-or-later; the repository at the release's tag is its source. |
| **Giac 1.9** (as KhiCAS ships it for the NumWorks), **GMP 6.2.1**, **MPFR 4.1.0**, **MPFI 1.5.4** | **downloaded and built** by `apps/cas/engine/build_engine.sh` (checked by SHA-256), not in git | **GPL-3.0-or-later** (Giac), **LGPL-3.0-or-later** (GMP, MPFR, MPFI) | KhiCAS's interface (`kdisplay.cc`) is left out; its turtle commands are cut out of it unchanged. `cas.so` as a whole is GPL-3.0-or-later; the repository at the release's tag and the archives the script names are its source. Details in [apps/cas/README.md](apps/cas/README.md). |
| **libstdc++, libsupc++ and libgcc's soft-fp** of **GCC 14.2.0** | **downloaded and built**, the same way | GPL-3.0 with the GCC Runtime Library Exception | rebuilt as position-independent code; `tzdb.cc` is replaced by an empty file |
| **openlibm 0.8.8** | **downloaded and built**, the same way | MIT, BSD-2-Clause and others (FreeBSD's msun) | one line patched in `riscv_fpmath.h` (a 128-bit long double on RV32) |
| **TLSF 3.1** (Matthew Conte) | **vendored**, `apps/cas/main/tlsf.c` | BSD-3-Clause | as KhiCAS carries it |
| **Lua 5.4.8** | **vendored**, `apps/lua/main/lua/` | MIT, Lua.org, PUC-Rio | one patch to `luaconf.h`, recorded in `VENDOR.md`; the notice stays in `lua.h` |
| **MicroPython 1.29.0** (Damien George and contributors) | **generated and vendored**, `apps/python/main/upy/`, by `apps/python/tools/gen_upy.sh` from the release tarball (checked by SHA-256) | MIT | its embed port; `main/upy/VENDOR.md` lists the changes (sources a module includes renamed `.inc`, `extmod/vfs_posix_file.c` built without VfsPosix, `<sys/poll.h>`); its `LICENSE` summarises the rest |
| musl's double-precision maths (`lib/libm_dbl`), **re1.5** (Russ Cox, Paul Sokolovsky), **uzlib** (Jørgen Ibsen, Paul Sokolovsky), **crypto-algorithms** SHA-256 (Brad Conte) | **vendored** with MicroPython, `apps/python/main/upy/lib/` | MIT; BSD-3-Clause; Zlib; public domain | as MicroPython carries them |
| **requests 1.1.1** and **mip 0.5.0** of **micropython-lib** (`90028be`) | **vendored** and frozen into `python.so`, `apps/python/frozen/` | MIT | one change in `requests` (reading a body whole), marked `P4OS:`; `apps/python/frozen/VENDOR.md` |
| compact AES (forward cipher), after **tiny-AES-c** (kokke) | **vendored**, `apps/rf/main/aes.c` | public domain (the Unlicense) | the standard AES S-box and schedule, generalised to a runtime key size, for `rf_mesh.c`'s AES-CTR (reading a Meshtastic channel the user has the key to) |

The firmware that loads `doom.so`, `duke3d.so`, `emulador.so`, `rf.so` and `cas.so` stays MIT.
The GPL applies to the Doom, Duke Nukem 3D, Emulador, RF and CAS apps and to any simulator build
that compiles them in; `duke3d.so` is also under the Build licence, for its engine.

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

- **The periodic table** in CAS (`apps/cas/main/cas_elements.c`): IUPAC's
  standard atomic weights; configurations, electronegativities, states,
  temperatures and densities from PubChem's periodic table (NIH, public
  domain). `apps/cas/tools/gen_elements.py` writes it.

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

## The case

| What | How it gets here | Licence | Notes |
|---|---|---|---|
| `case/v1/` (the enclosure and its buttons) | vendored, remixed | CC0, public domain | from [Waveshare ESP32-P4 5inch Touchscreen Enclosure](https://www.printables.com/model/1787054-waveshare-esp32-p4-5inch-touchscreen-enclosure) on Printables; see [case/README.md](case/README.md) |

## Names and marks

Doom is a trademark of id Software. Duke Nukem is a trademark of Gearbox
Software; the Duke Nukem 3D app is unofficial.

Claude and the Claude Code character are Anthropic's. The Claude app,
Claudito and Claude Jump are unofficial and not endorsed by Anthropic, and
their pixel art was drawn for this project. The Claude app signs in through
Claude Code's public OAuth client. Its tokens stay on the board and go only
to Anthropic, and using it is subject to Anthropic's terms
([docs/CLAUDE-APP.md](docs/CLAUDE-APP.md)).

Python® is a registered trademark of the Python Software Foundation; the
Python app uses the name to say what it runs, and is not affiliated with or
endorsed by the PSF. MicroPython is Damien George's project, MIT-licensed.

Home Assistant and the other products named in the apps belong to their
owners. P4OS is not affiliated with any of them.
