# VNC

A VNC viewer for P4OS: a computer's screen on the board, used with the
finger, a USB mouse and keyboard, or the keyboard on the screen. Workshop
item T5 of `docs/internal/HANDOFF-NUEVAS-APPS.md`. Id `aos.vnc`.

It builds two ways from the same source: the simulator compiles it in
(`-DP4OS_SIM_APPS="hello_app;vnc"`), and for the board it is a dynamic app,
`tools/build_apps.sh vnc` (~60 KB). No firmware of its own: it uses
`aos_hal_tcp_*`, `aos_hal_jpeg_*`, the worker and `lv_canvas`.

## What it speaks

| | |
| --- | --- |
| versions | RFB 3.3, 3.7, 3.8; Apple's 3.889 as 3.8 |
| security | None, VNC (DES of the challenge). Not Apple's own types 30/35: on a Mac turn on "VNC viewers may control screen with password" |
| pixels | RGB565 (the "16 bits" setting, LVGL's own format: Raw lands as it comes) or 32-bit ("24 bits", exact colours) |
| encodings | Tight (fill, palette, gradient, zlib, JPEG by the P4's engine), ZRLE, Hextile, CopyRect, Raw; DesktopSize, ExtendedDesktopSize (its list of monitors) and LastRect |

A remote screen bigger than 9 MB at 16 bits (a Retina Mac's 2880x1800, a
5K) is kept at a half or a quarter: the decoders write every pixel through
`vnc_fb.c`, which drops the ones in between. The view scales that to the
board's screen, averaging up to 3x3 samples a pixel when it shrinks, so text
stays readable.

## One part of the screen

A computer with several monitors may send them as one screen (a Mac does:
two side by side make one wide picture, black wherever no monitor is). The
viewer's monitor button picks a part, and from then on the session keeps
only that rectangle, asks the server for updates of that rectangle only and
moves the pointer into it: less memory (a part that fits whole is no longer
halved), less network, more frames. The choice is saved with the computer
(`zone = x,y,w,h` in `vnc.txt`, also in the portal's card) and the next
connection starts on it; one that no longer fits (a monitor unplugged) shows
all of it.

The parts on offer are the monitors the server lists (ExtendedDesktopSize),
when it lists them, and "Mark with a finger": a drag over the remote screen
draws the rectangle, and lifting the finger shows only that. Zoomed in
first, it is as precise as wanted; it works with any computer, any
arrangement and either way the board is turned (a part "as seen now" did
not: upright, a wide monitor zoomed to the width still took in all the
height). Finding the monitors by the black between them was tried
and dropped: right on a test pattern, a real desktop (a dark wallpaper, a
lock screen) came out in eight pieces.

## Files

| | |
| --- | --- |
| `main/vnc.c` | the app: the list, the editor, the viewer, gestures, mouse and keyboard |
| `main/vnc_rfb.c` | the session in the worker: socket, handshake, encodings, input out, the two view buffers |
| `main/vnc_fb.c` | the remote screen in PSRAM and the scaled view |
| `main/vnc_kbd.c` | the keyboard on the screen, the USB keyboard's keysyms |
| `main/vnc_cfg.c` | `/data/vnc.txt`, the saved computers (also written by the page) |
| `main/vnc_des.c` | DES (FIPS 46-3), written for P4OS |
| `main/tinfl.c`, `tinfl.h` | inflate: `tinfl_decompress` from miniz 2.1.0, MIT licence (in the files), one change marked P4OS |
| `web/vnc.js` | the page in the portal: the computers, by fields |
| `tools/fake_vnc.py` | a fake server to try it all |

Third-party code: only `tinfl.c` / `tinfl.h` (miniz, Copyright 2013-2014 RAD
Game Tools and Valve Software, 2010-2014 Rich Geldreich and Tenacious
Software LLC, MIT licence, the notice is kept in both files). The only copy
of D3DES at hand carried AT&T's GPL changes, so DES was written from the
standard instead.

## Trying it without a computer

Never against the user's Mac or any real computer: the fake server.

```bash
python3 apps/vnc/tools/fake_vnc.py --port 5999 --password prueba
```

It draws a desktop that moves (a ball, a clock, a terminal that scrolls a
line a second over CopyRect, a gradient wallpaper that Tight sends as JPEG),
marks where the viewer's pointer is and where it clicked, shows what was
typed, and prints every pointer and key event. `--version 3.3|3.7|3.889`,
`--apple` (a Mac without the VNC password), `--size 2880x1800`,
`--resize-every 10 --size2 5120x2880` (DesktopSize), `--no-jpeg` (Tight's
gradient filter), `--fps`, `--monitors 1920x1080+0+0,2940x1912+1920+404`
(several monitors in one screen, black between them, as a Mac sends them)
and `--list-monitors` (and say where they are, ExtendedDesktopSize). Updates
cover only the area asked for, as a real server's. The password is a test
one: never a real one.

In the simulator, `sim/sim_fs/data/vnc.txt` (ignored by git) with
`host = 127.0.0.1`, and `VNC_OPEN=<n>` connects to the n-th computer at
start. A run with ASan (`-DCMAKE_C_FLAGS=-fsanitize=address`) over every
encoding, closing and reopening, and turning the screen came out clean.

## Input

| | touch | trackpad |
| --- | --- | --- |
| one finger, tap | click there | click at the pointer |
| one finger, drag | drag with the button down | move the pointer |
| long press | right click when lifted | right click; dragging after it drags |
| two fingers, tap | right click | right click |
| two fingers, pinch | zoom | zoom |
| two fingers, move | pan the zoomed view, or the wheel when there is nothing to pan | the wheel |

The USB mouse is the system's arrow (`aos_hwmouse.c`): its position and left
button go to the computer. Its right button reaches apps as "back", which
the viewer turns into a right click while a mouse is in use; the wheel and
the middle button do not reach apps yet (a request to the firmware in the
handoff). The USB keyboard goes straight through (`aos_ui_hwkbd_handler`);
media keys stay the system's.

While the viewer is up the system's back swipe is off (a drag from the left
edge is the remote mouse's); the bar's X leaves. Sent to the background the
session stops asking for updates and keeps the connection; back in front it
asks for the whole screen.

## On the board (to measure)

The picture goes through an `lv_canvas` that points at the session's view
buffer, as Cameras does: the regions that changed are invalidated, nothing
more. The worker logs a line every 10 s with the size, encoding, fps, kbit/s
and the milliseconds of render and decode per update. Still to measure on
the board: the fps of a full-screen update through LVGL, a Mac's ZRLE at
its real resolution, and Tight's JPEG on the P4's engine.
