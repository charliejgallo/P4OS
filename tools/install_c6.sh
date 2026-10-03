#!/bin/bash
# install_c6.sh <host> [image]     the C6's firmware onto the card, then into the C6
# Uploads the image (default c6/build/p4os_c6.bin, from tools/build_c6.sh)
# to the card's /firmware/c6.bin and starts the update; the board restarts
# by itself when the C6 has it. Settings > Update does the same from the
# card. docs/C6.md says what can go wrong and how to recover.
set -e
cd "$(dirname "$0")/.."
host=${1:?usage: install_c6.sh <host> [image]}
img=${2:-c6/build/p4os_c6.bin}
[ -f "$img" ] || { echo "no image at $img (tools/build_c6.sh)"; exit 1; }
curl -4 -s -X POST "http://$host/api/fs/mkdir?path=/firmware" >/dev/null || true
echo "uploading $img ($(wc -c < "$img" | tr -d ' ') B)..."
curl -4 -s -f -X PUT --data-binary @"$img" "http://$host/api/fs/put?path=/firmware/c6.bin" >/dev/null
echo "   $(curl -4 -s "http://$host/api/c6")"
echo "starting the update..."
curl -4 -s -f -X POST "http://$host/api/c6/update" >/dev/null
while :; do
  sleep 2
  s=$(curl -4 -s -m 3 "http://$host/api/c6") || { echo "   the board is restarting"; break; }
  echo "   $s"
  case "$s" in *'"failed"'*) exit 1 ;; *'"done"'*) ;; esac
done
for i in $(seq 1 60); do
  sleep 2
  s=$(curl -4 -s -m 3 "http://$host/api/c6") && { echo "back: $s"; exit 0; }
done
echo "the board did not come back on the network in 2 minutes: docs/C6.md, Recovery"
exit 1
