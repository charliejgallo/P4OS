#!/bin/bash
# The board's whole log ring (32 KB), following the portal's X-Log-Next:
# GET /api/log returns 16 KB at a time from the oldest line.
#
#   tools/plog.sh [host]           from p4os.local, or the host given (192.168.7.1
#                                  over the cable); B=http://<host> also works
#   tools/plog.sh | grep mhop
# The portal's token, for a board that asks for its password here
# (Settings, Portal web; docs/SECURITY.md): P4OS_TOKEN=<token> tools/...
[ -n "$P4OS_TOKEN" ] && curl() { command curl -H "Authorization: Bearer $P4OS_TOKEN" "$@"; }

B=${B:-http://${1:-p4os.local}}
from=0
hdr=$(mktemp)
while :; do
    body=$(curl -4 -s -D "$hdr" "$B/api/log?from=$from")
    printf '%s' "$body"
    next=$(grep -i x-log-next "$hdr" | tr -dc 0-9)
    [ ${#body} -lt 16000 ] && break
    [ -z "$next" ] || [ "$next" = "$from" ] && break
    from=$next
done
rm -f "$hdr"
