#!/bin/sh
# BASTION - builds and runs the bot that plays the campaign (harness.c).
#   tools/harness.sh [skill 0..1] [-v]        plays it as bsworlds.py has it
#   tools/harness.sh -c [skill] [-t 0.72]      measures the HP table to paste
#                                              into bsworlds.py: per sector,
#                                              the health the bot holds with
#                                              that share of the fort left
set -e
case " $* " in *" -c "*) export BS_FLAT=1 ;; esac
cd "$(dirname "$0")/.."
mkdir -p build/defs
python3 - <<'PY'
import sys
sys.path.insert(0, 'tools')
import bsworlds
for w in bsworlds.WORLDS:
    open('build/defs/%02d.txt' % w.order, 'w').write(w.def_text())
PY
cc -O2 -DAOS_SIM -Imain -I../../components/aos_ui/include -I../../components/aos_hal/include \
   -o build/harness tools/harness.c main/bs_game.c main/bs_world.c main/bs_data.c main/bs_pak.c -lm
./build/harness "$@"
