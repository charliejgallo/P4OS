#!/usr/bin/env python3
"""
A make-believe Telegram Bot API, for the board's bot (components/aos_apps/
aos_bot.c); standard library only. Nothing here talks to Telegram.

    tools/fake_telegram_bot.py [--port 8081] [--token 123456:FAKE...] [--shots DIR]

Point the board (or the simulator) at it with the pref
    bot_api = http://127.0.0.1:8081
and load the same token in the Bot app.

The Bot API side, what aos_bot.c uses, over HTTP/1.1 with keep-alive:
getMe, getUpdates (long poll: it waits up to "timeout" seconds for an
update, honours "offset"), sendMessage (with reply_markup), sendPhoto
(multipart; the JPEG is saved in --shots), answerCallbackQuery and
setMyCommands. A wrong token answers 401 as Telegram does.

The person on the other side, from another terminal:

    curl 'localhost:8081/_say?text=/estado'                    # a message
    curl 'localhost:8081/_say?chat=222&name=Otra&text=hola'    # from another chat
    curl 'localhost:8081/_tap?data=z1:00124b0012345678:1'      # a button
    curl 'localhost:8081/_log'                                 # what the bot said

Everything the bot sends is printed, too.
"""
import argparse
import json
import os
import sys
import threading
import time
import urllib.parse
from email.parser import BytesParser
from email.policy import default as POLICY
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Condition()
UPDATES = []          # every update made, in order
SENT = []             # what the bot said
NEXT_ID = [1000]
NEXT_MSG = [1]
ARGS = None


def add_update(u):
    with LOCK:
        u["update_id"] = NEXT_ID[0]
        NEXT_ID[0] += 1
        UPDATES.append(u)
        LOCK.notify_all()


def chat_of(q):
    cid = int(q.get("chat", ["111"])[0])
    name = q.get("name", ["Prueba"])[0]
    return {"id": cid, "type": "private", "first_name": name}, {"id": cid, "is_bot": False, "first_name": name}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        pass

    def reply(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def ok(self, result):
        self.reply(200, {"ok": True, "result": result})

    # ---- the person ----
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/_say":
            chat, frm = chat_of(q)
            NEXT_MSG[0] += 1
            add_update({"message": {"message_id": NEXT_MSG[0], "from": frm, "chat": chat,
                                    "date": int(time.time()), "text": q.get("text", [""])[0]}})
            return self.reply(200, {"queued": True})
        if u.path == "/_tap":
            chat, frm = chat_of(q)
            add_update({"callback_query": {"id": str(NEXT_ID[0]) + "cb", "from": frm,
                                           "message": {"message_id": 1, "chat": chat},
                                           "data": q.get("data", [""])[0]}})
            return self.reply(200, {"queued": True})
        if u.path == "/_log":
            with LOCK:
                return self.reply(200, SENT[-50:])
        self.reply(404, {"ok": False})

    # ---- the Bot API ----
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n) if n else b""
        parts = self.path.strip("/").split("/")
        if len(parts) != 2 or not parts[0].startswith("bot"):
            return self.reply(404, {"ok": False, "error_code": 404, "description": "Not Found"})
        token, method = parts[0][3:], parts[1]
        if token != ARGS.token:
            return self.reply(401, {"ok": False, "error_code": 401, "description": "Unauthorized"})
        ctype = self.headers.get("Content-Type", "")
        if ctype.startswith("multipart/form-data"):
            msg = BytesParser(policy=POLICY).parsebytes(b"Content-Type: " + ctype.encode() + b"\r\n\r\n" + raw)
            body = {}
            for part in msg.iter_parts():
                name = part.get_param("name", header="content-disposition")
                data = part.get_payload(decode=True) or b""
                body[name] = data if part.get_filename() else data.decode("utf-8", "replace")
        else:
            body = json.loads(raw or b"{}")

        if method == "getMe":
            return self.ok({"id": 123456, "is_bot": True, "first_name": "Placa", "username": "placa_prueba_bot"})
        if method == "getUpdates":
            off = int(body.get("offset", 0) or 0)
            wait = min(int(body.get("timeout", 0) or 0), 50)
            end = time.time() + wait
            with LOCK:
                while True:
                    out = [u for u in UPDATES if u["update_id"] >= off]
                    left = end - time.time()
                    if out or left <= 0:
                        break
                    LOCK.wait(left)
            return self.ok(out)
        if method in ("sendMessage", "sendPhoto"):
            entry = {"method": method, "chat_id": body.get("chat_id"),
                     "text": body.get("text", body.get("caption", ""))}
            if "reply_markup" in body:
                entry["buttons"] = [[b["text"] + " -> " + b["callback_data"] for b in r]
                                    for r in body["reply_markup"].get("inline_keyboard", [])]
            if method == "sendPhoto":
                jpg = body.get("photo", b"")
                os.makedirs(ARGS.shots, exist_ok=True)
                path = os.path.join(ARGS.shots, "shot-%d.jpg" % int(time.time() * 1000))
                with open(path, "wb") as f:
                    f.write(jpg)
                entry["photo"] = "%s (%d bytes)" % (path, len(jpg))
            with LOCK:
                SENT.append(entry)
            print("bot ->", json.dumps(entry, ensure_ascii=False), flush=True)
            NEXT_MSG[0] += 1
            return self.ok({"message_id": NEXT_MSG[0], "date": int(time.time()),
                            "chat": {"id": int(body.get("chat_id", 0)), "type": "private"}})
        if method == "answerCallbackQuery":
            print("bot -> answer:", body.get("text", ""), flush=True)
            with LOCK:
                SENT.append({"method": method, "text": body.get("text", "")})
            return self.ok(True)
        if method == "setMyCommands":
            print("bot -> commands:", [c["command"] for c in body.get("commands", [])], flush=True)
            return self.ok(True)
        return self.reply(400, {"ok": False, "error_code": 400, "description": "method not faked: " + method})


def main():
    global ARGS
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=8081)
    p.add_argument("--bind", default="127.0.0.1")
    p.add_argument("--token", default="123456:FAKE-token-for-the-simulator_0123")
    p.add_argument("--shots", default=os.path.join(os.getcwd(), "fake_bot_shots"))
    ARGS = p.parse_args()
    srv = ThreadingHTTPServer((ARGS.bind, ARGS.port), H)
    srv.daemon_threads = True
    print("fake Bot API on http://%s:%d, token %s" % (ARGS.bind, ARGS.port, ARGS.token), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
