#!/bin/sh
# TRADUCTOR - the neural translator on the Mac, built the way the board runs
# it: one thread, float, no BLAS, no vectorizing. tools/bench_nmt.c says
# what it prints.
#
#   tools/bench_nmt.sh <pack> <dir> [--int8] [--card] [--runs N] "text" ...
#   tools/bench_nmt.sh traductor_es_en_p4.pak esen --card "Hola, ¿cómo estás?"
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/bench_nmt
cc -std=c11 -O2 -fno-vectorize -fno-slp-vectorize -DTD_BENCH -Wall -Wextra -Wno-unused-parameter \
   -o "$out" "$here/bench_nmt.c" "$here/../main/td_nmt.c" "$here/../main/td_spm.c" "$here/../main/td_pak.c" -lm
exec "$out" "$@"
