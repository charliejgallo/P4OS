#!/bin/bash
# install_emoji.sh <host> [emoji.pak]   the colour emoji onto the card
# The pack comes from tools/gen_emoji.py (or the release's emoji.pak) and goes
# to /fonts/emoji.pak; the firmware loads it at boot, so restart afterwards.
# The portal's token, for a board that asks for its password here
# (Settings, Portal web; docs/SECURITY.md): P4OS_TOKEN=<token> tools/...
[ -n "$P4OS_TOKEN" ] && curl() { command curl -H "Authorization: Bearer $P4OS_TOKEN" "$@"; }

set -e
cd "$(dirname "$0")/.."
host=${1:?usage: install_emoji.sh <host> [emoji.pak]}
pak=${2:-emoji.pak}
[ -f "$pak" ] || { echo "no pack at $pak (tools/gen_emoji.py)"; exit 1; }
curl -4 -s -X POST "http://$host/api/fs/mkdir?path=/fonts" >/dev/null || true
echo "uploading $pak ($(($(wc -c < "$pak") / 1048576)) MB)..."
curl -4 -s -f -X PUT --data-binary @"$pak" "http://$host/api/fs/put?path=/fonts/emoji.pak" >/dev/null
echo "done: restart the board to load it"
