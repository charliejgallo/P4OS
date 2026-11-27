#!/bin/sh
# CIELO - the engine's test on the computer (test_astro.c says what it
# compares). Builds into a temporary directory and runs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/cielo_test_$$
cc -O2 -std=c11 -Wall -Wextra -o "$out" "$here/test_astro.c" "$here/../main/cl_astro.c" \
    "$here/../main/cl_sgp4.c" "$here/../main/cl_dmath.c" -lm
"$out"
st=$?
rm -f "$out"
exit $st
