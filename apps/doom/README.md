# Doom

Chocolate Doom, through [doomgeneric](https://github.com/ozkl/doomgeneric), as
an ordinary app from the card: a 440 KB `.so` that the firmware loads like
any other, with no line of the firmware changed for it. It came from
AmoledOS, where it ran at 35 fps on a watch's ESP32-S3. On the P4 it is
always in landscape: Doom's 320x200 scaled x3 to 960x600 in the middle of the
1280x720 screen, with the controls in the bands around it.

On the board it has **not been measured yet** (see "What the board must
measure" below). In the simulator it runs at 35 fps, Doom's own tic rate and
so its ceiling.

## The WAD

The game's data (levels, graphics, sounds, music) lives in a WAD file, and
the WAD is **not** part of this app, of the repository or of the releases:
you bring it, and copy it to the card's `doom/` folder (the folder is also
created on the first run), with the card in a reader or through the portal's
file explorer (`PUT /api/fs/put?path=/doom/doom1.wad`). Without a WAD the
app says which folder it looked in.

The config (`default.cfg`, `doomgenericdoom.cfg`) and the saves (`saves/`)
land in the same folder.

### What it was tested with

**The shareware `DOOM1.WAD`, version 1.9**: episode 1, *Knee-Deep in the
Dead*, nine levels, 4,196,020 bytes, SHA-1
`5b2e249b9c5133ec987b3ea77596381dc0d6bc1d`. The AmoledOS numbers in this
README were measured with it, on two watches; the P4 port was tried with it
in the simulator.

### What its licence allows

The shareware WAD is **id Software's copyrighted data**, not free software
and not under the GPL that covers the code. What makes it usable here is
the licence it has always shipped with: it may be **copied and given to
other people, for free**, whole and unmodified; nobody may charge for it or
for its use. John Carmack put it shortly: the Doom shareware WAD is freely
distributable. That is why so many source ports and Linux distributions
point you to it (Ubuntu has packaged it as `doom-wad-shareware`).

What it does **not** allow: selling it or charging for it, modifying it, or
treating it as open data. id also asked back then that nobody make levels
that run on the shareware version, since it was the demo for the paid game.

This repository does not ship it anyway: the code is GPL v2 and the WAD's
terms are different, so it stays a separate download. This is a summary to
explain the choice, not legal advice; the licence text comes with the
original shareware package. Sources:
[the copyright file of Ubuntu's doom-wad-shareware](https://launchpad.net/ubuntu/trusty/+source/doom-wad-shareware/+copyright)
and [the Doom Wiki's page on licences](https://doomwiki.org/wiki/Licences).

### The full games (in theory)

Chocolate Doom plays every official IWAD, and the app looks for them
**before** the shareware one, in this order: `doom.wad` (registered or *The
Ultimate Doom*), `doom2.wad`, `doomu.wad`, `plutonia.wad` and `tnt.wad`
(*Final Doom*), then `doom1.wad`, then Freedoom's `freedoom1.wad` and
`freedoom2.wad`. So with a WAD from a copy of the game you own (the Steam
and GOG editions carry them) it **should** work: the engine is the same and
nothing here is specific to episode 1. **It is not tested**, and two things
to know:

- The commercial WADs are **not** redistributable: yours, for your board.
- They are big (`doom.wad` ~12 MB, `doom2.wad` ~14 MB): the card in a reader
  is the easy way. Doom II's bigger levels want more of the zone; on the P4
  it is 8 MB, what a DOS PC for Doom II had, but it is not measured.

[Freedoom](https://freedoom.github.io/) is the other road: complete games
built from new, BSD-licensed data, which can be shared freely.

## Playing

The screen turns to landscape when Doom opens and back when it closes
(`AOS_APP_FLAG_LANDSCAPE`). Both thumbs play at once: the touch reads two
fingers, and each finger owns what it landed on until it lifts.

```
 +--------+------------------------------------------+--------+
 |  MENU  |                                          |  MAP   |
 |        |                                          | WEAPON |
 |        |       Doom, 320x200 x3 = 960x600         | STRAFE |
 |        |                                          |  USE   |
 | stick  |       pressing the picture fires         |  FIRE  |
 |        |                                          |        |
 +--------+------------------------------------------+--------+
   160 px          60 px above and below               160 px
```

- **The stick**, in the left band, appears where the thumb lands: that
  point is neutral, and the thumb 60 px away is a full push. It is
  analogue: a small push turns and walks slowly, the edge runs and turns
  like the keyboard's fast turn. It always runs (Doom's old `joyb_speed 31`).
  It keeps working when the thumb slides onto the picture.
- **FIRE** fires, and so does **pressing the picture** (the strips above and
  below it too). The right band is split in rows, so a thumb that lands a
  little off a button still gets that button.
- **USE** opens doors and presses switches.
- **STRAFE** is a latch: a tap turns it on (it stays lit), another turns it
  off. While it is on the stick sidesteps instead of turning, so it walks in
  all four directions with the view fixed, and the right thumb stays free
  for FIRE.
- **MENU** is Doom's own menu (Escape). In a menu the stick moves the cursor,
  FIRE picks (Enter), USE goes back; on a yes/no question FIRE is *yes* and
  USE is *no*.
- **MAP** is the automap, **WEAPON** the next weapon.
- Saving: the slot gets a name (`SLOT 1`...) and one more FIRE confirms it,
  since there is no keyboard.
- **Leaving:** Quit Game in Doom's menu, or **holding MENU for two
  seconds**, which works even if the engine stopped answering. While a game
  runs the system's edge gestures are off: a thumb pushing the stick from
  the bottom edge would go home, and a panel pulled down from the top would
  be hidden under the picture. They come back when Doom stops.

**A USB gamepad** plays too, through the same buttons as the fingers
(`gamepad_poll` in doom.c): the left stick or the d-pad walks and turns
(the arrows in a menu), A is FIRE (Enter, *yes*), B is USE (back, *no*),
START is MENU and held two seconds leaves, L and R sidestep (Doom's own
strafe keys, `DP_BTN_STRAFE_L/_R`, which have no button on the screen) and
L and R together are WEAPON. A thumb on the screen's stick wins over the
pad's stick. There is no MAP on the pad: its button is on the screen.

Sound effects and **the music** play through the speaker: Doom's own OPL2
soundtrack, synthesised on the board. Doom's Sound menu sets both volumes,
and the options (volumes, screen size, mouse sensitivity, which is the
stick's turn speed) are kept in `default.cfg` between games.

## How it is put together

```
main/doom.c          the app: the layout, the pad, the blit, the life cycle (LVGL's task)
main/doom_port.h     the seam between the two sides, nothing but plain memory
main/port/           the platform layer doomgeneric asks for
    dg_system.c      the engine's life: worker, exit(), memory, files, stdout
    dg_video.c       I_VideoBuffer -> RGB565 320x200 in three frame slots
    dg_input.c       the pad as keys, the stick as a mouse
    dg_sound.c       the mixer: effects, plus the music on the same clock
    dg_opl.c         opl.h on DOSBox's OPL2 emulator, without threads
    dg_compat.h      what the engine sees instead of the C library
main/doomgeneric/    the engine, vendored (see below)
```

**Two tasks, two cores.** The engine runs in the app's worker on core 0 at
priority 3 (`aos_hal_worker_start_on`; the HAL's defaults, below LVGL's 4
and level with the apps' threads), and never touches LVGL or the
framebuffer. Each finished frame goes through the palette into one of three
320x200 RGB565 slots, little-endian. LVGL's task, pinned to core 1, takes the
newest one every 8 ms and hands it to
`aos_hal_display_blit_scaled(160, 60, 320, 200, frame, 3, false)`, which
scales it x3, turns it for the portrait framebuffer and writes it there in
one pass of the PPA (bilinear, slightly soft; with the preference
`blit_hw` = 0 the CPU does it, nearest neighbour). The blit returns with the
picture in the framebuffer, so the slot is free again at once. The slots
change hands with a compare-and-swap, and a frame nobody took yet is simply
overwritten by a newer one, so the engine never waits on the display.

**Nothing of LVGL's over the picture.** LVGL knows nothing of what the blit
wrote: its next redraw over that area wins until the next frame. So the pad
lives entirely in the side bands, the stick's base stays whole inside its
band and its knob travels less than the thumb (40 px drawn for 60 px of
thumb), and the "loading" label is drawn away before the first blit. In the
simulator, where the blit returns false, the frame goes through an LVGL
canvas scaled x3 on the CPU instead; on the board that canvas never exists.

**exit() is a longjmp.** Chocolate Doom has no way out but `exit()`, from the
Quit menu, from `I_Error` or anywhere else; on the board there is no process
to end. `dg_compat.h`, included by every engine file through `doomtype.h`,
reroutes it to a `longjmp` back to the top of the worker, and so does a stop
asked by the app when it closes. From there every block the engine allocated
and every file it opened is given back: the heap is not the `.so`'s and would
outlive it. Measured on the watch: after playing, the PSRAM free was within
200 bytes of what it was before opening the app.

**Memory.** The engine's heap is PSRAM, always (small blocks would otherwise go
to internal RAM, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`). Doom's zone takes what
the PSRAM gives in one block, up to 8 MB and leaving 2 MB for the system; with
32 MB of PSRAM that is the whole 8 MB. The frame slots are 3 x 125 KB of
PSRAM, the palette 512 bytes of internal RAM. The worker's stack is 16 KB, in
PSRAM on the P4 (the engine never reaches the internal flash, which a task
with a PSRAM stack must not do, docs/MEMORY.md); its peak measured on the
watch was 3.9 KB.

**The stick is a mouse.** Doom adds a mouse's motion to the tic it is building
(`angleturn -= mousex * 8`, `forward += mousey`), so posting one `ev_mouse` per
tic makes an analogue stick with no change to the game. With STRAFE down
(Doom's own `key_strafe`, held while the latch is on) the same sideways
motion becomes `side += mousex * 2`, a sidestep. In the menus and on the
screens between levels the stick becomes the arrow keys, with auto-repeat.

**The music is the Sound Blaster's.** Doom's music is MUS, which DMX played
on the OPL2 FM chip with the instrument patches in the WAD's `GENMIDI` lump.
Chocolate Doom 2.2.1's player does the same (`i_oplmusic.c`, MUS converted
to MIDI and stepped through by timed callbacks) on DOSBox's DBOPL emulator,
both GPL v2 and vendored here. Their SDL driver ran the chip in SDL's audio
thread; `port/dg_opl.c` implements the same `opl.h` with no threads at all:
the mixer asks for as many samples as it is about to queue, and the chip is
generated up to each callback's time, which then runs. The song's clock is
the sample count, so the tempo holds whatever the frame rate does. DBOPL
generates at 16 kHz directly (the HAL resamples to its 48 kHz bus) and skips
silent channels, so it costs what the track asks for: on the watch's 240 MHz
S3, **3-7 % of a core** in E1M1 and **9-13 %** on the title and in E1M3,
which use more voices. On the P4 the log line gives the same share of core 0
(`rdcycle`, over 360 MHz). A one-pole high-pass takes off the DC DBOPL's
output carries, and the music is mixed at four times the emulator's level:
at 1:1, E1M1 measured 470 RMS against the pistol's 20000 peaks. A soft knee
above three quarters of full scale keeps a shotgun over the music from
clipping flat.

**One frame per tic.** `TryRunTics` returns to redraw after a tic's worth of
waiting even when no tic ran, and drawing the same state again is a whole core
for nothing: 46 frames a second for 35 tics. The loop now draws only when
`gametic` moved.

## The engine

`main/doomgeneric/` is doomgeneric at
`dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284`, only the files the port uses
(the SDL, X11, Allegro and Windows back ends and the original `i_video.c` are
left out), plus the music from Chocolate Doom 2.2.1 (tag
`chocolate-doom-2.2.1`): `i_oplmusic.c`, `midifile.c/.h`, `dbopl.c/.h`,
`opl.h` and `opl_queue.c/.h`. Every change is marked `AmoledOS:` in the
source, where they were made; the P4 needed none of its own:

| File | Change |
| --- | --- |
| `doomtype.h` | includes `port/dg_compat.h` |
| `doomfeatures.h`, `i_sound.c` | sound on, through `port/dg_sound.c`, no SDL_mixer |
| `i_system.c` | the zone from `dg_zone_alloc()`; `I_Quit` and `I_Error` leave through the port |
| `m_config.c` | config and saves in `<card>/doom/`, the saves folder without a dot; saving and loading the config back on (off in doomgeneric), a line at a time, without the keys |
| `m_controls.c` | always run; `[` and `]` bound to previous/next weapon |
| `m_menu.c` | a name for an empty save slot; `dg_menu_state()` for the pad |
| `d_main.c` | no ENDOOM; draw only when a tic ran |
| `doomgeneric.c` | no 1 MB RGBA screen |
| `i_input.c` | the stick, once per tic |
| `v_video.c` | a float compare instead of a double one |
| `m_misc.c` | `M_TempFile` on the card: there is no `/tmp` |
| `i_oplmusic.c` | the module under the name `i_sound.c` looks for; Chocolate 2.2's `opl_driver_ver_t` |
| `dbopl.c` | tables and rate set-up in `float` (bit-exact against the `double` original on a test note) |
| `midifile.c` | its own big-endian swaps instead of SDL's |
| `opl_queue.c` | the engine's malloc; tempo changes in 32 bits |

The simulator builds the engine and the port as a library of their own
(`sim/CMakeLists.txt`), without our warnings, and the app on top. In the
simulator the engine's globals live as long as the process, so Doom runs once
per simulator run; on the board every open is a fresh `dlopen`.

## Traps

- **A byte swap that called itself.** `sha1.c` shifts bytes around and gcc
  turns that into `__bswapsi2`, which the firmware does not lend; the port
  supplies it. Written with the same shifts, gcc recognised the idiom inside
  the replacement too and compiled it into a call to itself. The recursion ate
  the worker's stack and then the kernel's lists beside it, and the watch
  died three different ways, always on the *other* core: a corrupted TCB in
  the tick interrupt, the scheduler reading `0xa5a5a5a5`, an `assert` in the
  SPI driver. The give-away was the backtrace: one address of the `.so`
  repeated with frames 32 bytes apart. It is built at `-O0` now. The P4's
  RISC-V has no byte-swap instruction either (no Zbb), so the replacement is
  still needed; the P4 build's `objdump` shows no call in it, only loads,
  shifts and ors, then `ret`.
- **`build_apps.sh` checks every undefined symbol**, and it caught the only
  two the engine needed from outside: that `__bswapsi2` and a `double`
  compare in `v_video.c`. Everything else is in the firmware's table: the P4
  build needs 104 symbols, all lent.
- **Local paths in the `.so`.** From AmoledOS v0.5.6 to v0.7.0 `doom.so`
  carried `/Users/...` in its `__FILE__` strings. `elf_loader.cmake` maps
  them away with `-ffile-prefix-map`, and `build_apps.sh` refuses any `.so`
  that still has one; this one has none.
- **`OPL_Delay` would hang forever.** Chocolate's version sets a callback and
  waits on a condition variable for the audio thread to fire it; with the
  chip generated from the game's own task nobody ever would. The only caller
  is the chip detection, which a software chip skips.
- **The music brought four more symbols** the firmware does not lend:
  `__assert_func` (now an `I_Error`, which shows and returns to the OS
  instead of restarting the board), `fgetc` (through `dg_compat.h`), and
  float to 64-bit conversions in the callback queue's tempo change (done in
  32 bits: a pending callback is seconds away at most).
- **The config kept nothing.** doomgeneric ships `M_SaveDefaults` and its
  loader switched off. Switched on as they were, the keys would break: they
  are written as keyboard scan codes and doomgeneric's own have none
  (`KEY_FIRE` is `0xa3`), so they would read back as 0. The keys are left
  out; the pad owns them anyway.
- **A DC blocker that stuck.** `y * 1019 >> 10` rounds negatives down, and
  any output a couple of hundred below zero was a fixed point: silence came
  out as a constant -260. `/ 1024` truncates towards zero and it decays.
  Found in the simulator with `DOOM_WAV=<file>`, which dumps the mix as raw
  16 kHz mono so it can be measured without listening.
- **The simulator's second finger has to move.** The simulator counts a new
  touch sample only when something changed, and a second finger counts after
  two samples, so a scripted `pinch` with the same start and end distance
  never brings it. `pinch 640 440 1100 1140 1500` puts one thumb on the stick
  and the other on FIRE.
- **LVGL's snapshot does not see the game on the board.** The picture goes
  to the framebuffer past LVGL; `aos_hal_display_fb()` reads it back. The
  simulator's shot does see it, because there it is a canvas.

## What the board must measure

- The frame rate in E1M1 and the blit's cost. Every 5 s the log says
  `level E1M1: N fps rendered, N shown, blit N us | audio N% of core 0
  (music N%) | internal N B, psram N B`. The blit test measured 6.8 ms for
  this size.
- Whether the PSRAM traffic (the renderer's zone, plus the PPA writing 1.1 MB
  a frame) starves the panel's refresh: a flash of the screen and
  `can't fetch data from external memory fast enough` in the log
  (docs/MEMORY.md).
- The music's share of core 0, and that the sound does not stutter with the
  engine at priority 3 beside the portal.
- The two thumbs on the GT911: stick and FIRE held together, USE while
  walking, a quick tap on FIRE reaching the engine (a press lasts at least
  70 ms).
- The PSRAM free after closing Doom against before opening it.

## Licence

Doom's source code, Chocolate Doom's music player and DOSBox's DBOPL, and so
this app, are under the **GNU GPL v2** (`main/doomgeneric/LICENSE`); the
rest of P4OS is MIT. The WAD files are not part of it: the shareware one is
freely distributable, the commercial ones are not.
