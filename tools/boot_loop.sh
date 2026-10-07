#!/bin/bash
# Restarts the board over and over and says how each boot went.
#
#     tools/boot_loop.sh p4os.local 40            40 restarts, results in /tmp/boot_loop
#     tools/boot_loop.sh p4os.local 40 out/       somewhere else
#
# A crash while booting is easy to miss: the board restarts by itself and is
# up again a few seconds later. Only after an OTA does it show, because the
# trial image is then rolled back. This asks for a plain restart
# (POST /api/restart), waits for the portal, and reads why the board last
# started (GET /api/sysmon: "software" is the restart asked for; "panic" or a
# watchdog means the boot fell over and started again). For each bad boot it
# keeps the previous boot's log and the core dump (decode them with
# tools/coredump.sh, or esp_coredump with the ELF of that image).
#
# It is how the crash in the card's app scan was found and its fix measured
# (2026-10-06): 4 bad boots in 70 before, none in 80 after
# (docs/MEMORY.md, "Cache maintenance on PSRAM"). The board restarts each
# round, ~20 s apiece: on a board in use, ask first.
[ -n "$P4OS_TOKEN" ] && curl() { command curl -H "Authorization: Bearer $P4OS_TOKEN" "$@"; }

HOST=${1:-p4os.local}
N=${2:-20}
OUT=${3:-/tmp/boot_loop}
B="http://$HOST"
mkdir -p "$OUT"
: > "$OUT/summary.txt"

reason() {
    curl -4 -s --max-time 5 "$B/api/sysmon?hist=0" |
        python3 -c "import json,sys; print(json.load(sys.stdin).get('system',{}).get('reset_reason','?'))" 2>/dev/null || echo "?"
}

bad=0
for i in $(seq 1 "$N"); do
    t0=$(date +%s)
    curl -4 -s --max-time 5 -X POST "$B/api/restart" >/dev/null
    sleep 6
    for k in $(seq 1 90); do curl -4 -s -o /dev/null --max-time 2 "$B/api/apps" && break; sleep 1; done
    up=$(( $(date +%s) - t0 ))
    sleep 3
    r=$(reason)
    if [ "$r" != "software" ]; then
        bad=$((bad + 1))
        curl -4 -s --max-time 15 "$B/api/log?prev=1" > "$OUT/r$i-prev.txt"
        curl -4 -sf --max-time 60 -o "$OUT/r$i-core.bin" "$B/api/coredump/elf"
    fi
    echo "round $i: $r, up after ${up}s" | tee -a "$OUT/summary.txt"
    sleep 4
done
echo "$bad bad boot(s) in $N" | tee -a "$OUT/summary.txt"
