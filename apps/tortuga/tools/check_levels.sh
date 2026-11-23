#!/bin/sh
# Tortuga - every level checked twice: by the C core (tools/tg_harness.c,
# which also writes each Logo solution as Lua and Python) and by the model
# (tools/tg_ref.py), which runs the three languages. Both have to agree.
#
#   apps/tortuga/tools/check_levels.sh [levels/mundos.tl]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(dirname "$HERE")
TL=${1:-$APP/levels/mundos.tl}
OUT=$APP/build/check
mkdir -p "$OUT/gen"
cc -O1 -Wall -DTG_NO_I18N -I"$APP/main" -o "$OUT/tg_harness" "$HERE/tg_harness.c" "$APP/main/tg_world.c" "$APP/main/tg_logo.c" -lm
[ -f "$OUT/liblua.dylib" ] || cc -O2 -shared -fPIC -o "$OUT/liblua.dylib" "$APP"/lua/*.c
echo "== C"
"$OUT/tg_harness" "$TL" --gen "$OUT/gen"
echo "== model"
python3 "$HERE/tg_ref.py" check --lua-lib "$OUT/liblua.dylib" --gen "$OUT/gen" "$TL"
