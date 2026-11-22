#!/usr/bin/env python3
"""A make-believe internet radio for the simulator: one station that plays a
file over and over, at the pace it would be heard, the way Icecast and
Shoutcast do, so aos_radio.c is tried without tuning a real one.

    tools/fake_radio.py --file tone.aac [--port 18400] [--strict11] [--chunked]

    /stream     the audio, for ever; with "Icy-MetaData: 1" in the request it
                says icy-metaint and puts a title in every so often
    /stream.pls a playlist whose one line is /stream

--strict11 answers 426 to a request in HTTP/1.0, as some servers do (adsb.lol
does to the board's HTTP client). --chunked sends the audio in chunks to an
HTTP/1.1 request. The file is any ADTS AAC or MP3; nothing here makes one:

    python3 - <<'PY'      # five seconds of a tone, as a WAV
    import math, struct, wave
    w = wave.open('tone.wav', 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(b''.join(struct.pack('<h', int(9000 * math.sin(2 * math.pi * 440 * i / 44100))) for i in range(5 * 44100)))
    PY
    afconvert -f adts -d aac -b 64000 tone.wav tone.aac      # macOS

It logs every request with the HTTP version it came in.
"""
import argparse
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

METAINT = 8192


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    audio = b""
    ctype = "audio/aac"
    rate = 8000             # bytes a second
    strict11 = False
    chunked = False

    def log_message(self, fmt, *args):
        pass

    def out(self, data, chunked):
        if chunked:
            self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
        else:
            self.wfile.write(data)

    def do_GET(self):
        icy = self.headers.get("Icy-MetaData") == "1"
        print("%s %s %s%s" % (time.strftime("%H:%M:%S"), self.path, self.request_version, " icy" if icy else ""), flush=True)
        if self.strict11 and self.request_version == "HTTP/1.0":
            body = b"Upgrade Required"
            self.send_response(426)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/stream.pls":
            host = self.headers.get("Host", "127.0.0.1")
            body = ("[playlist]\nNumberOfEntries=1\nFile1=http://%s/stream\n" % host).encode()
            self.send_response(200)
            self.send_header("Content-Type", "audio/x-scpls")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path != "/stream":
            self.send_error(404)
            return
        chunked = self.chunked and self.request_version != "HTTP/1.0"
        self.send_response(200)
        self.send_header("Content-Type", self.ctype)
        self.send_header("icy-name", "Radio de prueba")
        if icy:
            self.send_header("icy-metaint", str(METAINT))
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        at, since_meta, n = 0, 0, 0
        try:
            while True:
                step = min(2048, METAINT - since_meta) if icy else 2048
                piece = (self.audio * (step // len(self.audio) + 2))[at % len(self.audio):][:step]
                at += step
                since_meta += step
                self.out(piece, chunked)
                if icy and since_meta >= METAINT:
                    n += 1
                    title = ("StreamTitle='Prueba - Tema %d';" % (n // 8 + 1)).encode()
                    blocks = (len(title) + 15) // 16
                    self.out(bytes([blocks]) + title.ljust(blocks * 16, b"\0"), chunked)
                    since_meta = 0
                self.wfile.flush()
                time.sleep(step / (self.rate * 1.5))     # a little faster than it is heard, as real ones
        except (BrokenPipeError, ConnectionResetError):
            print("%s the listener left" % time.strftime("%H:%M:%S"), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", required=True, help="the audio it plays over and over: ADTS AAC or MP3")
    ap.add_argument("--port", type=int, default=18400)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--kbps", type=int, default=64, help="the pace it is sent at")
    ap.add_argument("--strict11", action="store_true", help="426 to a request in HTTP/1.0")
    ap.add_argument("--chunked", action="store_true", help="the audio in chunks, to HTTP/1.1")
    a = ap.parse_args()
    Handler.audio = open(a.file, "rb").read()
    if not Handler.audio:
        sys.exit("an empty file")
    Handler.ctype = "audio/mpeg" if a.file.lower().endswith(".mp3") else "audio/aac"
    Handler.rate = a.kbps * 125
    Handler.strict11, Handler.chunked = a.strict11, a.chunked
    print("http://%s:%d/stream   (%s, %d bytes, %d kbps)" % (a.bind, a.port, a.file, len(Handler.audio), a.kbps), flush=True)
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
