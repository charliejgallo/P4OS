#!/bin/sh
# HOCKEY - builds and runs the computer against itself (harness.c).
#   tools/harness.sh [-a diff -b diff] [-n matches] [-t minutes | -g goals] [-p] [-v]
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -O2 -Wall -Wextra -Wno-unused-parameter -DAOS_SIM -Imain -o build/harness tools/harness.c main/hk_game.c main/hk_ai.c -lm
./build/harness "$@"
