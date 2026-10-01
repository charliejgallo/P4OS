#!/usr/bin/env python3
"""
A stand-in for Anthropic's side of the Claude app (aos_claude.c), standard
library only, plain http on 127.0.0.1:

    tools/fake_claude_api.py [--port 8765] [--expires 60]

  GET  /oauth/authorize?...     the login page: "signs in" at once and shows the
                                code to paste, "code#state", like Claude's page
  POST /v1/oauth/token          authorization_code (checks the PKCE verifier
                                against the challenge, and the state) and
                                refresh_token (rotates: the old refresh token
                                stops working, as the real one does)
  GET  /api/oauth/usage         needs a live Bearer token and the anthropic-beta
                                header; the 5 h window climbs while "working"

Point the simulator at it through the preferences (sim/sim_fs/prefs.txt):

    claude_auth=http://127.0.0.1:8765/oauth/authorize
    claude_token=http://127.0.0.1:8765/v1/oauth/token
    claude_api=http://127.0.0.1:8765

The response shape follows what Claude Code's /usage reads: five_hour and
seven_day with utilization (percent) and resets_at (ISO 8601), the per-model
weekly windows (null when the plan has none) and extra_usage.
"""
import argparse
import base64
import hashlib
import json
import random
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

LOCK = threading.Lock()
CODES = {}          # code -> (challenge, state, redirect)
ACCESS = {}         # token -> expiry
REFRESH = set()
START = time.time()
P5 = [31.0]


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%06d+00:00" % int((t % 1) * 1e6)


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        print("%s %s" % (self.command, self.path.split("?")[0]), flush=True)

    def send(self, code, obj, ctype="application/json"):
        body = obj.encode() if isinstance(obj, str) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/oauth/authorize":
            if q.get("code_challenge_method") != "S256" or not q.get("code_challenge") or not q.get("state"):
                return self.send(400, "<p>missing PKCE</p>", "text/html")
            code = secrets.token_urlsafe(24)
            with LOCK:
                CODES[code] = (q["code_challenge"], q["state"], q.get("redirect_uri", ""))
            print("  login for client %s, scope %r" % (q.get("client_id"), q.get("scope")), flush=True)
            return self.send(200, "<html><body><p>Paste this code into P4OS:</p><pre id=code>%s#%s</pre></body></html>"
                             % (code, q["state"]), "text/html")
        if u.path == "/api/oauth/usage":
            auth = self.headers.get("Authorization", "")
            tok = auth[7:] if auth.startswith("Bearer ") else ""
            with LOCK:
                exp = ACCESS.get(tok)
            if not exp or exp < time.time():
                return self.send(401, {"type": "error", "error": {"type": "authentication_error",
                                                                  "message": "OAuth token has expired."}})
            if "oauth-" not in self.headers.get("anthropic-beta", ""):
                return self.send(401, {"type": "error", "error": {"type": "authentication_error",
                                                                  "message": "OAuth authentication is currently not supported."}})
            now = time.time()
            with LOCK:
                P5[0] = min(100.0, P5[0] + random.uniform(0, 0.8))
                p5 = P5[0]
            return self.send(200, {
                "five_hour": {"utilization": round(p5, 1), "resets_at": iso(now + 2 * 3600 + 13 * 60)},
                "seven_day": {"utilization": round(38 + p5 / 20, 1), "resets_at": iso(now + 3 * 86400 + 4 * 3600)},
                "seven_day_opus": {"utilization": 12.0, "resets_at": iso(now + 3 * 86400 + 4 * 3600)},
                "seven_day_sonnet": None,
                "extra_usage": {"is_enabled": False, "monthly_limit": None, "used_credits": None, "utilization": None},
            })
        self.send(404, {"error": "not found"})

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length", "0"))
        try:
            b = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self.send(400, {"error": "invalid_request", "error_description": "body is not JSON"})
        if u.path != "/v1/oauth/token":
            return self.send(404, {"error": "not found"})
        g = b.get("grant_type")
        if g == "authorization_code":
            with LOCK:
                c = CODES.pop(b.get("code", ""), None)
            if not c:
                return self.send(400, {"error": "invalid_grant", "error_description": "Invalid 'code' in request."})
            challenge, state, redirect = c
            v = b.get("code_verifier", "")
            calc = base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).rstrip(b"=").decode()
            if calc != challenge:
                return self.send(400, {"error": "invalid_grant", "error_description": "PKCE verification failed."})
            if b.get("state") != state or b.get("redirect_uri") != redirect:
                return self.send(400, {"error": "invalid_grant", "error_description": "state or redirect_uri mismatch."})
        elif g == "refresh_token":
            with LOCK:
                ok = b.get("refresh_token") in REFRESH
                REFRESH.discard(b.get("refresh_token"))
            if not ok:
                return self.send(400, {"error": "invalid_grant", "error_description": "Refresh token not found or invalid."})
            print("  refreshed", flush=True)
        else:
            return self.send(400, {"error": "unsupported_grant_type"})
        at, rt = "sk-ant-oat01-fake-" + secrets.token_urlsafe(40), "sk-ant-ort01-fake-" + secrets.token_urlsafe(40)
        with LOCK:
            ACCESS[at] = time.time() + ARGS.expires
            REFRESH.add(rt)
        self.send(200, {"token_type": "Bearer", "access_token": at, "refresh_token": rt,
                        "expires_in": ARGS.expires, "scope": "user:profile user:inference"})


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--expires", type=int, default=3600, help="seconds an access token lives")
    ARGS = ap.parse_args()
    print("fake Claude OAuth + usage on http://127.0.0.1:%d" % ARGS.port, flush=True)
    ThreadingHTTPServer(("127.0.0.1", ARGS.port), H).serve_forever()
