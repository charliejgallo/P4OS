#!/usr/bin/env python3
"""
A make-believe RTSP camera for P4OS's Cámaras app: H.264 over RTP,
interleaved on the RTSP connection (RTP/AVP/TCP;interleaved=0-1, the only
transport the app asks for), on 127.0.0.1 only.

    tools/fake_rtsp.py [--port 8554] [--cam timbre:704x576:12] [--cam ...]
                       [--profile baseline|main] [--gop 12] [--kbps 600]
                       [--user admin --pass secret] [--audio]

Each --cam is name:WxH:fps[:profile[:gop]], served at
rtsp://127.0.0.1:<port>/<name>. The picture is fake_mjpeg.py's (Mila walking
through a room, name, time, frame counter), encoded by ffmpeg's libx264 at
the camera's rate, with the parameter sets repeated before each keyframe,
as cameras do. One encoder per camera, shared by every client: a client
that cannot keep up loses whole pictures up to the next keyframe, like a
camera's own buffer overflowing.

What it speaks, as a Hikvision does: OPTIONS, DESCRIBE (SDP with
sprop-parameter-sets and profile-level-id), SETUP, PLAY, GET_PARAMETER (the
keep-alive), TEARDOWN; Digest authentication without qop when --user is
given; RTP single NAL units and FU-A fragments, the marker bit on the last
packet of a picture, a 90 kHz clock. --audio adds a PCMU track to the SDP
(never sent), to see the app notice it. --profile main makes a stream the
board refuses (CABAC), to see that message; the simulator plays it anyway.

The standard library and ffmpeg only.
"""
import argparse
import base64
import hashlib
import os
import queue
import random
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_mjpeg  # noqa: E402

MTU = 1400


def split_nals(buf):
    """Annex-B -> (list of complete NALs without start codes, leftover)."""
    out = []
    starts = []
    i = 0
    n = len(buf)
    while i + 3 <= n:
        if buf[i] == 0 and buf[i + 1] == 0 and buf[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        else:
            i += 1
    if len(starts) < 2:
        return out, buf
    for a, b in zip(starts, starts[1:]):
        end = b - 3
        if end > a and buf[end - 1] == 0:     # a 4-byte start code's leading zero
            end -= 1
        out.append(buf[a:end])
    rest = buf[starts[-1] - 3:]
    return out, rest


class Encoder:
    """One camera: ffmpeg -> H.264 access units, fanned out to the sessions."""

    def __init__(self, name, w, h, fps, profile, gop, kbps, index):
        self.name, self.w, self.h, self.fps = name, w, h, fps
        self.sps = self.pps = None
        self.ready = threading.Event()
        self.lock = threading.Lock()
        self.clients = []
        self.au = []
        self.frame_no = 0
        x264 = "repeat-headers=1:keyint=%d:min-keyint=%d:scenecut=0:sliced-threads=0" % (gop, gop)
        args = [fake_mjpeg.FFMPEG, "-hide_banner", "-loglevel", "error", "-re"] + \
            fake_mjpeg.scene_args(name, w, h, fps, index) + \
            ["-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
             "-profile:v", profile, "-pix_fmt", "yuv420p", "-bf", "0", "-refs", "1",
             "-b:v", "%dk" % kbps, "-maxrate", "%dk" % kbps, "-bufsize", "%dk" % (kbps * 2),
             "-x264-params", x264, "-bsf:v", "h264_metadata=aud=insert", "-f", "h264", "pipe:1"]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        out = self.proc.stdout
        while True:
            chunk = out.read1(65536)
            if not chunk:
                return
            buf += chunk
            nals, buf = split_nals(buf)
            for nal in nals:
                self._nal(nal)

    def _nal(self, nal):
        t = nal[0] & 0x1F
        if t == 9:                      # access unit delimiter: the previous picture is whole
            if self.au:
                self._publish(self.au)
            self.au = []
            return
        if t == 7:
            self.sps = nal
        elif t == 8:
            self.pps = nal
            if self.sps:
                self.ready.set()
        self.au.append(nal)

    def _publish(self, au):
        ts = int(self.frame_no * 90000 / self.fps) & 0xFFFFFFFF
        self.frame_no += 1
        key = any((n[0] & 0x1F) == 5 for n in au)
        with self.lock:
            for q in self.clients:
                q.offer(au, ts, key)

    def add(self, c):
        with self.lock:
            self.clients.append(c)

    def remove(self, c):
        with self.lock:
            if c in self.clients:
                self.clients.remove(c)

    def profile_level_id(self):
        return self.sps[1:4].hex() if self.sps else "42e01f"


class Client:
    """A session's queue: drops whole pictures up to a keyframe when it fills."""

    def __init__(self):
        self.q = queue.Queue(maxsize=60)
        self.need_key = True

    def offer(self, au, ts, key):
        if self.need_key and not key:
            return
        self.need_key = False
        try:
            self.q.put_nowait((au, ts))
        except queue.Full:
            while not self.q.empty():
                try:
                    self.q.get_nowait()
                except queue.Empty:
                    break
            self.need_key = True


def rtp_packets(au, ts, seq, ssrc, pt=96):
    pkts = []
    nals = [n for n in au if n]
    for ni, nal in enumerate(nals):
        last_nal = ni == len(nals) - 1
        if len(nal) <= MTU:
            pkts.append((nal, last_nal))
        else:
            hdr = nal[0]
            fu_ind = (hdr & 0xE0) | 28
            data = nal[1:]
            first = True
            while data:
                part, data = data[:MTU], data[MTU:]
                fu_hdr = (hdr & 0x1F) | (0x80 if first else 0) | (0x40 if not data else 0)
                pkts.append((bytes([fu_ind, fu_hdr]) + part, last_nal and not data))
                first = False
    out = []
    for payload, marker in pkts:
        b1 = (0x80 if marker else 0) | pt
        out.append(struct.pack("!BBHII", 0x80, b1, seq & 0xFFFF, ts, ssrc) + payload)
        seq += 1
    return out, seq


class Session(threading.Thread):
    def __init__(self, sock, addr, encoders, args):
        super().__init__(daemon=True)
        self.sock, self.addr, self.encoders, self.args = sock, addr, encoders, args
        self.nonce = "%032x" % random.getrandbits(128)
        self.session = "%08X" % random.getrandbits(32)
        self.enc = None
        self.client = None
        self.playing = False
        self.channel = 0
        self.wlock = threading.Lock()

    def log(self, msg):
        sys.stderr.write("[rtsp %s:%d] %s\n" % (self.addr[0], self.addr[1], msg))

    def send(self, data):
        with self.wlock:
            self.sock.sendall(data)

    def reply(self, cseq, code=200, reason="OK", headers=None, body=b""):
        h = "RTSP/1.0 %d %s\r\nCSeq: %s\r\nServer: fake_rtsp\r\n" % (code, reason, cseq)
        for k, v in (headers or {}).items():
            h += "%s: %s\r\n" % (k, v)
        if body:
            h += "Content-Length: %d\r\n" % len(body)
        self.send(h.encode() + b"\r\n" + body)

    def authorized(self, method, headers):
        if not self.args.user:
            return True
        a = headers.get("authorization", "")
        if not a.startswith("Digest "):
            return False
        f = {}
        for part in a[7:].split(","):
            if "=" in part:
                k, v = part.strip().split("=", 1)
                f[k] = v.strip('"')
        ha1 = hashlib.md5(("%s:%s:%s" % (self.args.user, "fake", self.args.password)).encode()).hexdigest()
        ha2 = hashlib.md5(("%s:%s" % (method, f.get("uri", ""))).encode()).hexdigest()
        want = hashlib.md5(("%s:%s:%s" % (ha1, self.nonce, ha2)).encode()).hexdigest()
        return f.get("response") == want and f.get("username") == self.args.user

    def encoder_for(self, url):
        path = url.split("://", 1)[-1]
        path = path[path.find("/"):] if "/" in path else "/"
        name = path.strip("/").split("/")[0]
        return self.encoders.get(name)

    def sdp(self, enc):
        sprop = "%s,%s" % (base64.b64encode(enc.sps).decode(), base64.b64encode(enc.pps).decode())
        s = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=%s\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
             "a=fmtp:96 packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s\r\n"
             "a=framerate:%g\r\na=control:trackID=1\r\n" % (enc.name, enc.profile_level_id(), sprop, enc.fps))
        if self.args.audio:
            s += "m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=control:trackID=2\r\n"
        return s.encode()

    def run(self):
        buf = b""
        try:
            self.sock.settimeout(0.5)
            while True:
                try:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                except socket.timeout:
                    pass
                while True:
                    if buf.startswith(b"$"):            # RTCP from the client, if any
                        if len(buf) < 4:
                            break
                        n = struct.unpack("!H", buf[2:4])[0]
                        if len(buf) < 4 + n:
                            break
                        buf = buf[4 + n:]
                        continue
                    e = buf.find(b"\r\n\r\n")
                    if e < 0:
                        break
                    head = buf[:e].decode(errors="replace")
                    buf = buf[e + 4:]
                    if not self.request(head):
                        return
        except (ConnectionResetError, BrokenPipeError, OSError):
            pass
        finally:
            self.stop()

    def request(self, head):
        lines = head.split("\r\n")
        try:
            method, url, _ = lines[0].split(" ", 2)
        except ValueError:
            return False
        headers = {}
        for l in lines[1:]:
            if ":" in l:
                k, v = l.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        cseq = headers.get("cseq", "0")
        self.log("%s %s" % (method, url))
        if method == "OPTIONS":
            self.reply(cseq, headers={"Public": "OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN"})
            return True
        if method != "OPTIONS" and not self.authorized(method, headers):
            self.reply(cseq, 401, "Unauthorized",
                       {"WWW-Authenticate": 'Digest realm="fake", nonce="%s"' % self.nonce})
            return True
        if method == "DESCRIBE":
            enc = self.encoder_for(url)
            if not enc:
                self.reply(cseq, 404, "Not Found")
                return True
            enc.ready.wait(10)
            self.enc = enc
            base = url.rstrip("/") + "/"
            self.reply(cseq, headers={"Content-Type": "application/sdp", "Content-Base": base},
                       body=self.sdp(enc))
            return True
        if method == "SETUP":
            if not self.enc:
                self.enc = self.encoder_for(url)
            t = headers.get("transport", "")
            if "TCP" not in t.upper():
                self.reply(cseq, 461, "Unsupported Transport")
                return True
            ch = 0
            if "interleaved=" in t:
                ch = int(t.split("interleaved=")[1].split("-")[0])
            if "trackID=2" not in url:      # the audio track is set up but never sent
                self.channel = ch
            self.reply(cseq, headers={"Transport": "RTP/AVP/TCP;unicast;interleaved=%d-%d" % (ch, ch + 1),
                                      "Session": "%s;timeout=60" % self.session})
            return True
        if method == "PLAY":
            if not self.enc:
                self.reply(cseq, 455, "Method Not Valid In This State")
                return True
            self.reply(cseq, headers={"Session": self.session, "Range": "npt=0.000-"})
            if not self.playing:
                self.playing = True
                self.client = Client()
                self.enc.add(self.client)
                threading.Thread(target=self.pump, daemon=True).start()
            return True
        if method == "GET_PARAMETER":
            self.reply(cseq, headers={"Session": self.session})
            return True
        if method == "TEARDOWN":
            self.reply(cseq, headers={"Session": self.session})
            return False
        self.reply(cseq, 501, "Not Implemented")
        return True

    def pump(self):
        seq = random.getrandbits(16)
        ssrc = random.getrandbits(32)
        sent = 0
        t0 = time.time()
        try:
            while self.playing:
                try:
                    au, ts = self.client.q.get(timeout=1)
                except queue.Empty:
                    continue
                pkts, seq = rtp_packets(au, ts, seq, ssrc)
                data = b"".join(b"$" + bytes([self.channel]) + struct.pack("!H", len(p)) + p for p in pkts)
                self.send(data)
                sent += 1
                if time.time() - t0 > 10:
                    self.log("%.1f pictures/s sent" % (sent / (time.time() - t0)))
                    sent, t0 = 0, time.time()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        self.stop()

    def stop(self):
        self.playing = False
        if self.enc and self.client:
            self.enc.remove(self.client)
            self.client = None
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--cam", action="append", default=[], help="name:WxH:fps[:profile[:gop]]")
    ap.add_argument("--profile", default="baseline", choices=["baseline", "main", "high"])
    ap.add_argument("--gop", type=int, default=0, help="frames between keyframes (default: fps)")
    ap.add_argument("--kbps", type=int, default=600)
    ap.add_argument("--user", default="")
    ap.add_argument("--pass", dest="password", default="")
    ap.add_argument("--audio", action="store_true", help="offer a PCMU track in the SDP")
    a = ap.parse_args()
    cams = a.cam or ["timbre:704x576:12"]
    encoders = {}
    for i, c in enumerate(cams):
        parts = c.split(":")
        name, w, h, fps = fake_mjpeg.parse_cam(":".join(parts[:3]))
        profile = parts[3] if len(parts) > 3 else a.profile
        gop = int(parts[4]) if len(parts) > 4 else (a.gop or max(1, int(round(fps))))
        encoders[name] = Encoder(name, w, h, fps, profile, gop, a.kbps, i + 3)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(8)
    for name, e in encoders.items():
        print("rtsp://127.0.0.1:%d/%s  (%dx%d @ %g)" % (a.port, name, e.w, e.h, e.fps))
    sys.stdout.flush()
    try:
        while True:
            s, addr = srv.accept()
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            Session(s, addr, encoders, a).start()
    except KeyboardInterrupt:
        pass
    finally:
        for e in encoders.values():
            e.proc.terminate()


if __name__ == "__main__":
    main()
