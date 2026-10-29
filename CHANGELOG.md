# Changelog

## Unreleased

- **The Programmer speaks SWD.** A third kind of board: an Arm Cortex-M
  through its debug port, on the pins of a header UART port (TX is SWCLK,
  RX is SWDIO, EN is NRST), bit-banged by a new `aos_io_swd_*`. It finds
  the chip (an RP2040 behind its multidrop DP, with the dormant wake-up
  and TARGETSEL), writes a `.hex`, `.elf`, `.uf2` or `.bin` and reads it
  back to compare: nRF52 through its NVMC, STM32F0/F1 through their flash
  controller, RP2040 through its boot ROM's functions. Read protection is
  said and kept; an nRF52's can only go with an erase of the whole chip
  through its CTRL-AP. And the **RTT console**: what a running firmware
  writes through SEGGER RTT, read from its RAM into the log, with a line
  typed in the portal going down to it. Reading a chip's whole flash by
  SWD is left out on purpose (`docs/plan/PROGRAMADOR.md`). In the
  simulator, against an emulated target (`sim/swd_sim.c`,
  `P4_SIM_SWD=nrf52840|stm32f103|rp2040`). On the board, with a Pico on
  `uart.b`: found, and a blink written and verified in 0.4 s, after two
  fixes the board showed (its DPs, left awake, answering all at once; the
  ROM's table of functions not starting on a word). The clock calibrates
  itself when the port opens, and Detectar measures it: 1005 kHz for the
  1000 asked (the first guess gave 390), about 1.14 MHz at most this way. A test firmware for
  it, built with the Mac's own clang, is in `tools/swd_test/blink_pico`.
- **The Programmer writes Arduinos.** A new kind of board, "Arduino", next
  to "ESP32": an AVR through the bootloader it already has, as the Arduino
  IDE does with avrdude. STK500 v1 (optiboot on an Uno, a Nano or a Pro
  Mini, and the old ATmegaBOOT at 57600), STK500 v2 (the Mega 2560) and
  AVR109 (Caterina: a Leonardo or a Micro on the USB host, sent to its
  bootloader by the 1200-baud touch and found again when it comes back as
  another device). The board is reset by DTR on USB, or by the port's EN
  line wired to RESET on the header, and the protocol is found by trying,
  in the order the USB ID makes likely. The chip comes from its signature.
  It writes the `.hex` the IDE exports, every page read back and compared;
  it refuses the "with_bootloader" one, which only an ISP can write. It
  reads the flash to the card (each page twice) and writes a dump back
  without touching the bootloader; it erases the program and keeps the
  bootloader. The list shows `.hex`, `.elf` and `.uf2` files for the
  families that take them; the portal page has the same choice
  (`"family":"avr"` in its API). The wiring card says that a 5 V board's
  TX needs a divider or a level shifter to reach the P4. In the simulator,
  against `tools/fake_avr_boot.py` (optiboot, ATmegaBOOT, the Mega's and
  Caterina on a pty, speaking only at their own speed): write, verify,
  read, erase and a dump written back, byte for byte, with the four.
- **The Programmer reads and erases.** "Leer la flash" copies a board's
  whole flash (or a range, from the portal) to the card, in
  `firmware/volcados/`, named after the chip, the board's MAC and the date,
  with a `.json` beside it (chip, revision, range, SHA-256); with the stub
  every 16 KB piece is checked by the MD5 the target computes. A dump comes
  back in the list as firmware, so writing it puts a board back as it was,
  or clones it onto another. "Borrar la flash" erases all of it, after
  asking. On the board, an AmoledOS watch (ESP32-S3, 16 MB) on its own USB:
  read whole in 121 s, written back in 98.5 s with its MD5 checked, and
  it came back with all its settings. In the simulator, against
  `tools/fake_esp_rom.py`, which now speaks the stub's read: dump, erase,
  write back and read again gave the same flash, byte for byte.
- **A USB serial port no longer goes deaf after its device restarts.** A
  port at rest whose input transfer had failed (an ESP32-S3 restarting into
  its program at the end of a job did it) never received another byte:
  the CDC-ACM driver does not submit a failed transfer again. Such a port
  is now opened anew, and the Programmer retries once over USB; before, it
  took pulling the cable. "Could not record" was the English for the
  Programmer's "No se pudo grabar": it is "Couldn't flash" now.
- The first steps of a universal programmer (`docs/plan/PROGRAMADOR.md`):
  the service is split into a core and families (ESP32 is the first), and
  it reads `.hex`, `.elf` and `.uf2` into regions for the families to come
  (SWD, Arduino, STM32), with a test on the Mac (`tests/flasher_img`).
- `aos_hal_sha256_begin/update/end`: SHA-256 over data that comes in
  pieces.

## 0.22.2 — 2026-10-26

- **Cartero**, a new app on the card: an API client in the manner of
  Postman, on the board and in the portal. Requests by hand (GET, POST,
  PUT, PATCH, DELETE, HEAD, OPTIONS) with params, headers, a body (JSON
  with a validity mark and Formatear, text, XML, form, GraphQL) and auth
  (Bearer, Basic, API key, inherited); the response with its status, the
  four times as a bar, headers, and the body as a JSON tree whose values go
  into variables with a tap. Tests in JavaScript with Postman's `pm` (and
  the old globals), run by the app's own interpreter on the board and by
  the browser in the portal; collections with folders that run in sequence
  (iterations, a pause, `setNextRequest`, stop at the first failure);
  environments and globals whose `{{variables}}` change the server without
  rewriting a request. The files are Postman's own (Collection v2.1,
  environments), so collections go both ways. A portal page, `#cartero`,
  with the same editor, runner and environments, that sends straight from
  the browser or through the board (no CORS, the board's network).
  `apps/cartero/tools/fake_api.py` is an API to try it against. Tried on
  the board against an HTTPS test server.
- Cartero tells a certificate it cannot verify ("El certificado del
  servidor no se pudo verificar") from a TLS handshake that did not finish
  ("No se pudo establecer la conexión segura"); they were one message, and
  it sent the first test on the board looking at the wrong thing.
- **A big TLS session ticket no longer locks a server out.** A server that
  handed out a 5.5 KB ticket broke the next connection on the board (its
  ClientHello did not fit the 4 KB output buffer, -0x6a00), and every one
  after it until a restart. Tickets that would not fit are not kept (up to
  3 KB are), and a handshake that fails while resuming a stored session
  forgets it and tries once more without it. Any app over HTTPS or wss://
  had it; it was seen with Cartero. `tools/fake_tls_tickets.c` reproduces it.
- Cartero's name in the language packs, and the app in the simulator.

## 0.22.1 — 2026-10-25

- **Impresoras 3D**, a new app on the card: every 3D printer in the house.
  A list with each one's state, heaters, progress, time left and the hour
  it ends; a printer's status (progress ring, the job's picture, pause and
  cancel, heaters, preheat by material, a temperature chart, a Bambu's AMS),
  controls (jog, home, extrude, fan, speed, flow, light, motors off,
  emergency stop), files, camera and G-code console. Notices when a print
  ends, fails or pauses, from the background. Eight protocols: OctoPrint,
  PrusaLink, Bambu Lab (LAN mode), Klipper/Moonraker, Duet RRF, Elegoo
  SDCP, FlashForge and Creality. A portal page, `#impresoras`, with the
  printers live and the list to edit. Tried on the board with three MK3S
  on OctoPrint; the others against `apps/impresoras/tools/fake_printers.py`.
- **TLS against a device's own authority**: `aos_tls_open_trust()` verifies
  the certificate against a PEM and a name the app gives (a Bambu Lab
  printer: its CA and its serial number) instead of the bundle and the
  address, still refusing anything else. For Impresoras 3D.
- Impresoras 3D's name in English and German, and the app in the simulator.

## 0.22.0 — 2026-10-25

- **Widgets chosen on the board.** The home screen's edit mode has
  "+ Widget" (clock, sensors, Home Assistant, Monitor, Claude), and a tap
  on a widget opens its sheet: its size (2×2, 4×2, 4×4), what it shows,
  and a way to take it off. Until now widgets came only from the portal.
  Settings, Edit home screen, says where. A widget long pressed starts the
  edit mode, as an icon does, and in it a widget held a moment drags to
  another place or page. Apps describe their widget with
  `aos_ui_describe_widget()`.
- **A sensor widget** (`widget sensors`): up to six of the sensor service's
  values, CO₂ and particles coloured by their level; on its own it picks
  CO₂, temperature, humidity, pressure and the like, or the ones chosen.
- **The Home Assistant widget shows chosen entities**, up to eight in a
  4×4, instead of the favourites.
- A widget's data in `menu.txt` grows from 63 to 255 bytes.
- **A folder of photos behind the lock screen**: another of them, at
  random, every time the board locks. Photos' palette, "Lock screen: the
  whole folder, rotating"; Settings, Wallpaper, keeps the current one or
  takes it off. Picked on a thread of its own, nothing written at each lock
  (`aos_ui_lock_album_set()`).
- **The home screen's photo stays in memory** with an app in front: going
  home showed the gradient for a second and a half while it was decoded
  again. Cameras lets it go while it shows HD (`aos_ui_wall_photo_spare()`).

## 0.21.6 — 2026-10-23

- **Accent colour of one's own** (Settings, Wallpaper): nine of iOS's
  system colours. `AOS_C_ACCENT` is a call now (`aos_theme_accent()`), so
  apps built against the new header follow it.
- **A wallpaper that follows the time of day**: night, dawn, day, dusk and
  evening, a little different every minute, on the home and lock screens.
- **The lock screen's clock**: large or medium, at the top or in the
  middle, white, the accent, or the lock photo's own colour.
- **System sounds**: locking, unlocking, notifications, calls,
  screenshots, recordings and an optional keyboard click
  (`aos_ui_sound()`). Off with the control centre's bell, which takes the
  place of its second orientation button, or in Settings, Sound.

## 0.21.5 — 2026-10-22

- **Screen recording.** The control centre's Grabar button records the
  screen to an MP4 in the card's `/videos`, games and cameras included,
  with a red dot in the status bar. The PPA turns each frame into YUV and
  the P4's hardware H.264 encoder compresses it, 70 ms a frame between
  them: 15 fps on a quiet screen, 7-9 while apps move. `/api/screenrec`.
  (The control centre's Apagar, which did what Bloquear does, made room.)
- **Photo wallpapers.** A photo behind the home screen and another behind
  the lock screen, chosen from Photos' viewer (the palette at the top
  right), darkened as much as each photo's brightness asks so the names
  read on it. Kept in PSRAM, and let go while an app is in front.
- **Cameras.** The viewer converts each picture straight into the panel's
  framebuffer, with no copy through LVGL; it opens the camera's SD stream
  and a button switches to HD; the turn button turns the picture, not the
  whole system; and the colour conversion is a little quicker. A 704x576
  doorbell runs at 7-9 fps without dropping frames.
- **Lock screen** no longer draws the home screen hidden under it: with a
  photo, 143 -> 108 ms a full redraw.
- **Folders** turn a whole page with each swipe.
- In English and German, Settings says On and Off instead of "Sí"; sensor
  states say Active and Inactive.
- Measured and left off by default: a draw unit that hands fills and photos
  to the PPA or the AXI-GDMA (`draw_hw`), and the PPA converting camera
  frames (`video_hw`): on this board both lose to the CPU, because the work
  is bound by PSRAM. Documented in docs/CAMERAS.md.

## 0.21.0 — 2026-10-22

- **Caños, a pipes game** (`apps/canos`, from the card). Join the tank to
  the drain before the liquid runs: turn the pipes (Turn) or lay the
  missing ones from a tray (Complete), in Easy (the valve by hand), Normal
  and Hard (it opens when the countdown ends). Six plants with their own
  liquid, ten generated levels each: water, fuel, acid, crude oil, lava and
  liquid nitrogen, each running and looking its own way, drawn per pixel,
  with leaks that spread into puddles. Gamepad, its own sound, English and
  German. Played on the board: 23-30 fps while the liquid runs. No firmware
  change: the app and its language files are new in `apps.zip` and
  `lang.zip`.

## 0.20.3 — 2026-10-22

- **An SCD4x that stops measuring is set up again.** A moment without
  power leaves an SCD41 idle, and its last result was shown as new, "ok",
  for as long as it stayed so. With nothing new for four periods it starts
  again; the SCD30 likewise. SCD41 tried on the board.

## 0.20.2 — 2026-10-21

- **A smaller firmware: 7.52 MB down to 6.09 MB,** its slot from 5 % to
  23 % free. The fonts from 24 px up and the 44 and 72 px icons are
  compressed (1 MB less). LVGL decompresses with one shared state and the
  board draws on two cores, so compressed glyphs are taken one at a time:
  without that, big text came out as noise. The clock's digits stay
  uncompressed. Redrawing a whole screen costs 6-9 % more, measured on the
  board.
- **The apps' translations live only on the card.** The firmware carried
  every app's English and German as well as `lang.zip` did (438 KB): now
  it carries the system's only. An app copied without its language files
  shows in Spanish.
- **MPU-9250 tried on the board,** and two fixes for the MPU family: a chip
  that reset on a loose wire came back awake at its factory ranges with
  the compass shut off (3.3 g at rest, a frozen compass), and is set up
  again now; and the first reading after setting it up gave about 4 g.
- Módulos says "1 sensor", not "1 sensors".

## 0.20.1 — 2026-10-20

- **Buttons with garbage after coming back to an app.** Leaving an app that
  stays alive freed its language catalogue, and coming back loaded it
  again elsewhere; LVGL's button matrices keep pointers to their labels,
  so in English or German PWM's mode and knob rows came back blank or
  showing pieces of other strings. Any app with such buttons could show
  it. A catalogue now lives as long as its app.
- **Five servos on a PCA9685 tried on the board** from the PWM app, one by
  one and then in a wave: the video and photos are in the README.

## 0.20.0 — 2026-10-19

- **PCA9685 in the PWM app.** A "16 servos" module declared in
  `modules.txt` (`module servos i2c.ext chip=pca9685 addr=0x40`) adds its
  sixteen outputs to the PWM app's pin sheet and to its portal page: PWM,
  servo and LED channels with the knob and the patterns, 12 bits, one
  frequency a chip (24-1526 Hz, held at 50 Hz while a servo is on it). An
  `aos_io_pca_*` API for any app; the sensor service recognises the chip and
  never takes it for an INA219; I2C ports are shared by every user at once
  (docs/MODULES.md).
- **Datalog keeps logging through a change of language,** and language
  packs up to 1 MB are read (the system's had passed 256 KB and was dropped).
- **AHT10 told from AHT20, "Es otro chip" in Módulos, Bus names every
  address from the catalogue, each I2C chip at its own speed** (the PCF8591
  at 400 kHz read anything), **a candidate lists what else it may be**, and
  the light sensors' range changes no longer give one absurd reading.

- **112 sensors, read by themselves.** The sensor service went from twelve
  chips to 112, from what Adafruit, SparkFun, Seeed, Waveshare, Pimoroni,
  DFRobot, M5Stack and the GY-xxx boards sell: temperature and humidity
  (SHT2x/HTU21D/Si7021, SHTC3, HDC1080/2080/302x, AM2320, MCP9808, TMP117,
  MLX90614...), pressure (BME680/688 with its gas heater, BMP180/388/581,
  DPS310, LPS22, MS5611, QMP6988), air quality (SCD30/40/41, SGP30, SGP40/41
  with Sensirion's VOC and NOx indices, CCS811, ENS160, SEN5x, SPS30,
  PMSA003I), light (VEML7700/6075, TSL2561/2591, TCS34725, APDS-9960,
  LTR390, OPT3001, Si1145, MAX44009), motion and compass (the MPU family
  with the MPU-9250's AK8963, ICM-20948/42688, LSM6DS, BMI160, QMI8658,
  BNO055, ADXL345, LIS3DH and six magnetometers), power (INA260/3221/228,
  MAX17048, ADS1015, PCF8591, MCP342x, NAU7802, MCP9600), distance (VL53L0X/
  L1X, VL6180X, URM09, TF-Luna, RCWL-9620) and more (MAX3010x with the
  pulse, MPR121, Seesaw, AS5600, DS3231, PCF8574). On SPI the MAX31855/
  31856/31865 and MCP3008/3208; on plain GPIOs the DHT11/22, HC-SR04, HX711,
  flow meters, rain gauges, anemometers, Geiger tubes, PIRs and contacts;
  on a serial port the MH-Z19, Senseair S8, PMS5003, SDS011, PZEM-004T and
  the LD2410/LD2450 radars. Autodetect only reads and never probes where a
  write could change a chip (a mux, a DAC, an expander); 29 emulated chips
  of every family read at once in the simulator without one taken for
  another. The catalogue, the module arguments and the wiring:
  [docs/SENSORS.md](docs/SENSORS.md).
- **Up to 12 values a sensor and 32 sensors**, each value with its key and
  its quantity; `aos_sensor_values()` gives them all, and
  `aos_sensor_text.h` says them as Módulos does (names, units, glyphs,
  errors).
- **Módulos' "Agregar módulo"** picks what it measures first, then the chip;
  GPIO sensors get their pins (and a second one), serial ones their port.
  The portal's form groups the chips the same way.
- **Datalog** reads every value of a sensor, not only the first four, with
  the unit the service says, and two new kinds: sensors on pins and serial
  sensors.
- **Pulses and counts in the HAL** (`aos_io.h`): edges timed by an
  interrupt, a pulse out, bits clocked in, and edges counted by the PCNT,
  for the DHT, HC-SR04, HX711 and pulse sensors and for apps.
- **The OTA slots fit in the first 16 MB.** The flash is mapped with 24-bit
  addresses, so an image past 16 MB fails its verification; the old
  `ota_1` reached 128 KB beyond that line. Both slots are now 7.94 MB, the
  most that fits. An update over Wi-Fi still works on the old table while
  the image is small; the new table needs one full flash by serial
  (`p4os-rev1_3-full.zip`, docs/BUILDING.md).

## 0.19.0 — 2026-10-19

- **Datalog** (`apps/datalog`): a data logger for the header's sensors.
  Each reading is a value written to its own CSV in `/datalog` with the
  date and time, every N seconds, minutes or hours or at set times of day:
  an I2C, SPI or 1-Wire sensor of the sensor service (a BME280, the
  MAX6675, a DS18B20...), a GPIO read with its pull-up or pull-down, an
  ADS1115 input (its range, in volts, counts or scaled through two points
  into a unit of one's own) or the board's own Wi-Fi signal and memory.
  Low and high alarms with hysteresis tell with a sound and a message over
  any app, or only with a note in the CSV, and can drive a GPIO (a relay)
  while they last. A chart per reading from an hour to the whole file, a
  portal page (`#datalog`) with the live values, the charts and the files;
  it goes on in the background and starts with the board while it logs.
  Tried on the board with a MAX6675 and a DS18B20
  ([apps/datalog/README.md](apps/datalog/README.md)).

- **MAX6675 thermocouples, in Módulos and in Bus.** The sensor service reads
  SPI chips too, declared in `modules.txt` (`module horno spi.a
  chip=max6675`, with `cs=` for a second one on the same wires): each round
  opens the port with the chip's own CS, reads it and closes it. Módulos
  adds one from its form (chip, SPI port and CS) and shows it as
  "spi.a CS49"; an open thermocouple and a chip that does not answer are
  said in words. Bus has a "MAX6675: temperatura" example, and the
  simulator emulates one (`P4_SIM_SPI_A="48=max6675"`). Tried on the board
  with a type K probe: 156 reads without an error, 26 to 63 °C in 15 s
  with a lighter at the tip (docs/MODULES.md).

- **DS18B20 thermometers in the sensor service.** A GPIO port with a
  module line becomes a 1-Wire bus (`port gpio.ow pin=28`, `module ds18b20
  gpio.ow`): every DS18B20, DS18S20, DS1822 or MAX31850 a search finds there
  is read each second, named after its serial (`ds18b20_A1B2C3`, the same
  at every boot), and `rom=28-...` gives one a name of its own. One
  conversion for the whole bus starts at the beginning of a round and is
  read at its end, while the I2C and SPI chips are read. A GPIO port with
  no such line is never driven. Módulos' form picks a GPIO port or a free
  pin and writes both lines.
- **Apps that start with the board.** An app with
  `AOS_APP_FLAG_BACKGROUND` can ask to start hidden at every boot
  (`aos_ui_boot_start_set()`): a data logger that goes on after a power
  cut or an update. Settings, Developer lists them and takes one off; the
  portal has `GET/POST /api/apps/boot` (docs/APPS-P4.md, "Starting with
  the board").

- **An app working in the background is never closed by the limit of
  apps alive**, and can show a glyph of its own in the status bar
  (`aos_ui_status_icon_set()`). Opening four more apps closed Datalog and
  its logging stopped until it was opened again; now the limit only takes
  apps with nothing running. Datalog shows a red dot while it logs.
  And the sensor service's pins count for no app: with a 1-Wire bus held
  750 ms a second, the app in front was taken for one driving pins and was
  never closed.

## 0.18.0 — 2026-10-17

- **Pinball 3D** (`apps/pinball`): a 3D pinball after Space Cadet, upright,
  with six tables chosen in a carousel: Noche de Zombis, Robots en Órbita,
  El Templo de la Jungla, El Gran Circo, Casino Real and La Isla Pirata.
  Each has its own layout and ramp (a loop to the right, the same to the
  left, a rope bridge, the big top's two ramps, a straight one up to a slot
  machine, a spiral into a whirlpool), toys after the machines of the
  eighties and nineties (a zombie to beat, a robot that swallows the ball,
  a rolling boulder, a human cannonball, a merry-go-round, a roulette, a
  ghost ship, the Kraken), a video mode on the DMD, and its own launch and
  pause buttons, sounds and music. Missions, locks and multiball, skill
  shot, kickback, ball save and tilt; touch or a USB gamepad. Each table is
  a Python module rendered in Blender through the game's camera, with its
  depth, so the ball passes behind ramps and toys; a new table is a module
  and a pack, with no new code. Measured on the board: 68-70 fps (3-6 ms a
  frame); Casino Real dipped to 38-45 in its video mode until the DMD
  redrew only the dots that change; now 63-70.
  PSRAM only; a table takes 10-11.6 MB of pictures
  ([apps/pinball/README.md](apps/pinball/README.md)). Its six art packs
  (`pinball_<table>_p4.pak`, 19 MB) are rendered on the author's machine:
  attached by hand to the first release that has it, carried over after.
- **Home screen: edit mode without leaving it.** A swipe turns the page in
  edit mode too, and an icon held still for a moment (150 ms) is the one
  that drags; before, the pages did not scroll while editing, and taking a
  second app back to the page it came from meant "Listo", a swipe and edit
  mode again. A drop on another page now stays there: the item kept its old
  page and the saved menu put it back where it came from (a folder or an
  app alike).
- **Home screen: the dock is edited too.** Its icons drag like the pages':
  along the dock, out onto a page, and an app of a page goes in; in a full
  dock it changes places with the one it was dropped on.
- **Folders show twelve apps a page upright**, three across and four down,
  instead of nine.
- **No more `flash_mmap` errors at boot with a core dump in flash.** IDF's
  summary of the dump maps its partition, which sits at 28 MB, beyond what
  quad flash maps (16 MB); it was tried first every time and failed. The
  dump is read with flash reads alone, as it already was after that.

## 0.17.0 — 2026-10-17

- **Emulador** (`apps/emulador`): fourteen consoles as an app from the
  card. The NES, Game Boy and Color, Game Boy Advance, Super Nintendo, Mega
  Drive, Master System, Game Gear, ColecoVision, PC Engine, Neo Geo and the
  Atari 2600, 7800 and Lynx, in eleven modules (`/apps/emulador/<core>.so`)
  that load only while one of their games plays; the app finds them in
  515 ms the first time and 31 ms after. A library that opens on the last
  game and the picture it was left on, upright and lying down, each
  console's own pad on the screen, USB gamepads and a second player on the
  keyboard, four save states and an automatic one with pictures, `.sav`
  battery saves, fast forward, and zipped games inflated once to the card.
  A page in the portal takes games, BIOS files and saves, and opens a game
  on the board. Measured there: the Game Boy Advance at 59.7 fps (5-7 ms a
  frame) with gpSP's RISC-V recompiler, once the instructions that rev 1.3
  lacks were left out, and Metal Slug at 59 fps (13-17 ms) with a sprite
  cache in PSRAM that keeps what the frame on screen uses. Each core keeps
  its own licence in its module; Snes9x and the Mega Drive's Z80 are
  non-commercial ([apps/emulador/README.md](apps/emulador/README.md)).
- **Touch: five fingers, and a finger resting beside another.** The GT911
  reports five points and the HAL read two; now `aos_touch_points_n()` and
  `aos_hal_touch_frames_n()` give up to `AOS_TOUCH_MAX` (5), and the
  two-finger functions are as they were. A finger that lands while another
  rests counts after 30 ms instead of waiting for the chip's next report,
  which never came for fingers that do not move: the emulator's d-pad with
  a button held did not work before it. The simulator's scripts take
  `fingers <ms> x1 y1 [x2 y2 ...]` (docs/APPS-P4.md, "Five fingers").
- **Duke Nukem 3D: a new game from the menu no longer hangs.** After
  choosing the skill, the game waited for its voice in an empty loop that
  on DOS the sound card's interrupt ended; here the game's own task mixes
  the sound, so it spun forever and starved core 0. The wait mixes the
  sound now, and the startup's 0.75 s pause is gone
  ([apps/duke3d/README.md](apps/duke3d/README.md)).
- **For emulators, four things in the firmware's table** (asked by
  `apps/emulador`; docs/APPS-P4.md, "Emulators"): `aos_hal_code_sync()`,
  code written at run time made visible to the instruction fetch (a
  recompiler's output in the PSRAM heap); `aos_hal_module_open/sym/close()`,
  .so modules of an app's own from the card's `apps/<app>/` (one core per
  console), which `build_apps.sh` checks, `install_apps.sh` installs and the
  CI packs; `aos_hal_usb_kbd_held()`, the USB keyboards' keys held right
  now; and `aos_hal_display_refresh_request()`, a refresh rate while the
  app is in front, the closest that keeps the panel's blanking wide enough.
- **The panel at 60 Hz.** Its pixel clock is 80 MHz instead of 60, which
  gives room for 60 Hz with ~3.9 ms of blanking for the refresh's restart,
  twice 0.16.0's margin: measured with Duke Nukem 3D at 640x400 filling
  core 0, no frame lost (`POST /api/panel/qos?mhz=60` goes back).
- **USB: the OTG port after the host.** Stopping the USB host now clears
  the session overrides it had set on the High-Speed controller, which the
  OTG modes inherited. And docs/USB.md, "When the computer sees nothing",
  lists what kept a Mac from the board on 2026-10-16: a bad cable, macOS's
  accessory prompt, and something plugged into header pins 25/27, the OTG
  connector's own data lines, which spoiled every High-Speed transfer
  until it was unplugged.

## 0.16.0 — 2026-10-16

- **No more blue flashes when a game uses all of core 0.** Duke Nukem 3D at
  640x400 flashed the screen a few times a minute, more the longer the
  board was on: the task watchdog found core 0's idle task starved every
  5 s, printed its register dump from its interrupt, and the panel's
  refresh, restarted by an interrupt every frame, missed a restart. Core 0
  is the apps' workers' and its idle task is not watched any more; core
  1's is. Measured on the board: 3 flashes in two minutes, each 30-37 ms
  after a watchdog trigger, then none. It was not PSRAM bandwidth. The
  panel's vertical blanking is wider too (120 lines of front porch instead
  of 24: the refresh at 51 Hz instead of 55), for the smaller delays that
  were left: none at 640x400 nor at 640x480 after that.
- **The panel's refresh, measured** (`/api/sysmon`, `system.panel`): frames
  that ran dry, restarts that came late and when, and the watchdog's
  triggers; `POST /api/panel/qos` tries the interconnect's priorities and
  the vertical blanking (docs/MEMORY.md). The panel comes up from core 1,
  and its interrupts run with the cache suspended (1.3 KB of internal RAM).
- **Duke Nukem 3D** (`apps/duke3d`): Chocolate Duke3D, the Build engine and
  3D Realms' game, as an app from the card, no firmware changed. 30 fps at
  320x200 and 480x300 (the game's own rate; 59 and 34 uncapped), with the
  effects and the Sound Blaster's FM music synthesised on the board (OPL3 on
  DBOPL, through Apogee's own AdLib driver), the touch pad of Doom with Duke's
  buttons (jump, crouch, items), a USB gamepad, saves, and the shareware or
  the full game's `DUKE3D.GRP` in the card's `duke3d/` (none is included).
  640x400 and 640x480 too, at 15-21 and 15-18 fps, clean with the panel's
  fixes above (they use all of core 0, which flashed the screen before them).
  GPL v2, and the Build licence for its engine; the details and the traps
  (int32_t as long, 1996 C under gcc 14, a pixel budget that ran out) in its
  README.

## 0.15.1 — 2026-10-15

- **The web portal speaks the board's language.** The portal, the pages the
  apps bring (23) and the API's messages followed nothing but Spanish: now
  English and German too, about 1,900 texts, following Settings, Language
  (the login screen as well). The portal asks the board its language
  (`/api/auth`) and the catalogs come from the card at `/lang/<code>/`:
  `_portal.lang` and one `web.<page>.lang` per page, in the packs' format,
  read by the browser and never by the board. A page marks its texts
  `P.tr('...')` (docs/PORTAL-PAGES.md, "Languages"), and
  `tools/gen_lang.py` finds them as it finds `_()`. Numbers and dates take
  the language's form (a decimal point in English). Nothing in the portal
  tells a missing file by an error's text any more, only by its 404.
- **The Programmer speaks the interface's language.** Its messages are
  formatted in the flashing service, which the translation tool did not
  read: with the board in English they came out in Spanish. So did the C6
  update's errors and the pendrive's, which the HAL writes; it now keeps the
  reason as a key that Settings translates. English and German added.
- **The Programmer talks to WCH's newer USB-serial chips.** A CH343 or
  CH9102 (vendor `1a86`, pid `55d2` and up: the ESP32-P4-Nano's, for one)
  is a standard CDC-ACM device under the CH340's vendor id; it went to the
  CH34x driver, whose own requests reset the target but left the data
  stuck (`TX transfer timeout`, "No response"). The USB host now takes a
  CDC-ACM interface first, whoever made the chip, and the vendor drivers
  only when there is none.
- **The Programmer reads an ESP32-P4 image and checks its revision.** A
  merged P4 image has its bootloader at 0x2000, and the chip went
  unrecognised. The chip revisions an image accepts (from its header) are
  read too, and an image for other revisions is refused before anything is
  written: an ESP32-P4 v3.1 with a build for v0.0-v1.99 would have been
  flashed and never started. The wiring help says GPIO35 for the P4's BOOT.
- **The Programmer flashes an ESP32-P4 v3.1 or v3.2.** Those revisions come
  out of reset with their flash unpowered, and esptool powers it right after
  connecting; esp-serial-flasher does not yet (its issue 184), so the first
  erase hung. The Programmer runs esptool 5.3.1's sequence before the stub
  goes up. Tried with an ESP32-P4-Nano (v3.1, a CH343 on USB): ESPHome's
  2.7 MB image written and MD5-checked in 254 s at 115200 baud, and the
  board started it.
- **Two more of esp-serial-flasher's guesses, corrected.** Flash encryption
  is read from FLASH_CRYPT_CNT, as esptool does (the library counted the key
  purposes' bits, and the Nano came out "encrypted"), and an erase gets 30 s
  per MB, esptool's margin, instead of 10.

## 0.15.0 — 2026-10-13

- **Python** (`python.so`, from the card): MicroPython 1.29 with double
  floats, big integers and complex numbers. **A console** on the board with
  a keyboard made for Python (a bar of the words typed most, history,
  auto-indent), or a USB or Bluetooth keyboard; `input()` and `help()` in
  the interface's language. **The card** is its filesystem: `open()`, `os`
  and `import` as on any MicroPython board, `.mpy` too. **The network:**
  `socket` and `tls` over the firmware's connection (https:// with the
  system's certificates), and micropython-lib's `requests` and `mip`
  inside: `mip.install()` fetches libraries into `/python/lib` with their
  dependencies. **Apps:** every `.py` in `/python/apps` is an app of its own
  in the launcher, drawing with the `aos` module (Lua's names, plus a loop
  of its own around `aos.frame()`, MicroPython's `framebuf` over the canvas,
  RGB565 images); seven samples, written there the first time. The heap
  grows from 2 MB to 12 MB as a program asks and goes back when it ends.
  On the board: `fib(24)` in 481 ms, a megabyte downloaded and written to
  the card in 2 s, a sample app at 50 fps.
  [apps/python/README.md](apps/python/README.md).
- **Python's page in the portal** (`#python`): the board's console from
  the browser, the card's programs, apps and libraries with an editor; a
  program runs in the console, an app opens on the board with its output
  and its errors under the editor; `mip` by name.
- `tools/fake_mip.py`: a fake micropython-lib index and web server (HTTP or
  HTTPS) to test the network without outside servers.
- **Two apps of one module no longer pull the code from under each other.**
  The loader's `dlopen()` hands back the module already loaded, with no
  count, and `dlclose()` unloaded it outright: the apps of `lua.so` and
  `python.so` share one copy, and closing one took the code from the next
  (going from one Python app to another without passing through the home
  screen: an illegal instruction, and a restart). A module is unloaded now
  only when none of its apps is alive, and the same goes for the scan and
  for an app that fails to start. Tried on the board: five Python apps and
  three Lua scripts one after the other, with no restart.
- **`fsync` and `ftruncate` for the apps.** Python's `f.flush()` reaches the
  card now (the size shows before the file is closed).
## 0.14.0 — 2026-10-12

- **CAS, a computer algebra system** (`cas.so`, from the card): Giac, the
  engine of Xcas, built as an app (exact arithmetic, algebra, calculus,
  equations, matrices) behind six views. **Calculate:** results drawn as a
  book prints them, a two-dimensional editor and a maths keypad (or a USB
  or Bluetooth keyboard). **Graph:** six functions, pan, pinch, trace.
  **Geometry:** constructions with points that drag and everything built on
  them following, and exact measurements (4√2, 25/4, 16π). **Statistics:**
  four lists, charts, six regressions, seven distributions, tests.
  **Finance:** TVM, loans with their table, rates (TNA, TEA, TEM), NPV and
  IRR. **Sciences:** the periodic table, molar mass, equations balanced
  exactly, gases, solutions and pH; 41 physics formulas that solve for any
  variable, a projectile, the CODATA constants; Punnett, Hardy-Weinberg,
  DNA, growth. Portrait and landscape, Spanish, English and German. On the
  board: the engine opens in ~0.5 s and the bench's 23 lines take 1-92 ms
  each. [apps/cas/README.md](apps/cas/README.md).
- **CAS's page in the portal** (`#cas`): the calculator from the
  computer's keyboard, the grapher's functions, the statistics lists in and
  out as CSV, and the plot on the board's screen saved as a PNG.
- `cas.so` is GPL-3.0-or-later (Giac); the firmware stays MIT
  ([THIRD-PARTY.md](THIRD-PARTY.md)). The CI builds its engine
  (`apps/cas/engine/build_engine.sh`) and keeps it in its cache.
- **The card's apps keep going when the screen turns.** The loader never
  passed an app's `resize()` on: every `.so` app was destroyed and created
  again when the screen turned, though twenty of them (ARKANOS, Claude
  Jump, Doom, Maps, Mila, CAS...) know how to lay themselves out again.
  Created again within the same tick, the module was not reloaded either,
  and CAS, whose engine starts once per load, came back without it and
  closed. The UI also stood still ~5 s while CAS stopped its engine, which
  is why `/api/screen.bmp` failed right after turning. The simulator links
  the apps without the loader, so it never showed.
- **Booting does not open every app any more.** What each app says about
  itself (id, name, colours, order, flags, its AIC icon) is kept in
  `/data/apps.cache`, checked by the file's name, size and date and by the
  firmware's ELF hash; only a new or changed `.so` is opened to be
  described. On the board with 44 modules: the scan went from 2.6 s to
  0.5 s, and `cas.so` (5 MB) no longer costs ~0.6 s at every boot.
- **Each release carries its firmware's ELF** (`p4os-<rev>.elf.xz`), so a
  crash dump from a released firmware can be read: `tools/coredump.sh`
  takes the version from the dump, downloads that release's ELF and keeps
  it only if its hash matches the dump's.

## 0.13.5 — 2026-10-11

- **BLE activates Xiaomi sensors from the board.** A Xiaomi with its auth
  service (0xFE95 with 0x0010 and 0x0019; the LYWSD03MMC and its kin) gets
  "Activar (Xiaomi)" in the GATT explorer, on the board and on the portal's
  page. After a warning (the sensor forgets its previous key), the app runs
  the register pvvx's TelinkMiFlasher does: the sensor's id, a P-256 key
  exchange, HKDF-SHA256 of the secret into the new bind key, and the id
  sent back encrypted. When the sensor accepts, the key is kept in
  `ble/claves.txt`, the sensor reads at once, and the key is shown to copy
  into Home Assistant. Tried with a stock LYWSD03MMC: accepted in ~5 s.
- **Crypto for the apps** (`aos_hal.h`, "Crypto"): random bytes from the
  chip, SHA-256, HMAC, HKDF and P-256 key pairs and ECDH, on the mbedTLS
  the firmware already had for HTTPS (`components/aos_hal/aos_crypto.c`,
  shared with the simulator). The BLE app needs this firmware or later.
- The simulator's stock Xiaomi takes a connection and an activation, and
  then encrypts its readings with the new key; `apps/ble/test` runs the
  whole exchange on the Mac.
- **The released firmware says its version.** Every firmware built by the
  CI said "1" in the portal: in the IDF's container git refused the
  checkout and had no tags, so `git describe` failed. Fixed from this
  release on.

## 0.13.4 — 2026-10-10

- **BLE's GATT client connects at 15-30 ms.** At NimBLE's default
  30-50 ms a stock Xiaomi thermometer let the service discovery hang until
  the ATT timeout: on the board 4 tries out of 6 failed after 35-43 s and
  the other 2 took 16-17 s. The connection now asks for 12-24 × 1.25 ms,
  latency 0 and a 2 s supervision timeout: 10 out of 10 went through in
  7-15 s. A peripheral that wants slower asks afterwards and is given it as
  asked; the request and the update are logged.
- **The GATT explorer in the portal's page too.** Connect, read all, and
  per characteristic read, write and listen, from the computer, on the
  board's connection; it goes on with the board's screen elsewhere or
  locked.
- **Hi-Link presence radars read in words** (LD2410 and kin). Their frames
  are told by their content wherever they show: a report reads as who is
  there and at what distance (engineering mode with its gates, light and
  OUT pin), a command and its answer by name and outcome. On a module
  named `HLK-`, the UART bridge's 0xFFF2 gets a button that sends the
  documented permission command with the factory password, without which
  it reports nothing over Bluetooth. Tried with an LD2410: "someone still
  at 4.67 m", ~11 frames a second.
- Asking again for the device already connected no longer cuts the
  connection (NimBLE answered `BLE_HS_EDONE` while the old link was torn
  down), and that code reads as what it is.

## 0.13.3 — 2026-10-10

- **BLE keeps up to 2048 devices, not 256.** At a fair the screen said
  256: the table had a fixed top, and phones change their private address
  every few minutes, each a new device. It now starts at 256 and doubles up
  to 2048 (~0.8 KB each in PSRAM; the sensors' two hours kept apart, for 64
  of them), a device is found by a hash of its address, and "seen since
  launch" counts them all, forgotten ones too. Airwaves and the portal say
  when the top is reached and how many were forgotten, and the page says
  how many fit in its live answer. In the simulator, `P4_SIM_BLE_CROWD=N`
  adds that many phones changing address every minute.
- **A Wi-Fi scan no longer cuts a first connection.** 0.13.2's scan
  borrowed the radio from any station not connected yet, also one that was
  connecting to a network in reach (right after boot, say): the attempt was
  let go. It now borrows it from a station that has failed at least once
  (away from home), or after 2.4 s of a first attempt; and two scans at
  once (the portal's and the board's) give it back only when both are done.

## 0.13.2 — 2026-10-09

- **Scanning for Wi-Fi works away from home.** With a saved network out of
  reach the station retries for ever, and the IDF refuses to scan while it
  is connecting: the Network app and the portal's Wi-Fi page found nothing
  (seen at a convention). The scan now borrows the radio, as AmoledOS did:
  the retries stop, the attempt is let go, the scan runs, and the station
  looks for its network again. Connected, it scans as before.
- **Away from home the board no longer restarts every minute.** The
  network check restarts a board with no network after a minute (for a C6
  that came up without it), at most twice, but its count was left in the
  cache and lost at each restart: it said "try 1 of 2" for ever. The count
  is written to PSRAM now (and the hang and C6-link checks' too, which had
  the same flaw), and no restart happens when the station says why it has
  no network: the network is not around, or the password is wrong. Tried on
  the board with a saved network that does not exist: scanned, and stayed
  up.
- Drawing tried on the board by finger and with a USB mouse: it works.
- `tools/console_log.py` records the serial console to a file, for what
  the portal cannot show (no network at all, a boot loop). Opening the port
  restarts the board.

## 0.13.1 — 2026-10-08

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
- **Apps in the background** (Settings, Developer): an app kept on leaving
  is closed instead if it holds 1 MB of PSRAM or more and has nothing
  running (header pins, sound, the microphone, a recording, the worker, or
  the app saying so with `aos_ui_set_busy()`); below 8 MB of free PSRAM the
  hidden ones go too. "Close on leaving" and "Keep open" are the other
  modes. The page lists what is open and what each one holds; the log says
  each decision and whether a closed app gave its PSRAM back. Measured on
  the board: Photos, Pixel Art and Drawing (2.1, 2.0 and 1.7 MB) now close;
  the games already did, and give all their memory back.
- VNC stays open while it has a session, so going home does not drop it.
- Settings' restart button says "Restart the board" in English (it said
  "Reset", the stopwatch's word).

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
