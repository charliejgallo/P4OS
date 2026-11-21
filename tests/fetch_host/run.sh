#!/bin/bash
# The Tienda's downloader (components/aos_hal/aos_fetch.c) on the Mac, against
# tools/fake_github.py: the two redirects of a release download, a body cut
# in the middle and picked up with Range, a signed address that stopped
# working, a server without ranges, one that sends chunks, a wrong hash, a
# wrong size, a 404, a 503 that passes, a loop of redirects, and over TLS the
# redirect down to http that must be refused. Nothing leaves this machine.
#
#   tests/fetch_host/run.sh        (brew: mbedtls)
set -e
cd "$(dirname "$0")"
P4=$(cd ../.. && pwd)
MB=${MBEDTLS:-/opt/homebrew/opt/mbedtls}
out=$(mktemp -d)
PORT=${PORT:-18190}
TPORT=$((PORT + 10))

cc -O1 -g -Wall -Wextra -Werror -DAOS_SIM -I"$P4/components/aos_hal/include" -I"$P4/components/aos_hal" \
   -I"$MB/include" -o "$out/t" main.c fakes.c \
   "$P4/components/aos_hal/aos_fetch.c" "$P4/components/aos_hal/aos_tls.c" "$P4/components/aos_hal/aos_crypto.c" \
   -L"$MB/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lpthread

mkdir -p "$out/rel" "$out/card"
python3 - "$out/rel" <<'PY'
import os, random, sys
random.seed(11)
d = sys.argv[1]
open(os.path.join(d, "big.pak"), "wb").write(random.randbytes(3 * 1024 * 1024 + 4321))
open(os.path.join(d, "small.so"), "wb").write(random.randbytes(3000))
open(os.path.join(d, "empty.txt"), "wb").write(b"")
PY

# a throwaway authority and a certificate for localhost, for the https part
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj "/CN=fake github test CA" \
    -keyout "$out/ca.key" -out "$out/ca.pem" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "/CN=localhost" \
    -keyout "$out/srv.key" -out "$out/srv.csr" 2>/dev/null
printf "subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n" > "$out/ext"
openssl x509 -req -in "$out/srv.csr" -CA "$out/ca.pem" -CAkey "$out/ca.key" -CAcreateserial -days 2 \
    -extfile "$out/ext" -out "$out/srv.pem" 2>/dev/null

python3 "$P4/tools/fake_github.py" --dir "$out/rel" --port $PORT --quiet > "$out/fake.log" 2>&1 &
FAKE=$!
python3 "$P4/tools/fake_github.py" --dir "$out/rel" --port $TPORT --quiet --tls "$out/srv.pem" "$out/srv.key" \
    > "$out/fake-tls.log" 2>&1 &
FAKE_TLS=$!
trap 'rc=$?; kill $FAKE $FAKE_TLS 2>/dev/null || true; wait $FAKE $FAKE_TLS 2>/dev/null || true; rm -rf "$out"; exit $rc' EXIT
for i in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/_fake" && break; sleep 0.1; done

export P4_SIM_EXTRA_CA="$out/ca.pem"
cd "$out"
python3 - $PORT $TPORT <<'PY'
import hashlib, json, os, subprocess, sys, urllib.request
port, tport = int(sys.argv[1]), int(sys.argv[2])
G = "http://localhost:%d" % port
T = "https://localhost:%d" % tport
LATEST = "/fake/p4os/releases/latest/download/"
big = open("rel/big.pak", "rb").read()
small = open("rel/small.so", "rb").read()
sha = lambda b: hashlib.sha256(b).hexdigest()
fails = 0

def fake(q, base=G):
    urllib.request.urlopen(urllib.request.Request(base.replace("https", "http") + "/_fake?reset=1&" + q, method="POST")).read()

def served():
    return json.loads(urllib.request.urlopen(G + "/_fake").read())["bytes"]

def fake_tls(q):
    import ssl
    ctx = ssl.create_default_context(cafile="ca.pem")
    urllib.request.urlopen(urllib.request.Request(T + "/_fake?reset=1&" + q, method="POST"), context=ctx).read()

def get(url, name, *args, env=None):
    path = os.path.join("card", name)
    e = dict(os.environ)
    e.update(env or {})
    p = subprocess.run(["./t", url, path] + [str(a) for a in args], capture_output=True, text=True, env=e, timeout=120)
    try:
        r = json.loads(p.stdout.strip().splitlines()[-1])
    except Exception:
        r = {"state": "crashed", "why": p.stdout + p.stderr}
    r["file"] = open(path, "rb").read() if os.path.exists(path) else None
    r["part"] = os.path.getsize(path + ".part") if os.path.exists(path + ".part") else None
    return r

def check(what, ok, r):
    global fails
    print("%-58s %s" % (what, "ok" if ok else "FAILED"))
    if not ok:
        fails += 1
        print("    ", {k: v for k, v in r.items() if k != "file"})

def clean(name):
    for s in ("", ".part"):
        if os.path.exists("card/" + name + s): os.remove("card/" + name + s)

# 1. the way it should go: two redirects, another host, the file whole
fake("")
r = get(G + LATEST + "big.pak", "a.pak", sha(big), len(big))
check("two redirects, the whole file, its hash", r["state"] == "done" and r["file"] == big and r["connects"] == 3
      and r["redirects"] == 2 and r["retries"] == 0 and r["part"] is None, r)

# 2. cut twice in the middle: the rest by Range, from the first address again
fake("cut=700000&cuts=2&oneshot=1")
r = get(G + LATEST + "big.pak", "b.pak", sha(big), len(big))
check("cut twice, signed addresses that work once", r["state"] == "done" and r["file"] == big and r["retries"] == 2
      and r["connects"] == 9, r)

# 3. cancelled, then started again: it goes on from the .part
fake("kbps=4000")
r = get(G + LATEST + "big.pak", "c.pak", sha(big), len(big), 900000)
check("cancelled: the .part stays, no file", r["state"] == "cancelled" and r["file"] is None and (r["part"] or 0) >= 900000, r)
left = r["part"]
fake("")
r = get(G + LATEST + "big.pak", "c.pak", sha(big), len(big))
r["served"] = served()
check("started again: only the rest travels", r["state"] == "done" and r["file"] == big and r["resumed"] == left
      and r["served"] == len(big) - left, r)

# 4. a server that does not do ranges: the whole file over what was there
open("card/d.pak.part", "wb").write(big[:123456])
fake("norange=1")
r = get(G + LATEST + "big.pak", "d.pak", sha(big), len(big))
check("no ranges: the whole file again", r["state"] == "done" and r["file"] == big and r["status"] == 200, r)

# 5. chunks nobody asked for
fake("chunked=1")
r = get(G + LATEST + "big.pak", "e.pak", sha(big), len(big))
check("a body in chunks", r["state"] == "done" and r["file"] == big, r)

# 6. a .part of something else, longer than the file
open("card/f.so.part", "wb").write(b"x" * 5000)
fake("")
r = get(G + LATEST + "small.so", "f.so", sha(small))
check("a .part longer than the file: started again", r["state"] == "done" and r["file"] == small, r)

# 7. a .part of something else, shorter: only the hash can tell
open("card/g.so.part", "wb").write(b"x" * 1000)
r = get(G + LATEST + "small.so", "g.so", sha(small), len(small))
check("a .part of another file: the hash refuses it", r["state"] == "failed" and r["status"] == -21 and r["file"] is None
      and r["part"] is None, r)
r = get(G + LATEST + "small.so", "g.so", sha(small), len(small))
check("...and the next try is clean", r["state"] == "done" and r["file"] == small, r)

# 8. not the size it should be: refused before a byte is kept
r = get(G + LATEST + "small.so", "h.so", "-", 2999)
check("another size than the catalogue's", r["state"] == "failed" and r["status"] == -22 and r["file"] is None, r)

# 9. what is not there
r = get(G + LATEST + "nothing.so", "i.so")
check("a 404 fails at once", r["state"] == "failed" and r["status"] == 404 and r["retries"] == 0 and r["ms"] < 2000, r)

# 10. a server having a bad moment
fake("fail=503&fails=2")
r = get(G + LATEST + "small.so", "j.so", sha(small))
check("503 twice, then the file", r["state"] == "done" and r["file"] == small and r["retries"] == 2, r)

# 11. redirects in a circle
fake("loop=1")
r = get(G + LATEST + "small.so", "k.so")
check("a loop of redirects", r["state"] == "failed" and r["status"] == -23, r)

# 12. an empty file is a file
fake("")
r = get(G + LATEST + "empty.txt", "l.txt", sha(b""), 0)
check("an empty file", r["state"] == "done" and r["file"] == b"", r)

# 13. no hash, no size: whatever comes
r = get(G + LATEST + "small.so", "m.so")
check("with nothing to check against", r["state"] == "done" and r["file"] == small, r)

# 13b. into folders the card does not have yet
r = get(G + LATEST + "small.so", "apps/emulador/nes.so", sha(small))
check("folders made on the way", r["state"] == "done" and r["file"] == small, r)

# 14. the same over TLS, with the test's own authority
fake_tls("")
r = get(T + LATEST + "big.pak", "n.pak", sha(big), len(big))
check("https all the way", r["state"] == "done" and r["file"] == big and r["connects"] == 3, r)

# 15. https that redirects down to http
fake_tls("down=1")
r = get(T + LATEST + "small.so", "o.so")
check("from https down to http: refused", r["state"] == "failed" and r["status"] == -23 and r["file"] is None, r)

# 16. an authority nobody trusts
fake_tls("")
r = get(T + LATEST + "small.so", "p.so", env={"P4_SIM_EXTRA_CA": ""})
check("a certificate from nobody: no retries", r["state"] == "failed" and r["status"] == -8 and r["retries"] == 0, r)

# 17. the board before it knows the time
r = get(T + LATEST + "small.so", "q.so", env={"FAKE_NO_CLOCK": "1"})
check("no clock yet: tried again, then given up", r["state"] == "failed" and r["status"] == -24 and r["retries"] == 4, r)
print("     ", r["why"])

sys.exit(1 if fails else 0)
PY
echo "all passed"
