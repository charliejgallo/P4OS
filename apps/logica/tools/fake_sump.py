#!/usr/bin/env python3
"""A make-believe SUMP logic analyser on a pseudo-terminal, to try the Logic
app's SUMP driver (apps/logica/main/la_sump.c) in the simulator with no
hardware. It answers as the app's own firmware does (firmware/main/main.c):
"1ALS", metadata named "P4OS LA fake", the 0xF0 edge extension, samples
newest first. The signal: a 9600 baud serial line on channel 0 sending
"SUMP ok\\r\\n", a 1 kHz square wave on 1, and a counter on 4-7.

    python3 apps/logica/tools/fake_sump.py          # prints the pty's path
    P4_SIM_UART_A=/dev/ttys012 ./build-x/p4os_sim   # then, in the app:
                                                    # Analizador, uart.a 921600
"""
import os, pty, sys, tty, select

CLOCK = 100_000_000
MEM = 262144
MAXRATE = 20_000_000


def signal(rate, n):
    """n samples at rate of the fake circuit, from t = 0"""
    out = bytearray(n)
    bit = rate / 9600.0
    frame = []
    for ch in b"SUMP ok\r\n":
        frame += [0] + [(ch >> i) & 1 for i in range(8)] + [1]
    for i in range(n):
        k = int(i / bit) % (len(frame) + 40)
        uart = frame[k] if k < len(frame) else 1
        sq = 1 if (i * 1000 // rate) % 2 == 0 else 0
        cnt = (i * 100000 // rate) & 15
        out[i] = uart | sq << 1 | cnt << 4
    return out


def main():
    master, slave = pty.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    st = dict(mask=0, val=0, rise=0, fall=0, div=99, read=4096, delay=4096, flags=0x38)
    buf = b""
    while True:
        r, _, _ = select.select([master], [], [], 1)
        if not r:
            continue
        buf += os.read(master, 4096)
        while buf:
            c = buf[0]
            if c & 0x80:
                if len(buf) < 5:
                    break
                v = int.from_bytes(buf[1:5], "little")
                buf = buf[5:]
                if c == 0xC0: st["mask"] = v & 0xFF
                elif c == 0xC1: st["val"] = v & 0xFF
                elif c == 0x80: st["div"] = v
                elif c == 0x81: st["read"] = ((v & 0xFFFF) + 1) * 4; st["delay"] = ((v >> 16) + 1) * 4
                elif c == 0x82: st["flags"] = v
                elif c == 0x83: st["delay"] = (v + 1) * 4
                elif c == 0x84: st["read"] = (v + 1) * 4
                elif c == 0xF0: st["rise"] = v & 0xFF; st["fall"] = (v >> 8) & 0xFF
                continue
            buf = buf[1:]
            if c == 0x02:
                os.write(master, b"1ALS")
            elif c == 0x04:
                m = b"\x01P4OS LA fake\x00\x02" + b"0.1\x00"
                for k, v in ((0x20, 8), (0x21, MEM), (0x23, MAXRATE), (0x24, 2)):
                    m += bytes([k]) + v.to_bytes(4, "big")
                os.write(master, m + b"\x00")
            elif c == 0x01:
                rate = CLOCK // (st["div"] + 1)
                n = min(st["read"], MEM)
                pre = n - min(st["delay"], n)
                sig = signal(rate, n * 3)
                t = pre
                # the first place the trigger holds, with pre samples before it
                if st["mask"] or st["rise"] or st["fall"]:
                    for i in range(max(1, pre), len(sig) - (n - pre)):
                        s, p = sig[i], sig[i - 1]
                        if (s & st["mask"]) == (st["val"] & st["mask"]) and (st["rise"] & ~p & s) == st["rise"] \
                                and (st["fall"] & p & ~s) == st["fall"]:
                            t = i
                            break
                win = sig[t - pre:t - pre + n]
                os.write(master, bytes(reversed(win)))
                print(f"run: {rate} Hz, {n} samples, trigger at {t}", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
