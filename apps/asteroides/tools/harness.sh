#!/bin/sh
# DERIVA - builds and runs the game with no screen (harness.c).
#   tools/harness.sh -m campaign|arcade [-d 0|1|2] [-n runs] [-S first sector] [-g] [-l] [-v]
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -O2 -Wall -Wextra -Wno-unused-parameter -DAST_HARNESS -Imain -o build/harness tools/harness.c main/ast_game.c main/ast_bot.c -lm
./build/harness "$@"
