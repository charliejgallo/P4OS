# Memory on the P4: internal RAM and PSRAM

The board has 32 MB of PSRAM at 200 MHz and about 440 KB of internal RAM.
Internal RAM is what runs out. It is also the only RAM that some things
accept. So the rule is **everything goes to PSRAM, except what cannot.** This
file lists what cannot, and why, so that new code follows it.

## What must stay in internal RAM

- **The stack of any task that touches the internal flash.** Every flash
  operation, NVS included (reads too), suspends the cache that PSRAM sits
  behind. The flash driver asserts when the calling task's stack is in PSRAM
  (`spi_flash/cache_utils.c`, `esp_task_stack_is_sane_cache_disabled`).
  `CONFIG_SPIRAM_XIP_FROM_PSRAM` does not change this: the code keeps running
  from PSRAM, but the cache is still suspended around the operation.
  - The preferences are safe from any task. `aos_hal_pref_*` hand the NVS
    call to a helper task with an internal stack when the caller's stack is
    in PSRAM (`aos_flash_call`, `components/aos_hal/aos_flashop_p4.c`).
  - Anything else that reaches the flash has to go through `aos_flash_call`
    too: `esp_partition_*`, `esp_ota_*`, `esp_image_*`, `esp_flash_*`,
    `esp_core_dump_*`. Reading a slot's state (`esp_ota_get_state_partition`)
    or its description reads the flash as well: the first OTA build looped at
    boot for that (`aos_ota_p4.c`, `aos_coredump_p4.c`). The flash task's stack
    is 8 KB.
- **The display path.** LVGL's draw buffers and the rotation buffer are DMA2D
  sources. From PSRAM they compete with the panel's own refresh.
- **Real-time audio and the display tasks:** `aos_aout`, the player, `lvgl`,
  `swdraw`. Their stacks are hot, and a slow cache miss there costs frames or
  audio.
- **Whatever an interrupt touches**, and whatever runs while the cache is
  off.
- **The USB host's DMA buffers** (since 2026-10-04, the 0.9 host). ESP-IDF
  can put them in PSRAM (`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM`,
  marked with an open issue on the buffers' alignment, IDF-11368), and with
  it a pendrive read 15 % slower, which would have been fine; but a CP2102
  serial port read 400 KB/s of repeated garbage, more than its cable
  carries, and closing it then aborted in the CDC-ACM driver. So they stay
  in internal RAM: each device's control buffer (4 KB, for a webcam's
  configuration descriptor), the pendrives' transfers, the webcams' and
  sound cards' isochronous ones. The class drivers' own rings and frame
  buffers (the webcam's MJPEG frames, the serial ports' 16 KB, the sound
  card's) are in PSRAM where they ask for it. `docs/USB.md`.

## The panel's three frame buffers (2026-09-29)

The DPI has three frame buffers of 1.8 MB each in PSRAM, 5.5 MB in all:
the one shown, one waiting for the next refresh, and one free for an app
(`aos_hal_display_back` / `aos_hal_display_flip`). Pref `fbs` = 1 goes back
to one. LVGL's draw buffers are 56 rows (2 x 80 KB internal). A cap of 40 rows was
tried: it freed 46 KB and let the rotation buffer exist lying down, but the
retro canvas is scaled once per chunk LVGL refreshes and 2043 lost 15 % of
its fps, so it went back (2026-09-30).

Measured with Monster Hop: upright, the bands copied straight into the free
buffer and flipped, 20 -> 22 fps with no tearing (the flip is 0.4 ms).
Lying down it stays at ~15.5 fps: the PPA turning the whole frame into the
free buffer takes 42 ms alone and 62 ms while both cores draw beside it,
because both fight for PSRAM. Turning bands of internal RAM instead (32 ms
for a frame, measured) needs two blocks of 20 KB of internal RAM an app does
not get today; bands of 4 rows hung the PPA, 8 work.

## PSRAM bandwidth: bursts flash the screen

The panel's refresh (DPI) reads its framebuffer from PSRAM, about 100 MB/s
without a pause. A burst of other PSRAM traffic can starve it: the screen
flashes white or blue, and the log says `can't fetch data from external
memory fast enough`. The first such flash came from reading every task's
whole stack for the Monitor (2026-09-29).

So long sequential passes over PSRAM (scanning, big `memcpy`) should be split
or spaced out. The Monitor now reads the high-water mark of **one** PSRAM
stack per refresh, taking turns (`aos_tasks_p4.c`).

## What already lives in PSRAM

| What | How |
|---|---|
| The apps' threads (`aos_hal_thread_start`): portal, HA, MQTT, Claude, bench, files, network tools... | `xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM)`; falls back to internal RAM |
| The shell's 5 Hz tick | a thread of its own; `app_main` returns and frees the main task's 16 KB |
| `aos_http`, `aos_radio`, `net-up`, `stats` | `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)` |
| esp_hosted's six tasks (SDIO and RPC to the C6) | its thread table, patched from a constructor (`aos_hosted_mem_p4.c`) |
| esp_hosted's SDIO buffers: mempool, streaming buffers, allocator | `heap_caps_aligned_alloc` renamed to `aos_hosted_aligned_alloc` inside its library only (`objcopy --redefine-sym`, project `CMakeLists.txt`) |
| mDNS: its task and its memory | `CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM`, `CONFIG_MDNS_MEMORY_ALLOC_SPIRAM` |
| LVGL's objects, styles and layers | `--wrap=lv_malloc_core` (`aos_lvmem.c`) |
| Every `malloc` of 1 KB or more | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` |
| lwIP's and mbedTLS's buffers | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` |
| The statics (`.bss`) of `aos_apps`, `aos_ui`, `aos_io`, `aos_portal`, `aos_flasher`, of `aos_tasks_p4`, `aos_radio`, `aos_tls` and `aos_audio_p4`, of lwIP and of mDNS (but its task's) | `components/aos_hal/psram.lf` |
| Other big statics | `AOS_BSS_PSRAM` |
| cJSON's trees (portal, HA, MQTT, Claude) | `cJSON_InitHooks` in `aos_hal_init`: each node is small enough to land in internal RAM otherwise, and a listing of 3000 files drained it |
| The SD driver's bounce buffer (32 KB) | `host.dma_aligned_buffer` at mount, instead of a new 8 KB of internal DMA RAM for every unaligned read and write |
| Copy, upload and download buffers | `aos_hal_io_alloc()`: PSRAM on a 128-byte line, which the SDMMC DMA takes directly |

Initialised statics (`.data`) cannot move to PSRAM: `.ext_ram.bss` starts
zeroed. A big struct with a few non-zero fields is left uninitialised and
set up from a constructor instead (see `aos_scope.c`, `T_defaults`).

## For new code

- A thread: use `aos_hal_thread_start`. Its stack goes to PSRAM. Reach the
  flash only through the preferences or `aos_flash_call`.
- A big buffer: `malloc`. From 1 KB up it lands in PSRAM on its own. Use
  `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)` only for what cannot live in
  PSRAM.
- Many small allocations at once (a tree, a list of thousands): under 1 KB
  each they go to internal RAM, and thousands of them drain it. Take them
  from PSRAM explicitly (`heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`), as cJSON
  now does.
- A buffer the card reads into or writes from: `aos_hal_io_alloc()`. Read
  big files with `read()` on the descriptor, not an unbuffered `fread()`,
  which newlib does a byte at a time.
- An RPC to the C6 (`esp_wifi_*`) never under LVGL's lock: it can take
  seconds when the link is busy. Keep the value somewhere and read that.
- A big static in `aos_hal`: `AOS_BSS_PSRAM`, or add the object to
  `psram.lf`. In the other components it is automatic, as long as it is
  zero-initialised.
- A fast internal buffer (a band to draw in): ask for
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA`. Without
  `MALLOC_CAP_DMA` the heap may hand out the LP SRAM
  (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`), which counts as internal
  but is slow from the HP cores. Monster Hop's second 24 KB band landed
  there: every job that drew into it ran 8-15 times slower, a `memcpy` out
  of it 119 ms against 8 (2026-09-29). With the flag the block of 24 KB
  was not there at all, and two of 12 KB were.
- Cache maintenance on PSRAM: always by address range, with
  `esp_cache_msync()`, on blocks that start and end on the 128-byte L2
  line, and never the ROM's whole-cache calls (`Cache_WriteBack_All`,
  `Cache_Invalidate_All`) while the other core runs. The app loader did
  that after every `.so` it opened, with LVGL drawing the boot screen on
  the other core, and about one boot in 18 fell over during the card's
  app scan: a module's symbol table read back broken right after
  `dlopen`, or the PSRAM heap's free lists broken under LVGL. Measured
  with `tools/boot_loop.sh`: 4 bad boots in 70 with the whole-cache
  calls, none in 80 (and none in 27 more on the release build) with the
  ranged ones (`esp_elf_arch_flush_code()` in `components/elf_loader`,
  2026-10-06). Espressif went the other way on the ESP32-S31 (loader
  v1.3.3), but there the ranges were not aligned to the line.
- Something kept across a restart in PSRAM (`EXT_RAM_NOINIT_ATTR`: a
  count of tries, a log): write it back with `esp_cache_msync(...,
  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED)` right
  after changing it. A restart drops what is only in the cache: the network
  check's count read "try 1 of 2" at every boot and the board restarted
  every minute away from home (2026-10-09). The log ring
  (`aos_logring.c`) always did it.
- Two cores for one frame: `aos_hal_worker_split()` (`aos_hal.h`). Each
  half needs its own scratch, and what they share must only be read. A
  pass bound by PSRAM gains little: measured, the 3D viewer's vertex pass
  did not speed up at all, and its triangle pass got 1.75x faster.

## Measured

On the board, home screen, Wi-Fi up, one minute after boot:

| | Before (7195942) | After |
|---|---|---|
| Static internal RAM (.data + .bss + IRAM) | 226 KB | 149 KB |
| Internal free | 46,5 KB | 125,6 KB |
| Largest internal block | 8 KB | 45 KB |
| Internal DMA, least free since boot | 2,4 KB | 47 KB |
| Task stacks in internal RAM | 128 KB (23 tasks) | 49,5 KB (13 tasks) |
| Task stacks in PSRAM | 0 | 63 KB (11 tasks) |

300 portal requests in a row, each on a thread of its own created and deleted
with its stack in PSRAM, left internal RAM and PSRAM exactly where they were
after the first hundred (the first ones fill lwIP's caches).

Stack sizes trimmed from what the board measured: `lvgl` 16 → 12 KB (7,9 KB
used), `swdraw` 8 → 4 KB each (1,2 KB used), `sys_evt` up from 2,3 to 3,5 KB
(328 bytes were left).

## Internal RAM audit (2026-09-30)

Asked by the user as a standing priority, with Bluetooth and more still to
come: everything that can go to PSRAM goes, and what stays internal is
justified. Static internal RAM had grown from 149 KB to 202 KB since the
first audit (TinyUSB's buffers, the H.264 decoder in IRAM, OTA, core dump).

**What moved** (`sdkconfig.defaults`, `components/aos_hal/psram.lf`):

| What | How | Saved |
|---|---|---|
| The H.264 decoder's hot loops (camera app only) | `CONFIG_ESP_H264_DECODER_IRAM=n` | 21 KB IRAM |
| The heap's functions | `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`: no IRAM interrupt, ours or IDF's, allocates | 8 KB IRAM |
| FreeRTOS's and the ring buffer's non-ISR functions | `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`, `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH` | ~11 KB IRAM |
| The audio's statics (paths, metadata: no interrupt reads them) | `aos_audio_p4` in `psram.lf` | 3.6 KB |
| lwIP's statics (sockets, DNS table, IPv6 neighbours) | `liblwip.a` in `psram.lf` | 5 KB |
| mDNS's statics but its task's | four objects of `libespressif__mdns.a` in `psram.lf` | 3 KB |

| | Before | After |
|---|---|---|
| Static internal RAM | 202 KB | **148 KB** |
| Internal free, home screen | 112 KB | **146 KB** |
| Internal free, least since boot | 61 KB | **93 KB** |
| Internal DMA free / largest block | 73 / 30 KB | **106 / 47 KB** |

**The trap found on the way:** a task's control block (`StaticTask_t`) and
the lists of a static queue, semaphore or event group must be in internal
RAM: `xPortCheckValidTCBMem` / `xPortCheckValidListMem` assert. The first try
moved all of mDNS, whose task's `StaticTask_t` is a static in
`mdns_service.c`: the board panicked at mDNS's start and went back to the
previous image by itself (the OTA trial). Before mapping a library's `.bss`
to PSRAM, grep it for `Static`.

**Who holds the internal RAM now** (`GET /api/heap`, home screen):

| | Bytes | Why it stays |
|---|---|---|
| LVGL's two draw buffers, 56 rows | 2 x 80 KB | DMA2D sources, and PSRAM bandwidth (see below) |
| Task stacks in internal RAM | ~58 KB | lvgl, swdraw x2, aout (display and audio: hot); esp_timer, sys_evt, ipc, idle, Tmr Svc, tiT, TinyUSB (IDF's own tasks); flashop (touches the flash). Idle, most keep 1.5-6 KB free: trimming them gains 2-3 KB with risk |
| The DMA reserve | 32 KB | `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`: not lost, a pool of its own that DMA drivers draw from; it shows as one used block in the walk |
| FreeRTOS queues and semaphores | 6 KB | must be internal |
| Small allocations (< 1 KB) | ~40 KB in ~370 blocks | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`; NVS, interrupts, drivers |

Static, what is left: the interrupt stack (4 KB), the core dump's stack
(2 KB), `esp_system`/`esp_hw_support` data, `esp_new_jpeg`'s tables (3.7 KB,
a precompiled library), and TinyUSB's buffers (21 KB, below).

**Second round, the same day: LVGL's draw buffers and TinyUSB's to PSRAM.**

| | Static internal | Internal free (home) | Least since boot | Largest DMA block |
|---|---|---|---|---|
| Morning | 202 KB | 112 KB | 61 KB | 30 KB |
| After the first round | 148 KB | 146 KB | 93 KB | 47 KB |
| **After the second** | **126 KB** | **304 KB** | **251 KB** | **139 KB** |

- **LVGL's draw buffers** are two in PSRAM, 128 rows each (pref `lvbuf`,
  `lvrows`; `aos_hal_p4.c` has the measures). Against the guess, taller
  buffers in PSRAM are as fast or faster: the whole screen redrawn upright
  in 67.0 ms with two internal x 56 rows, 66.7 with two PSRAM x 128, 60.3
  x 320; lying down 90.5 / 83.3 / 77.8. The per-chunk cost (setting up the
  copy, waiting for the DMA) is paid fewer times. Played on the board: no
  flashing, the games fine. 320 rows is the default-to-be: Monster Hop's
  map lying down once went missing, and it was not LVGL but PSRAM: the map
  (720 x thousands of pixels) was built at 3 bytes a pixel, ~12 MB at its
  peak in two blocks of one piece, and anything else growing in PSRAM left
  it without a block. Built at 2 bytes now (`mh_ui.c`, `uimg_sheet_opaque`),
  4 MB, and it logs when it fails. But Monster Hop, with 320-row buffers,
  has 320 KB of PSRAM left at its worst (880 KB with 128): 128 stays until
  the apps need less.
- **TinyUSB's buffers**: `components/aos_hal/tusb/aos_tusb_config.h`, taken
  through `CFG_TUSB_CONFIG_FILE` by TinyUSB, esp_tinyusb and aos_hal: the
  buffers in `.ext_ram.bss`, aligned and padded to 128 (PSRAM's L2 line),
  and the rounding of the DWC2 port's cache operations made 128 too. The
  disk's buffer grew from 8 to 32 KB (writes 3.3 -> 4.8 MB/s from a Mac;
  reads ~8.5, the card's own ceiling is ~10.8) and NCM got four buffers
  each way (6.75 MB/s over the cable; it was 7.4 with two internal).
- **The games' bands stay internal, measured.** Mila and Monster Hop blend
  their sprites in bands of internal RAM (44 KB, only while they are open)
  and the PPA turns each band into the panel's free buffer lying down. Pref
  `bands_psram` = 1 takes them from PSRAM, to measure: Monster Hop lying
  down fell to 9 fps (108 ms a frame), against ~20 fps (49 ms) with them
  internal; Mila's level frames took 22-27 ms against 8-12. Internal RAM
  bought twice the speed, and only while the game is open: that is what it
  is for. With the RAM this audit freed, Monster Hop lying down gets its
  bands for the first time: ~20 fps, up from ~15.5 without them.
- **A safe boot**, because these preferences are read at boot and a wrong
  one could keep the board from coming up: each boot counts itself in the
  pref `boot_bad`, `main.c` clears it after 30 s up, and the third boot of a
  run that never got there erases the tuning preferences. In NVS, so a
  hang that only ends with the power counts too. Seen working the same day.
- **`GET/POST /api/tune`**: those preferences (`lvbuf`, `lvrows`, `fbs`,
  `blit_hw`), a closed list; `GET /api/display/bench?frames=`: the whole
  screen redrawn, ms a frame.

**The levers left, largest first:**

1. ~~LVGL's draw buffers~~ and ~~TinyUSB's buffers~~: done, above. Left
   there: Monster Hop's appetite for PSRAM, which keeps the draw buffers at
   128 rows and not 320 (~10 % faster); and the network over the cable lost
   9 % with its buffers in PSRAM (bigger NTBs, which PSRAM can afford, to
   try).
3. ~~`CONFIG_SPI_FLASH_AUTO_SUSPEND`~~: **not on this board.** It would
   stop flash writes from disabling the cache (tens of KB of IRAM, and no
   more PSRAM-stack assert), and the P4 supports it, but IDF enables it only
   for the flash chips it lists, and this one is not there: the board's
   flash answers `0xC84019` (GD25Q256, 32 MB; `GET /api/ota`, `flash_id`),
   and `spi_flash_chip_gd.c` lists `0xC84016/17/18` and `0xC84319`. With
   the option on and the chip refused, the firmware would be built
   counting on a suspend that never happens: code out of IRAM running with
   the cache off at the first flash write. Patching the list means trusting
   an untested suspend with 4-byte addresses: not worth the flash.
4. **Bluetooth, when it comes:** the host stack runs on the P4 (the C6 is
   the controller, through esp_hosted). NimBLE can take its memory from
   PSRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`): that from the start.

**Tools:**

- `GET /api/heap`: the internal heap block by block, the big ones named
  after the task whose stack they are, the rest as a histogram by size.
- `GET /api/heap?trace=1` and `tools/heap_owners.py p4os.local`: who made each
  live internal allocation, with function names. Only on a diagnostic
  build: in `build/rev1_3/sdkconfig` (not the defaults) set
  `CONFIG_HEAP_TRACING_STANDALONE=y`, `CONFIG_ESP_SYSTEM_USE_FRAME_POINTER=y`
  and `CONFIG_HEAP_TRACING_STACK_DEPTH` (3 is too shallow for the stacks'
  owners; 6), build, OTA, read, and put the sdkconfig back. `main.c`
  starts the trace at the top of `app_main`; what is allocated before is
  not in it.
- `python -m esp_idf_size --archives build/rev1_3/p4os.map`: the static
  side, by library; `riscv32-esp-elf-nm -S --size-sort` on the ELF, the
  symbols in `0x4ff00000`-`0x4ffc0000`.

## Wi-Fi throughput (2026-09-29)

| | Before | After |
|---|---|---|
| Ping, idle | 206 ms | 15 ms |
| Upload to the card (portal, one 0,9 MB file) | 171 KB/s | 1,1 MB/s |
| Download from the card | 163 KB/s | 1,8 MB/s |
| 376 files, 41 MB, one request each | 9 failed, one panic | 71 s, none failed |

- Modem sleep off while the board runs on USB (`wifi_ps=1` in the
  preferences brings it back for a battery).
- lwIP's send buffer 64 KB, receive window 16 segments (23 KB). At 64 KB the
  receive side fell to 76 KB/s: the bursts outran esp_hosted's 20-slot queue.
- esp_hosted's buffers in PSRAM, as above. Its mempool keeps every block it
  ever took, and from internal RAM that walked it down until an 18 KB
  streaming buffer could not be had (`sdio_drv.c:670`). Internal RAM now
  stays put through the whole transfer.
- The receive side must stay in streaming mode: the C6's factory firmware
  sends streams, and with the host in packet mode the link died at the first
  upload.

## The card (2026-09-29)

Copying a 25 MB file inside the card, in Archivos:

| | Total | Reading | Writing |
|---|---|---|---|
| malloc'd 32 KB buffer | ~1,2 MB/s | | |
| `aos_hal_io_alloc` | 2,1 MB/s | 3,5 MB/s | 5,5 MB/s |
| + the driver's fixed bounce buffer | 2,5 MB/s | 4,3 MB/s | 6,0 MB/s |
| + `read()` instead of stdio | 2,8 MB/s | 5,3 MB/s | 6,2 MB/s |

The card itself did ~9 MB/s writing and 7-9 reading from internal RAM in
p4bench. Listing a folder of 3000 files through the portal: 0,7 s (it did
not answer in a minute with a `stat()` per file).
