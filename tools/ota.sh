#!/bin/bash
# ota.sh [host] [profile]: the firmware to the board over Wi-Fi.
#
#   tools/ota.sh                      build rev1_3, upload to p4os.local, restart
#   tools/ota.sh 10.0.0.23            by address
#   NOBUILD=1 tools/ota.sh ...        upload what is already built
#
# The image goes into the idle slot (PUT /api/ota), the board restarts into
# it, and it boots on trial: it is confirmed after 30 s up, and if it crashes
# or hangs before that, the next restart brings the previous image back by
# itself. This script checks that the slot changed and waits for the
# confirmation. The ELF is kept by its sha (tools/elf_keep.sh) so a later
# core dump from this image can be decoded (tools/coredump.sh).
set -e
cd "$(dirname "$0")/.."
HOST=${1:-p4os.local}
PROFILE=${2:-rev1_3}
B="http://$HOST"
BIN="build/$PROFILE/p4os.bin"

# a failed build leaves the previous .bin in place: never upload that one
if [ -z "$NOBUILD" ]; then
    out=$(tools/build_fw.sh "$PROFILE" 2>&1)
    echo "$out" | grep -q "Project build complete" || { echo "$out" | grep -E "error" | tail -5; echo "the build failed: nothing uploaded"; exit 1; }
fi
[ -f "$BIN" ] || { echo "no $BIN"; exit 1; }
before=$(curl -4 -s --max-time 5 "$B/api/ota" | python3 -c "import json,sys; print(json.load(sys.stdin)['running'])")
echo "running $before; uploading $(wc -c < "$BIN" | tr -d ' ') B..."
ans=$(curl -4 -s --max-time 600 -X PUT --data-binary "@$BIN" "$B/api/ota")
echo "   $ans"
echo "$ans" | grep -q '"ok":true' || exit 1
tools/elf_keep.sh "$PROFILE"
curl -4 -s --max-time 5 -X POST "$B/api/ota/restart" >/dev/null || true
sleep 6
for i in $(seq 1 60); do curl -4 -s -o /dev/null --max-time 2 "$B/api/apps" && break; sleep 1; done
st=$(curl -4 -s --max-time 5 "$B/api/ota")
after=$(echo "$st" | python3 -c "import json,sys; print(json.load(sys.stdin)['running'])")
if [ "$after" = "$before" ]; then
    echo "still on $before: the new image did not take (rolled back?) - see GET $B/api/log?prev=1"
    exit 1
fi
echo "running $after on trial; waiting for the confirmation (30 s up)..."
for i in $(seq 1 60); do
    sleep 1
    st=$(curl -4 -s --max-time 3 "$B/api/ota" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['running'], d['trial'])" 2>/dev/null || echo "? ?")
    case "$st" in
        "$after False") echo "   confirmed: $after is the firmware now"; exit 0 ;;
        "$before "*) echo "   it crashed or hung on trial: back on $before (GET $B/api/log?prev=1, tools/coredump.sh)"; exit 1 ;;
    esac
done
echo "   not confirmed yet: look at the board (GET $B/api/log)"
exit 1
