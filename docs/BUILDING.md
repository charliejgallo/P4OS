# Building P4OS

Three things build from this repository, independently:

| What | Where | Command |
|---|---|---|
| Firmware | repo root | `tools/build_fw.sh rev1_3` (the board here is rev 1.3), or `rev3_x` |
| Apps (`.so`) | `apps/` | `tools/build_apps.sh [name…]` (see `docs/APPS-P4.md`) |
| Simulator | `sim/` | `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8` |
| Hardware bench | `bench/p4bench/` | `./build.sh rev3_x lvB` (see `bench/README.md`) |

## Requirements

- ESP-IDF **v5.5.x** in `~/esp/esp-idf` (IDF 6 is not supported yet: the
  ESP32-C6 link is only validated on 5.5 with esp_hosted 1.4).
- Simulator: `brew install sdl2 cmake mbedtls ffmpeg`, and LVGL **9.5.0** in
  `sim/lvgl` - either a link to AmoledOS's checkout or
  `git clone --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl sim/lvgl`.

## Chip revision

The ESP32-P4 comes in rev1.x and rev3.x silicon, and a binary for one does
not boot on the other. `esptool.py --chip esp32p4 chip_id` says which one you
have. Each profile builds in its own directory (`build/rev3_x`,
`build/rev1_3`) so their `sdkconfig`s never mix.

## Flashing

    tools/build_fw.sh rev1_3 -p /dev/cu.wchusbserial<id> -b 460800 app-flash

`app-flash` writes the app partition only; the board restarts by itself. The
card's apps and the preferences stay. Opening the CH340 port in any other way
(a serial monitor) also resets the board.

`app-flash` always writes `ota_0`, so `build_fw.sh` erases the otadata after
it: otherwise a board that last updated over Wi-Fi goes on booting `ota_1`,
the old image. A full `flash` (bootloader and partition table too) is only
needed when those change; the rollback below needed one, once.

## Updating over Wi-Fi

    tools/ota.sh p4os.local           build rev1_3, upload, restart, check
    NOBUILD=1 tools/ota.sh p4os.local   upload what is already built

Or from the portal: **Firmware**, "Elegir el .bin…" with
`build/rev1_3/p4os.bin`, then "Reiniciar". The upload takes ~25 s for 5.7 MB.

- The flash has two 8 MB app slots (`ota_0`, `ota_1`). The image is written
  into the idle one while it arrives (`PUT /api/ota`), so a cut upload leaves
  the running firmware untouched.
- The new image boots **on trial** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
  `main.c` confirms it once the system has been up for 30 s. A crash, a
  watchdog or a restart before that, and the bootloader brings the previous
  image back by itself and marks the new one "aborted". Tested with an image
  that panicked on purpose.
- `ota.sh` waits for the confirmation and says so when the board came back
  on the old slot. The portal's page shows both slots, and "Volver a la otra
  ranura" boots the idle one at the next restart.
- `ota.sh` (and `build_fw.sh` after a `flash` or `app-flash`) keeps the ELF in
  `build/elf/<sha>.elf`, the last ten. `<sha>` is the first 16 hex of the
  ELF's sha256, the same one the portal shows as "ELF" for each slot: the key
  for decoding a core dump from any image still around.
- The "Compilado" date is the app descriptor's, which only changes when that
  file recompiles. The ELF sha is what tells two builds apart.

## When the board crashes

Two things survive a restart, and both are readable with no serial cable
(the USB port may be busy being a keyboard):

- **The previous boot's log.** The log ring (256 KB) lives in PSRAM the
  restart does not clear (`EXT_RAM_NOINIT_ATTR`, `aos_logring.c`). At boot
  its last 64 KB are kept: portal **Registro** → "Ver el arranque anterior",
  or `GET /api/log?prev=1`. It is lost by a power cut, and by a boot of a
  *different* build (the ring's address moves with each link).
- **The core dump.** A panic writes it to the `coredump` partition (1 MB).
  Portal **Firmware** → "Último cuelgue" downloads or erases it. On the Mac:

      tools/coredump.sh p4os.local           # ERASE=1 to erase it afterwards

  It tries every ELF in `build/elf/` until one matches. The full decode needs
  `riscv32-esp-elf-gdb` (`python ~/esp/esp-idf/tools/idf_tools.py install
  riscv32-esp-elf-gdb`). Without it, `tools/coredump_min.py` gives each
  task's PC and RA as functions and lines, with `addr2line`.

And two things that restart the board on purpose:

- **The network watchdog:** if the Wi-Fi is on, a network is saved, and it
  has not connected 60 s after boot, `main.c` logs it and restarts, at most
  twice in a row (a counter in noinit PSRAM). Three times in a day the
  network did not come back after a software restart; this is the
  workaround until the cause is found.
- **The hang watchdog** (`aos_hal_p4.c`): once, after an OTA's software
  restart, the board came up with neither network nor USB and stayed so
  until RESET, which loses the previous boot's log. Now the boot marks its
  stages (`boot stage: ...` in the log), the main loop beats every 200 ms,
  and an esp_timer of its own restarts the board by software when the boot
  has not finished 60 s in or the beat stops for 30 s, after logging the
  stage and every task's state. Twice in a row at most. The next boot's
  `GET /api/log?prev=1` says where it hung.
  On 2026-09-30 it hung again after an OTA, and the watchdog did not fire:
  no network or USB for over 5 minutes, until RESET. Either the whole chip
  stopped, esp_timer included, or the boot did finish and the main loop
  kept beating with only the network and USB dead, which this watchdog
  does not cover. Next time: does the screen still answer the touch before
  RESET? In the run before it, the BOOT button gave no edge at all, also
  after an OTA's software restart; after the RESET it worked at once. That
  was seen once.
  On 2026-10-03 it happened on the replacement board as well, right after an
  OTA: so it is not that board. The next OTA ran with the serial port
  recording (open it first, then OTA, and keep reading): that time the
  restart was clean, and the log is the baseline to compare with. It shows
  the P4 resetting the C6 through GPIO54 on every boot, so a C6 left in a
  bad state by the previous run is ruled out.

When a HAL function is added or removed, the apps' symbol table must follow:

    tools/build_fw.sh rev1_3
    python3 tools/gen_symbols.py --build build/rev1_3
    tools/build_fw.sh rev1_3

## Simulator

    cd sim && ./build/p4os_sim

A 720x1280 window scaled by `P4_SIM_ZOOM` (default 0.6). The fake SD card is
`sim/sim_fs`; `sim_fs/menu.txt` is the demo home screen.

| Key | Does |
|---|---|
| r | portrait / landscape |
| h, b | home, back |
| x | a notification arrives |
| e | edit the home screen |
| s | screenshot to `sim_shot.png` |

Environment:

- `P4_SIM_UART_A=/dev/cu.usbserial-110` (and `_B`): the header port
  `uart.a` / `uart.b` is a real USB-serial adapter on the Mac. Without it the
  port is a simulated ESP32 printing a boot log.
- `P4_SIM_I2C_EXT=0x76,0x44`: the addresses that answer on `i2c.ext`.
- `P4_SIM_SPI_A="49=w25q128,48=max31855,47=mcp3008"`: which simulated chip
  answers on `spi.a` for each CS (that is the default); a bare chip name for
  every CS, `loop` for a jumper from MOSI to MISO, `none` for nothing
  (`docs/MODULES.md`, "SPI").
- `P4_SIM_BATTERY=1`: pretend a battery is connected.
- `P4_SIM_SCRIPT="wait 800; tap 360 700; shot a.png; quit"`: scripted input
  and screenshots (`wait`, `tap x y [ms]`, `drag x1 y1 x2 y2 [ms] [rest]`,
  `key k`, `open <app>`, `rotate`, `shot <file.png>`, `quit`). Delete
  `sim_fs/prefs.txt` first: it remembers the orientation. Or point
  `P4_SIM_PREFS` at a file of your own: agents running sims side by side
  each use their own, and a build directory of their own (`sim/build-<x>`).

`tools/montage.py out.png a.png b.png ...` lays screenshots side by side.

## HAL stubs

`components/aos_hal/aos_hal_stubs.c` is generated: `tools/gen_hal_stubs.py`
writes a weak "not available" version of every function in `aos_hal.h`, so
the board's HAL can be written a block at a time. Run it again after
changing the header.
