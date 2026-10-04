#!/bin/zsh
#
# Uploads language packs to the board over wifi, without taking the card out.
#
#   ./tools/install_lang.sh p4os.local en de
#   ./tools/install_lang.sh p4os.local en
#   ./tools/install_lang.sh localhost:8080 en     the simulator's portal
#   ./tools/install_lang.sh sim en de xx          straight into sim/sim_fs/lang
#
# The packs come from lang/<code>/ at the root of the repo, which is where
# tools/gen_lang.py writes them and where they are tracked in git (sim_fs is
# not). The pseudo-locale xx is generated, not tracked: make it with
# `python3 tools/gen_lang.py pseudo xx` first if you want it.
#
# It uses the P4 portal's file API (components/aos_portal/aos_portal.c):
#   POST /api/fs/mkdir?path=/lang/<code>        409 when it already exists: fine
#   PUT  /api/fs/put?path=/lang/<code>/<file>   the body is the file
# The portal writes to <file>.part and renames at the end, so a cut upload
# leaves the old catalog whole.
#
# After uploading, choose the language in Settings; this only puts the files
# there. The pack is read at startup and on changing language, so overwriting
# the active one has no effect until it is chosen again (or the board
# restarts). The portal can also choose it:
#   curl -X POST http://<host>/api/settings -d '{"lang":"en"}'
# The portal's token, for a board that asks for its password here
# (Settings, Portal web; docs/SECURITY.md): P4OS_TOKEN=<token> tools/...
[ -n "$P4OS_TOKEN" ] && curl() { command curl -H "Authorization: Bearer $P4OS_TOKEN" "$@"; }

set -e
ROOT=${0:a:h:h}
HOST=${1:?usage: install_lang.sh <ip-or-name[:port]|sim> <code> [code...]}
shift
CODES=("$@")
(( ${#CODES[@]} )) || { echo "the language code is missing (en, de, ...)"; exit 1; }

for code in $CODES; do
    DIR=$ROOT/lang/$code
    [[ -d $DIR ]] || { echo "$DIR does not exist - generate the pack with gen_lang.py"; exit 1; }
    files=($DIR/*(.N))
    (( ${#files[@]} )) || { echo "$DIR is empty"; exit 1; }

    if [[ $HOST == sim ]]; then
        # The simulator reads sim_fs/lang relative to sim/, where it runs.
        DEST=$ROOT/sim/sim_fs/lang/$code
        mkdir -p $DEST
        cp $files $DEST/
        echo "== $code -> $DEST: ${#files[@]} files"
        continue
    fi

    echo "== $code -> $HOST =="
    for d in /lang /lang/$code; do
        c=$(curl -4 -sS -o /dev/null -w '%{http_code}' --max-time 10 \
            -X POST "http://$HOST/api/fs/mkdir?path=$d") || c=000
        [[ $c == 2* || $c == 409 ]] || { echo "   mkdir $d FAILED (HTTP $c)"; exit 1; }
    done
    total=0
    for f in $files; do
        name=${f:t}
        size=$(wc -c < $f | tr -d ' ')
        code_http=$(curl -4 -sS -o /dev/null -w '%{http_code}' \
            --max-time 60 \
            -X PUT "http://$HOST/api/fs/put?path=/lang/$code/$name" \
            -H 'Content-Type: application/octet-stream' \
            --data-binary "@$f") || code_http=000
        if [[ $code_http == 2* ]]; then
            printf "   %-24s %7s B  ok\n" $name $size
            (( total += size ))
        else
            printf "   %-24s %7s B  FAILED (HTTP %s)\n" $name $size $code_http
            exit 1
        fi
    done
    echo "   ${#files[@]} files, $total bytes"
    listed=$(curl -4 -sS --max-time 10 "http://$HOST/api/fs?path=/lang/$code" | grep -o '\.lang"' | wc -l | tr -d ' ')
    echo "   checking: the card lists $listed .lang files in /lang/$code"
done
echo
echo "done. Choose the language in Settings > Language."
