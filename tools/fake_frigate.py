#!/usr/bin/env python3
"""
A make-believe Frigate NVR for P4OS's Cámaras app: the few HTTP API calls the
Frigate tab makes, on 127.0.0.1 only, with cameras that move (fake_mjpeg.py's
picture: Mila walking through a room) and a list of recent events that grows.

    tools/fake_frigate.py [--port 5000] [--cam entrada:1280x720:10] [--cam ...]
                          [--events 8] [--every 45]

    GET /api/config                          {"cameras": {...}}; one of them disabled
    GET /api/events?limit=N                  newest first, Frigate 0.13/0.14 fields
    GET /api/events/<id>/thumbnail.jpg       175x175
    GET /api/events/<id>/snapshot.jpg        the camera's size
    GET /api/<camera>/latest.jpg?h=360       the newest frame (h is ignored)
    GET /api/<camera>?fps=10&h=720           MJPEG, as Frigate's debug view

A new event appears every --every seconds on a random camera. The snapshot
and thumbnail of an event are the camera's frame at the moment it was made.
The standard library and ffmpeg only.
"""
import argparse
import json
import os
import random
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_mjpeg  # noqa: E402

LABELS = [("person", 0.86), ("cat", 0.91), ("car", 0.78), ("dog", 0.74), ("package", 0.69)]


def thumbnail(jpg):
    """175x175 centre crop of a JPEG, through ffmpeg."""
    p = subprocess.run([fake_mjpeg.FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "mjpeg", "-i", "pipe:0",
                        "-vf", "crop='min(iw,ih)':'min(iw,ih)',scale=175:175", "-frames:v", "1",
                        "-f", "mjpeg", "-q:v", "4", "pipe:1"], input=jpg, capture_output=True)
    return p.stdout


class Frigate:
    def __init__(self, cams, n_events):
        self.sources = {}
        self.lock = threading.Lock()
        self.events = []
        for i, c in enumerate(cams):
            name, w, h, fps = fake_mjpeg.parse_cam(c)
            self.sources[name] = fake_mjpeg.Source(name, w, h, fps, i + 1)
        now = time.time()
        # a history, oldest first: the last one a few minutes ago
        for k in range(n_events):
            self.add_event(now - (n_events - k) * 900 - 240, wait=(k == n_events - 1))

    def add_event(self, start=None, wait=True):
        cam = random.choice(list(self.sources))
        src = self.sources[cam]
        _, jpg = src.wait(-1 if wait else src.seq, timeout=5)
        if not jpg:
            _, jpg = src.wait(-1, timeout=5)
        label, score = random.choice(LABELS)
        start = start or time.time()
        ev = {
            "id": "%.6f-%06x" % (start, random.getrandbits(24)),
            "camera": cam, "label": label, "sub_label": None,
            "start_time": start, "end_time": start + random.randint(8, 40),
            "has_clip": random.random() < 0.7, "has_snapshot": True,
            "zones": [], "retain_indefinitely": False, "plus_id": None,
            "top_score": score, "false_positive": None, "box": None,
            "data": {"top_score": score, "score": score - 0.03, "type": "object"},
        }
        with self.lock:
            self.events.append((ev, jpg, thumbnail(jpg) if jpg else b""))
        return ev

    def config(self):
        cams = {}
        for name, s in self.sources.items():
            cams[name] = {"enabled": True, "name": name,
                          "detect": {"width": s.w, "height": s.h, "fps": 5},
                          "ffmpeg": {"inputs": [{"path": "rtsp://127.0.0.1:8554/%s" % name, "roles": ["detect"]}]},
                          "objects": {"track": ["person", "cat", "car"]}}
        cams["garaje"] = {"enabled": False, "name": "garaje", "detect": {"width": 640, "height": 360}}
        return {"mqtt": {"enabled": False}, "cameras": cams, "version": "0.14.1-fake",
                "record": {"enabled": True}, "snapshots": {"enabled": True}}


def make_handler(fg):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt, *a):
            sys.stderr.write("[frigate] %s\n" % (fmt % a))

        def send_bytes(self, data, ctype):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path, _, query = self.path.partition("?")
            q = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            parts = [p for p in path.split("/") if p]
            if parts[:1] != ["api"]:
                self.send_error(404)
                return
            if parts == ["api", "config"]:
                self.send_bytes(json.dumps(fg.config(), indent=1).encode(), "application/json")
                return
            if parts == ["api", "events"]:
                n = int(q.get("limit", "25"))
                with fg.lock:
                    evs = [e for e, _, _ in reversed(fg.events)][:n]
                self.send_bytes(json.dumps(evs).encode(), "application/json")
                return
            if len(parts) == 4 and parts[1] == "events" and parts[3] in ("snapshot.jpg", "thumbnail.jpg"):
                with fg.lock:
                    found = [(j, t) for e, j, t in fg.events if e["id"] == parts[2]]
                if not found:
                    self.send_error(404)
                    return
                self.send_bytes(found[0][0] if parts[3] == "snapshot.jpg" else found[0][1], "image/jpeg")
                return
            src = fg.sources.get(parts[1]) if len(parts) >= 2 else None
            if not src:
                self.send_error(404)
                return
            if len(parts) == 3 and parts[2] == "latest.jpg":
                _, jpg = src.wait(-1)
                self.send_bytes(jpg, "image/jpeg")
                return
            if len(parts) == 2:
                self.send_response(200)
                self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
                self.end_headers()
                seq = -1
                try:
                    while True:
                        seq, jpg = src.wait(seq)
                        if jpg:
                            self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"
                                             % len(jpg) + jpg + b"\r\n")
                            self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            self.send_error(404)

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--cam", action="append", default=[], help="name:WxH:fps")
    ap.add_argument("--events", type=int, default=8)
    ap.add_argument("--every", type=float, default=45.0, help="seconds between new events")
    a = ap.parse_args()
    fg = Frigate(a.cam or ["entrada:1280x720:10", "jardin:960x540:8", "living:640x360:10"], a.events)

    def more():
        while True:
            time.sleep(a.every)
            ev = fg.add_event()
            sys.stderr.write("[frigate] new event %s on %s\n" % (ev["label"], ev["camera"]))
    threading.Thread(target=more, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(fg))
    srv.daemon_threads = True
    print("http://127.0.0.1:%d  (%s)" % (a.port, ", ".join(fg.sources)))
    sys.stdout.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for s in fg.sources.values():
            s.stop()


if __name__ == "__main__":
    main()
