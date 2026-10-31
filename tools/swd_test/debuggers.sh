#!/bin/bash
# The board's probe (Programador, "Sonda USB": CMSIS-DAP v2 on the OTG
# connector) driven by the debuggers everyone uses, against a real target
# on the board's SWD port: OpenOCD, gdb through OpenOCD, and probe-rs.
#
#     tools/swd_test/debuggers.sh rp2040 [image.bin]     a Pico
#     tools/swd_test/debuggers.sh nrf52 [image.hex]      an nRF52840 (the badge)
#
# The probe open on the board first, the OTG connector to the Mac, and
# nothing in the board's "2.0" socket (pins 25/27 are the OTG connector's
# own wires). An image given is also written with OpenOCD's program, and
# read back; without one nothing on the target changes, only a halt, a
# breakpoint and a resume. Needs open-ocd, probe-rs-tools, arm-none-eabi-gdb
# (Homebrew). Each step says ok or FAIL; the exit is the failures' count.
set -u
TARGET=${1:-rp2040}
IMAGE=${2:-}
KHZ=${KHZ:-4000}
case "$TARGET" in
rp2040) CFG=target/rp2040.cfg; CHIP=RP2040; FLASH=0x10000000; RAM=0x20000000 ;;
nrf52)  CFG=target/nrf52.cfg;  CHIP=nRF52840_xxAA; FLASH=0x00000000; RAM=0x20000000 ;;
*) echo "rp2040 or nrf52"; exit 2 ;;
esac
OCD=(openocd -f interface/cmsis-dap.cfg -c "adapter speed $KHZ" -f "$CFG")
LOG=$(mktemp -d)
fails=0
step() {      # step <what> <log> <command...>: ok when the command exits 0
    local what=$1 log=$2; shift 2
    if "$@" > "$LOG/$log" 2>&1; then echo "ok    $what"; else echo "FAIL  $what (log: $LOG/$log)"; fails=$((fails + 1)); fi
}

echo "== the probe, as the Mac sees it"
step "probe-rs lists it" list sh -c "probe-rs list | grep -i 'CMSIS-DAP'"
grep -i 'cmsis' "$LOG/list" | head -3

echo "== OpenOCD"
step "connect, halt, the PC, RAM, resume" ocd "${OCD[@]}" -c "init; halt; reg pc; mdw $RAM 4; mdw $FLASH 4; resume; shutdown"
grep -E 'CMSIS-DAP|SWD DPIDR|Cortex|pc \(' "$LOG/ocd" | head -8

# gdb: a hardware breakpoint on the reset handler, the vector table's second
# word (an RP2040's table is after its 256-byte boot2)
VEC=$([ "$TARGET" = rp2040 ] && echo 0x100 || echo 0)
"${OCD[@]}" -c "init" > "$LOG/ocd-gdb" 2>&1 &
OCD_PID=$!
sleep 2
step "gdb: reset, a breakpoint on the reset handler, continue" gdb arm-none-eabi-gdb -nx -batch \
    -ex "target extended-remote :3333" \
    -ex "monitor reset halt" \
    -ex "set \$reset = *(unsigned *)($FLASH + $VEC + 4) & ~1" \
    -ex "printf \"reset handler at 0x%08x\\n\", \$reset" \
    -ex "hbreak *\$reset" \
    -ex "continue" \
    -ex "printf \"stopped at 0x%08x\\n\", \$pc" \
    -ex "if \$pc != \$reset" -ex "quit 1" -ex "end" \
    -ex "delete" -ex "monitor resume" -ex "detach"
grep -E 'reset handler at|stopped at' "$LOG/gdb"
kill "$OCD_PID" 2>/dev/null; wait "$OCD_PID" 2>/dev/null

if [ -n "$IMAGE" ]; then
    echo "== writing $IMAGE"
    case "$IMAGE" in
    *.bin) PROG="program $IMAGE $FLASH verify reset" ;;
    *) PROG="program $IMAGE verify reset" ;;
    esac
    step "OpenOCD program and verify" prog "${OCD[@]}" -c "$PROG; shutdown"
    grep -E 'wrote|Verified|verified|Error' "$LOG/prog" | head -4
fi

echo "== probe-rs"
step "probe-rs info" info probe-rs info --protocol swd --speed "$KHZ"
step "probe-rs reads the flash's first words" read probe-rs read --chip "$CHIP" --protocol swd --speed "$KHZ" b32 "$FLASH" 4
cat "$LOG/read" | tail -1

echo
[ "$fails" = 0 ] && echo "all good" || echo "$fails failed"
exit "$fails"
