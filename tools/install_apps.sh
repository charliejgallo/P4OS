#!/bin/zsh
#
# Uploads the built .so files, and each app's pack, to the board over Wi-Fi,
# without taking the card out, and restarts it so it registers them.
#
#   ./tools/install_apps.sh p4os.local            all of them
#   ./tools/install_apps.sh p4os.local turbo mila just these
#   NORESTART=1 ./tools/install_apps.sh ...       upload only
#
# build_apps.sh BUILDS but does not install: it leaves the .so files in
# apps/<x>/build/. Forget this step and the board goes on running the old
# binaries, and the symptom misleads: the firmware looks updated and the apps
# do not.
#
# What goes to /apps on the card:
#   apps/<x>/build/<x>.so
#   apps/<x>/build/<x>_p4.pak and its parts (.pak.1, .pak.2...) when the app
#   has a pack (monsterhop, turbo, mila, golf). The game reads parts until
#   one is missing, so the parts on the card that this build does not have
#   are deleted first: a stale .pak.2 would be read as the end of the pack.
#
# It speaks the P4 portal: PUT /api/fs/put?path= (the body is the file,
# streamed, any size), POST /api/fs/delete?path=, POST /api/restart. The
# firmware reads the .so files once at startup, hence the restart.
set -e
ROOT=${0:a:h:h}
HOST=${1:?usage: install_apps.sh <ip-or-name> [app...]}
shift
WANT=("$@")
B="http://$HOST"

files=()
paks=()
for d in $ROOT/apps/*(/); do
    name=${d:t}
    (( ${#WANT[@]} )) && [[ ${WANT[(Ie)$name]} -eq 0 ]] && continue
    so=($d/build/*.so(.N))
    (( ${#so[@]} )) && files+=($so[1])
    for pak in $d/build/*_p4.pak(.N); do
        paks+=(${pak:t})
        files+=($pak $pak.<1-99>(.N))
    done
done
(( ${#files[@]} )) || { echo "nothing built - run ./tools/build_apps.sh first"; exit 1; }

# stale parts of the packs being replaced
for p in $paks; do
    for i in {1..9}; do
        curl -s -o /dev/null --max-time 10 -X POST "$B/api/fs/delete?path=/apps/$p.$i" || true
    done
done

echo "== ${#files[@]} files -> $HOST =="
total=0
for f in $files; do
    name=${f:t}
    size=$(wc -c < $f | tr -d ' ')
    code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 600 \
        -X PUT "$B/api/fs/put?path=/apps/$name" --data-binary "@$f") || code=000
    if [[ $code == 2* ]]; then
        printf "   %-24s %9s B  ok\n" $name $size
        (( total += size ))
    else
        printf "   %-24s %9s B  FAILED (HTTP %s)\n" $name $size $code
        exit 1
    fi
done
echo "   $total bytes in total"

if [[ -n $NORESTART ]]; then
    echo "not restarting (NORESTART): the board registers them at its next start"
    exit 0
fi
echo "restarting the board (the .so files are read at startup)..."
curl -s -o /dev/null --max-time 10 -X POST "$B/api/restart" || true
for i in {1..60}; do
    sleep 1
    curl -s -o /dev/null --max-time 2 "$B/api/apps" && { echo "   back after ${i} s"; exit 0; }
done
echo "   the board did not answer within a minute: look at it"
exit 1
