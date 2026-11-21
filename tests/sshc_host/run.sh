#!/bin/bash
# The firmware's SSH and SFTP client (components/aos_ssh/aos_sshc.c) on the
# Mac, against the SFTP server of the simulator: nothing here leaves this
# computer. The key exchange with each cipher, the server's key kept and
# refused when it is another, the board's key let in by a server that asks
# for one, and files up byte for byte.
#
#   SIM=sim/build/p4os_sim tests/sshc_host/run.sh
#                                   (brew: mbedtls; the simulator, built)
# The simulator opens its window while this runs, and is stopped at the end.
set -e
FROM=$PWD
cd "$(dirname "$0")"
P4=$(cd ../.. && pwd)
SIM=${SIM:-$P4/sim/build/p4os_sim}
case "$SIM" in /*) ;; *) SIM="$FROM/$SIM" ;; esac
[ -x "$SIM" ] || { echo "no simulator at $SIM: build it, or say where it is with SIM="; exit 2; }
MB=${MBEDTLS:-/opt/homebrew/opt/mbedtls}
A=$P4/components/aos_ssh
M=$P4/components/monocypher
out=$(mktemp -d)
pids=""
trap 'rc=$?; for p in $pids; do kill $p 2>/dev/null || true; done; for p in $pids; do wait $p 2>/dev/null || true; done; rm -rf "$out"; exit $rc' EXIT

cc -O1 -g -Wall -Wextra -Werror -DAOS_SIM \
   -I"$P4/components/aos_hal/include" -I"$P4/components/aos_hal" -I"$A/include" -I"$A" -I"$M/include" -I"$MB/include" \
   -o "$out/t" main.c fakes.c "$A/aos_sshc.c" "$A/aos_ssh_crypto.c" "$A/aos_ssh_wire.c" "$A/aos_ssh_chacha.c" \
   "$M/monocypher.c" "$M/monocypher-ed25519.c" -L"$MB/lib" -lmbedcrypto -lpthread

cd "$out"
P1=$((20000 + RANDOM % 20000)); P2=$((P1 + 1)); P3=$((P1 + 2)); P4P=$((P1 + 3))
SEED=$(python3 -c 'import os; print(os.urandom(32).hex())')
OTHER=$(python3 -c 'import os; print(os.urandom(32).hex())')
LINE=$(./t - 0 - "$SEED" - key)

# two boards: one that lets anybody on its network in, one that asks, with
# the key on its list
for b in a b; do mkdir -p $b/sim_fs/apps; done
printf 'wall=0\nssh_on=1\n' > a/prefs.txt
printf 'wall=0\nssh_on=1\npt_ask=1\npt_pass=x\nssh_k0=%s\n' "$LINE" > b/prefs.txt
cd a
P4_SIM_SSH_PORT=$P1 P4_SIM_PORTAL_PORT=$P2 P4_SIM_PREFS=$PWD/prefs.txt "$SIM" > sim.log 2>&1 < /dev/null &
pids="$!"
cd ../b
P4_SIM_ZONE=home P4_SIM_SSH_PORT=$P3 P4_SIM_PORTAL_PORT=$P4P P4_SIM_PREFS=$PWD/prefs.txt "$SIM" > sim.log 2>&1 < /dev/null &
pids="$pids $!"
cd ..
for i in $(seq 1 40); do nc -z 127.0.0.1 $P1 2>/dev/null && nc -z 127.0.0.1 $P3 2>/dev/null && break; sleep 0.25; done

head -c 300000 /dev/urandom > grande.bin
echo "un respaldo" > chico.p4bak
: > vacio.txt

P1=$P1 P3=$P3 SEED=$SEED OTHER=$OTHER python3 - <<'PY'
import os, subprocess, sys

fails = 0
def check(what, ok, detail=""):
    global fails
    print("%-66s %s" % (what, "ok" if ok else "FAILED"))
    if not ok:
        fails += 1
        print("     " + str(detail)[-700:])

P1, P3, SEED, OTHER = os.environ["P1"], os.environ["P3"], os.environ["SEED"], os.environ["OTHER"]
def run(port, seed, kept, *steps, user="p4os"):
    p = subprocess.run(["./t", "127.0.0.1", port, user, seed, kept] + list(steps), capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout + p.stderr

rc, o = run(P1, SEED, "-")
host = [l.split(" ") for l in o.splitlines() if l.startswith("host ")]
check("connected, keys exchanged, in: the server's key and its fingerprint", rc == 0 and host and host[0][1] == "ED25519"
      and host[0][2].startswith("SHA256:") and len(host[0][2]) == 50 and "\nok\n" in o, o)
kept = host[0][3] if host else "-"

rc, o = run(P1, SEED, kept, "mkdirs=respaldos/placa/2026", "put=chico.p4bak:respaldos/placa/2026/chico.p4bak",
            "put=grande.bin:/respaldos/placa/grande.bin", "put=vacio.txt:respaldos/vacio.txt", "stat=respaldos/placa/grande.bin",
            "stat=respaldos/nada", "list=respaldos/placa")
there = lambda p: os.path.exists("a/sim_fs/" + p)
check("with its key kept: folders made, three files up", rc == 0 and there("respaldos/placa/2026/chico.p4bak") and there("respaldos/vacio.txt"), o)
check("...byte for byte, the 300 KB one too, and none left half named", there("respaldos/placa/grande.bin")
      and open("a/sim_fs/respaldos/placa/grande.bin", "rb").read() == open("grande.bin", "rb").read()
      and open("a/sim_fs/respaldos/placa/2026/chico.p4bak").read() == "un respaldo\n"
      and os.path.getsize("a/sim_fs/respaldos/vacio.txt") == 0
      and not [f for d, _, fs in os.walk("a/sim_fs/respaldos") for f in fs if f.endswith(".part")], o)
check("...its size asked for, one that is not there, and the folder listed", "stat respaldos/placa/grande.bin 1 300000" in o
      and "stat respaldos/nada 0 0" in o and "name d 2026 0" in o and "name f grande.bin 300000" in o, o)

open("chico.p4bak", "w").write("el de hoy\n")
rc, o = run(P1, SEED, kept, "put=chico.p4bak:respaldos/placa/2026/chico.p4bak", "rm=respaldos/vacio.txt", "rm=respaldos/vacio.txt")
check("a file sent again takes the place of the one there; one removed, twice", rc == 0
      and open("a/sim_fs/respaldos/placa/2026/chico.p4bak").read() == "el de hoy\n" and not there("respaldos/vacio.txt"), o)

rc, o = run(P1, SEED, kept, "put=no-hay.bin:respaldos/x.bin")
check("a local file that is not there: said, nothing left on the server", rc == 1 and "kind=7" in o and not there("respaldos/x.bin")
      and not there("respaldos/x.bin.part"), o)
rc, o = run(P1, SEED, kept, "put=chico.p4bak:no/existe/x.bin")
check("into a folder the server does not have: the server's no", rc == 1 and "kind=6" in o, o)

# another server where the kept one was: its key is another
rc, o = run(P3, SEED, kept, "put=chico.p4bak:nunca.p4bak")
check("another server in its place: refused by its key, nothing sent", rc == 1 and "kind=3" in o and "seen " in o
      and not os.path.exists("b/sim_fs/nunca.p4bak"), o)

# a server that asks: the key on its list is let in, with no password
rc, o = run(P3, SEED, "-", "put=chico.p4bak:con-clave.p4bak")
log = open("b/sim.log").read()
check("a server that asks who it is: the board's key, on its list, is let in", rc == 0 and os.path.exists("b/sim_fs/con-clave.p4bak"), o + log[-400:])

rc, o = run("1", SEED, "-")
check("nobody at that port: said as such", rc == 1 and "kind=1" in o, o)
sys.exit(1 if fails else 0)
PY
echo "all passed"
