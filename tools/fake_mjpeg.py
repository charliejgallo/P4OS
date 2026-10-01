#!/usr/bin/env python3
"""
Make-believe cameras over HTTP for P4OS's Cámaras app: MJPEG streams and
single-JPEG snapshots, served on 127.0.0.1 only, so the app can be built and
measured without touching a real camera.

    tools/fake_mjpeg.py [--port 8081] [--cam patio:640x360:12] [--cam ...]
                        [--chunked] [--quality 5]

Each --cam is name:WxH:fps. For each one:

    http://127.0.0.1:<port>/<name>.mjpeg    multipart/x-mixed-replace, like go2rtc
    http://127.0.0.1:<port>/<name>.jpg      one JPEG and close, like a snapshot URL

The picture is a light room with Mila, the black kitten, walking across it,
the camera's name, the time and a frame counter (so a frozen or dropped frame
shows); without her picture (--overlay, or ESP32S3_AmoledOS next to this
repo) it is ffmpeg's testsrc2. ffmpeg makes the frames (-re, at the camera's
rate); every client gets the newest one, like a real camera.

--chunked answers HTTP/1.1 with Transfer-Encoding: chunked, as ffmpeg's own
HTTP server does even to HTTP/1.0 requests (AmoledOS found its chunk lines
inside JPEGs; cam_http.c undoes them).

The standard library and ffmpeg only. fake_rtsp.py and fake_frigate.py use
scene_args() and Source from here.
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MILA = os.path.join(os.path.dirname(REPO), "ESP32S3_AmoledOS", "apps", "mila", "assets", "_sample",
                    "mila_walk_e.png")
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"
FFMPEG = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"

COLORS = ["0xF2E8D8", "0xE3EEF2", "0xEFE3EC", "0xE6F0E0", "0xF4EFD9", "0xE9E6F4"]


def parse_cam(text):
    """name:WxH:fps -> (name, w, h, fps)"""
    parts = text.split(":")
    name = parts[0]
    w, h = (640, 360)
    fps = 12
    if len(parts) > 1 and "x" in parts[1]:
        w, h = (int(v) for v in parts[1].split("x"))
    if len(parts) > 2:
        fps = float(parts[2])
    return name, w, h, fps


def scene_args(name, w, h, fps, index=0, overlay=None):
    """ffmpeg input and filter arguments for a camera's picture."""
    overlay = overlay if overlay is not None else (MILA if os.path.exists(MILA) else "")
    bg = COLORS[index % len(COLORS)]
    font = ("fontfile=%s:" % FONT) if os.path.exists(FONT) else ""
    label = name.replace(":", " ")
    text = (",drawtext=%stext='%s  %%{localtime\\:%%H\\\\\\:%%M\\\\\\:%%S}  #%%{n}':"
            "x=24:y=24:fontsize=%d:fontcolor=0x333333" % (font, label, max(16, h // 18)))
    floor = (",drawbox=x=0:y=%d:w=%d:h=%d:color=0xC8B8A0:t=fill"
             ",drawbox=x=%d:y=%d:w=%d:h=%d:color=0xBFDDF0:t=fill"
             ",drawbox=x=%d:y=%d:w=%d:h=%d:color=0xFFFFFF:t=6"
             % (h * 3 // 4, w, h - h * 3 // 4,
                w * 60 // 100, h * 12 // 100, w * 26 // 100, h * 34 // 100,
                w * 60 // 100, h * 12 // 100, w * 26 // 100, h * 34 // 100))
    if overlay:
        ch = h * 60 // 100
        speed = w / 7.0
        return ["-f", "lavfi", "-i", "color=c=%s:s=%dx%d:r=%g" % (bg, w, h, fps),
                "-loop", "1", "-framerate", "%g" % fps, "-i", overlay,
                "-filter_complex",
                "[1:v]scale=-1:%d[cat];[0:v]%s[room];"
                "[room][cat]overlay=x='mod(t*%g\\,W+w)-w':y='H*3/4-h*0.62-abs(10*sin(t*5))':shortest=0%s,format=yuv420p[v]"
                % (ch, floor.lstrip(","), speed, text),
                "-map", "[v]"]
    return ["-f", "lavfi", "-i", "testsrc2=s=%dx%d:r=%g" % (w, h, fps),
            "-vf", "format=yuv420p" + text, "-map", "0:v"]


class Source:
    """One camera: an ffmpeg making JPEGs at its rate; the newest is kept."""

    def __init__(self, name, w, h, fps, index=0, quality=5, overlay=None):
        self.name, self.w, self.h, self.fps = name, w, h, fps
        self.frame = None
        self.seq = 0
        self.cond = threading.Condition()
        args = [FFMPEG, "-hide_banner", "-loglevel", "error", "-re"] + \
            scene_args(name, w, h, fps, index, overlay) + \
            ["-c:v", "mjpeg", "-q:v", str(quality), "-f", "mjpeg", "pipe:1"]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        out = self.proc.stdout
        while True:
            chunk = out.read1(65536) if hasattr(out, "read1") else out.read(65536)
            if not chunk:
                return
            buf += chunk
            while True:
                s = buf.find(b"\xff\xd8")
                e = buf.find(b"\xff\xd9", s + 2) if s >= 0 else -1
                if s < 0 or e < 0:
                    break
                jpg = buf[s:e + 2]
                buf = buf[e + 2:]
                with self.cond:
                    self.frame = jpg
                    self.seq += 1
                    self.cond.notify_all()

    def wait(self, after_seq, timeout=5.0):
        with self.cond:
            self.cond.wait_for(lambda: self.seq != after_seq, timeout)
            return self.seq, self.frame

    def stop(self):
        self.proc.terminate()


def make_handler(sources, chunked):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1" if chunked else "HTTP/1.0"

        def log_message(self, fmt, *a):
            sys.stderr.write("[mjpeg] %s %s\n" % (self.address_string(), fmt % a))

        def _body(self, data):
            if chunked:
                self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
            else:
                self.wfile.write(data)

        def do_GET(self):
            path = self.path.split("?")[0].lstrip("/")
            name, _, ext = path.rpartition(".")
            src = sources.get(name)
            if not src or ext not in ("mjpeg", "jpg"):
                self.send_error(404)
                return
            if ext == "jpg":
                _, frame = src.wait(-1)
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                if chunked:
                    self.send_header("Transfer-Encoding", "chunked")
                else:
                    self.send_header("Content-Length", str(len(frame)))
                self.send_header("Connection", "close")
                self.end_headers()
                self._body(frame)
                if chunked:
                    self.wfile.write(b"0\r\n\r\n")
                return
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-cache")
            if chunked:
                self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            seq = -1
            try:
                while True:
                    seq, frame = src.wait(seq)
                    if frame is None:
                        continue
                    part = (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(frame)) \
                        + frame + b"\r\n"
                    self._body(part)
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--cam", action="append", default=[], help="name:WxH:fps")
    ap.add_argument("--chunked", action="store_true")
    ap.add_argument("--quality", type=int, default=5, help="ffmpeg -q:v, 2 (best) .. 31")
    ap.add_argument("--overlay", default=None, help="a PNG walking across the picture")
    a = ap.parse_args()
    cams = a.cam or ["patio:640x360:12", "cocina:640x480:10"]
    sources = {}
    for i, c in enumerate(cams):
        name, w, h, fps = parse_cam(c)
        sources[name] = Source(name, w, h, fps, i, a.quality, a.overlay)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(sources, a.chunked))
    srv.daemon_threads = True
    for name, s in sources.items():
        print("http://127.0.0.1:%d/%s.mjpeg  (%dx%d @ %g)   .jpg for one picture" % (a.port, name, s.w, s.h, s.fps))
    sys.stdout.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for s in sources.values():
            s.stop()


if __name__ == "__main__":
    main()
