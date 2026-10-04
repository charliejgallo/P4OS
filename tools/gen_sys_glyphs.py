#!/usr/bin/env python3
"""
System glyphs of the P4OS shell: the status bar, the control centre, the
notification centre, the home widgets and the list rows of Settings.

One catalogue of Material Design Icons (the same release the folder glyphs and
the watch's Settings use), rendered at three sizes:

    aos_sym_28   status bar and list rows
    aos_sym_44   control-centre tiles, widgets
    aos_sym_72   the big glyph of a toggle or an empty state

and a header with every glyph as a UTF-8 string, AOS_SYM_<NAME>, usable with
any of the three fonts.

    tools/gen_sys_glyphs.py        (needs lv_font_conv; MDI is cached by
                                    tools/gen_folder_glyphs.py)
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_folder_glyphs as mdi  # noqa: E402  fetch(), MDI_VERSION, utf8_escape()

REPO = mdi.REPO
SIZES = [28, 44, 72]

GLYPHS = [
    # connectivity
    "wifi-strength-off-outline", "wifi-strength-outline", "wifi-strength-1",
    "wifi-strength-2", "wifi-strength-3", "wifi-strength-4", "wifi", "wifi-off",
    "bluetooth", "bluetooth-off", "bluetooth-connect", "access-point", "lan",
    "home-assistant", "server-network", "cloud-outline", "earth",
    # storage, ports, modules
    "sd", "usb", "usb-port", "serial-port", "chip", "expansion-card", "memory",
    "power-plug", "connection",
    # battery
    "battery-outline", "battery-20", "battery-50", "battery-80", "battery",
    "battery-charging", "battery-unknown",
    # system
    "bell", "bell-off", "bell-outline", "moon-waning-crescent", "screen-rotation",
    "phone-rotate-portrait", "phone-rotate-landscape", "brightness-5",
    "brightness-6", "brightness-7", "volume-high", "volume-medium", "volume-low",
    "volume-off", "cog", "magnify", "lock", "lock-open-variant", "power",
    "restart", "monitor-screenshot", "monitor", "fullscreen", "apps",
    "view-grid-outline", "folder", "delete", "pencil", "plus", "minus", "close",
    "check", "chevron-left", "chevron-right", "chevron-up", "chevron-down",
    "information-outline", "alert-outline", "dots-horizontal", "drag",
    "translate", "palette", "image", "clock-outline", "timer-outline",
    "alarm", "calendar", "account-circle", "av-timer", "timer-sand", "brain",
    "flag-outline", "bell-ring", "minus-circle", "calendar-today", "swap-horizontal",
    "grid", "bomb", "gamepad-variant", "dice-5", "cards-playing", "puzzle", "download", "update", "shield-check",
    # media
    "play", "pause", "skip-next", "skip-previous", "stop", "music", "radio",
    "phone", "phone-hangup",
    "microphone", "speaker",
    "shuffle-variant", "music-note", "folder-music", "image-multiple", "image-broken-variant",
    # home
    "lightbulb", "lightbulb-outline", "lightbulb-group", "thermometer",
    "water-percent", "weather-partly-cloudy", "fan", "air-conditioner",
    "television", "sofa", "door", "door-open", "garage", "blinds",
    "window-shutter", "robot-vacuum", "cctv", "motion-sensor", "lightning-bolt",
    "flash", "gauge", "chart-line", "robot", "cat",
    # Conversor (magnitudes, keypad) and Life
    "ruler", "weight-kilogram", "cup-water", "speedometer", "vector-square", "database",
    "engine", "angle-acute", "backspace-outline", "swap-vertical", "plus-minus-variant",
    "eraser", "ladybug", "shape-outline", "brush",
    # Home Assistant
    "power-plug-off", "toggle-switch", "fan-off", "blinds-open", "garage-open", "script-text",
    "gesture-tap-button", "star", "star-outline", "window-open", "window-closed", "molecule-co2",
    "radiator", "snowflake", "fire",
    # Terminal
    "record-rec",
    # Banco (bench): the scope and the generator
    "sine-wave", "waveform", "square-wave", "triangle-wave", "sawtooth-wave", "auto-fix",
    "step-forward", "trending-up", "trending-down", "arrow-expand-vertical",
    "arrow-expand-horizontal", "lan-connect", "lan-disconnect",
    # MQTT, Red, Monitor, Archivos, Claude
    "send", "message-text-outline", "pound", "radar", "ip-network-outline", "lan-pending",
    "router-wireless", "cpu-64-bit", "file-outline", "file-document-outline", "file-image-outline",
    "file-music-outline", "file-code-outline", "folder-open", "content-copy", "content-cut",
    "content-paste", "sort", "package-variant", "chart-areaspline", "arrow-up", "arrow-left",
    "timer-outline", "pulse", "dns", "web", "file-delimited-outline", "folder-plus", "pin", "pin-off",
    # Cámaras, Macro pad, Módulos, the retro canvas
    "video", "video-off", "record", "grid-large", "camera", "keyboard", "keyboard-outline", "mouse",
    "cursor-default-click", "rocket-launch", "play-pause", "microphone-off", "application-brackets",
    "led-on", "electric-switch", "developer-board", "resistor", "arrow-left-bold", "arrow-right-bold",
    "arrow-up-bold", "arrow-down-bold", "circle", "circle-outline", "pause-circle", "trophy",
]

HEADER = os.path.join(REPO, "components", "aos_ui", "include", "aos_sys_glyphs.h")


def main():
    paths = mdi.fetch() or {k: os.path.join(mdi.CACHE, k) for k in mdi.FILES}
    import json
    meta = {m["name"]: m for m in json.load(open(paths["meta.json"]))}
    missing = [g for g in GLYPHS if g not in meta]
    if missing:
        sys.exit("not in MDI %s: %s" % (mdi.MDI_VERSION, ", ".join(missing)))
    cps = [int(meta[g]["codepoint"], 16) for g in GLYPHS]
    ranges = ",".join("0x%X" % c for c in cps)
    for px in SIZES:
        name = "aos_sym_%d" % px
        out = os.path.join(REPO, "components", "aos_ui", name + ".c")
        subprocess.run(["lv_font_conv", "--no-compress", "--no-prefilter", "--bpp", "4",
                        "--size", str(px), "--font", paths["mdi.ttf"], "-r", ranges,
                        "--format", "lvgl", "--lv-font-name", name,
                        "--force-fast-kern-format", "-o", out], check=True)
        src = open(out).read()
        src = re.sub(r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE.*?#endif\n', '#include "lvgl.h"\n', src,
                     count=1, flags=re.S)
        src = src.replace(paths["mdi.ttf"], "materialdesignicons-webfont.ttf %s" % mdi.MDI_VERSION)
        src = src.replace(out, "components/aos_ui/%s.c" % name)
        open(out, "w").write(src)
        print("  %s  %d glyphs" % (name, len(GLYPHS)))

    lines = ["/* Generated by tools/gen_sys_glyphs.py from Material Design Icons %s." % mdi.MDI_VERSION,
             " * Do not edit: change GLYPHS there and run it again.",
             " *",
             " * Every glyph as a UTF-8 string, for a label whose font is one of",
             " * aos_sym_28 / aos_sym_44 / aos_sym_72. */",
             "#pragma once", "", '#include "lvgl.h"', ""]
    lines += ["LV_FONT_DECLARE(aos_sym_%d);" % px for px in SIZES]
    lines.append("")
    for g, cp in zip(GLYPHS, cps):
        lines.append('#define AOS_SYM_%-28s "%s"' % (g.upper().replace("-", "_"), mdi.utf8_escape(cp)))
    lines.append("")
    open(HEADER, "w").write("\n".join(lines))
    print("  %s" % os.path.relpath(HEADER, REPO))


if __name__ == "__main__":
    main()
