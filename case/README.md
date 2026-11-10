# The case

A 3D-printed case for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 running
P4OS: the one in the photos of the README and of [docs/USB.md](../docs/USB.md).

## v1

| File | What it is |
|---|---|
| [v1/p4os-case.stl](v1/p4os-case.stl) | the enclosure, remixed: the back holds the two USB-A sockets of the USB host, wired to the 40-pin header ("2.0" on pins 25/27, "1.1" on pins 21/23; [docs/USB.md](../docs/USB.md), "The ports") |
| [v1/p4os-buttons.stl](v1/p4os-buttons.stl) | the buttons (BOOT, RESET and POWER) |

It is a remix of
[Waveshare ESP32-P4 5inch Touchscreen Enclosure](https://www.printables.com/model/1787054-waveshare-esp32-p4-5inch-touchscreen-enclosure)
on Printables, which its author published as **public domain (CC0)**. This
remix is CC0 as well: print it, change it, sell it, no credit needed. The
credit above is given anyway.

This is the case as it is on the bench today. It will change when the
auxiliary board for the header ([docs/plan/PCB.md](../docs/plan/PCB.md)) is
done: the sockets and the wires behind the header become that board.

## The board's own 3D model

Waveshare publishes the board's 3D model with its
[hardware files](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5/tree/main/hardware).
It is theirs and it is large, so it is not in this repository: a copy kept
here as `ESP32-P4-WIFI6-Touch-LCD-5_3d-*.stl` is ignored by git.
