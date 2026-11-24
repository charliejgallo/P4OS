#!/bin/bash
#
# P4OS - Vision: builds esp-dl for the Mac (its plain C kernels, the
# simulator's stand-ins in engine/compat) with our model reader, and runs
# each model's own test vectors (tools/ve_check.cpp).
#
#   apps/vision/tools/ve_check.sh model.espdl ...
#   apps/vision/tools/ve_check.sh --detect face in.ppm out.ppm msr.espdl mnp.espdl
#                                  (tools/ve_detect.cpp: the engine on a picture)
#
# The objects stay in apps/vision/build/check (ignored by git); only what
# changed is compiled again (everything, when a header changed).
set -euo pipefail
TOOL=ve_check
if [ "${1:-}" = --detect ]; then TOOL=ve_detect; shift; fi
HERE=$(cd "$(dirname "$0")/.." && pwd)
E=$HERE/engine
OUT=$HERE/build/check
mkdir -p "$OUT"
INC="-I$E/compat -I$E/gen/c -I$E -I$HERE/main"
for d in dl dl/tool/include dl/tensor/include dl/base dl/base/isa dl/base/isa/c dl/math/include dl/model/include \
         dl/module/include fbs_loader/include vision/detect vision/image vision/image/isa vision/recognition \
         vision/classification; do
    INC="$INC -I$E/espdl/$d"
done
CXX="${CXX:-clang++} -std=gnu++20 -O2 -g -w -fno-exceptions -fno-rtti -DAOS_SIM -DVE_UNPACK_FILTERS=1"
SRCS=$(cd "$E" && ls espdl/dl/tool/src/*.cpp espdl/dl/tensor/src/*.cpp espdl/dl/math/src/*.cpp \
       espdl/dl/model/src/*.cpp espdl/dl/module/src/*.cpp espdl/dl/base/*.cpp espdl/dl/base/isa/c/*.cpp \
       espdl/vision/detect/*.cpp espdl/vision/image/*.cpp espdl/vision/recognition/*.cpp \
       espdl/vision/classification/*.cpp ve_fbs_model.cpp ve_fbs_loader.cpp ve_engine.cpp 2>/dev/null)
# a header newer than the objects: build them all again (no dependency files)
stamp=$OUT/.stamp
if [ -f "$stamp" ] && [ -n "$(find "$E" "$HERE/main" \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \) -newer "$stamp" | head -1)" ]; then
    rm -f "$OUT"/*.o
fi
touch "$stamp"
objs=()
pids=()
for s in $SRCS; do
    o=$OUT/$(echo "$s" | tr / _).o
    objs+=("$o")
    if [ ! -f "$o" ] || [ "$E/$s" -nt "$o" ]; then
        $CXX $INC -c "$E/$s" -o "$o" &
        pids+=($!)
        if [ ${#pids[@]} -ge 8 ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
    fi
done
for p in "${pids[@]+"${pids[@]}"}"; do wait "$p"; done
$CXX $INC "$HERE/tools/$TOOL.cpp" "${objs[@]}" -o "$OUT/$TOOL"
exec "$OUT/$TOOL" "$@"
