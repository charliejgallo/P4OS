#!/bin/zsh
#
# Builds every app in apps/ and checks that they are usable on the board.
#
#   ./tools/build_apps.sh            all of them
#   ./tools/build_apps.sh gemas      just one
#   PROFILE=rev3_x ./tools/build_apps.sh   against that firmware profile
#
# P4OS: the apps are built for the ESP32-P4 whatever their own
# sdkconfig.defaults say (-DIDF_TARGET and the profile's defaults on top),
# and checked against the firmware of build/$PROFILE.
#
# It does three things that get forgotten by hand:
#
#  1. Deletes build/so_objs before building. project_so() declares its objects
#     with DEPENDS on the .c alone, without tracking headers, so a change in
#     aos_app.h or aos_hal.h does NOT trigger a rebuild: 'idf.py so' relinks an
#     old object and announces "Build Shared Object" as if nothing were wrong.
#  2. Checks that the ABI number baked into the .so is the one the firmware
#     expects.
#  3. Checks that every undefined symbol of the .so is in the firmware's table,
#     since otherwise the app fails only when it is loaded on the board.
#
set -e

ROOT=${0:a:h:h}
# an environment with ESP-IDF already exported (the CI's container) keeps it
command -v idf.py >/dev/null || source ~/esp/esp-idf/export.sh > /dev/null 2>&1

TABLE=$ROOT/components/aos_dynapp/aos_symbols.c
ABI=$(grep -oE '#define AOS_ABI_VERSION +[0-9]+' $ROOT/components/aos_ui/include/aos_app.h | grep -oE '[0-9]+$')
PROFILE=${PROFILE:-rev1_3}
FW=$ROOT/build/$PROFILE
NM=$(command -v riscv32-esp-elf-nm || ls ~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm | head -1)
OD=$(command -v riscv32-esp-elf-objdump || ls ~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objdump | head -1)
RE=$(command -v riscv32-esp-elf-readelf || ls ~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-readelf | head -1)

if [ ! -f $TABLE ] || ! grep -q ESP_ELFSYM_EXPORT $TABLE; then
    echo "The symbol table is empty. Run this first:"
    echo "  tools/build_fw.sh $PROFILE && python3 tools/gen_symbols.py --build build/$PROFILE && tools/build_fw.sh $PROFILE"
    exit 1
fi

# The app and the firmware share LVGL structures, and some of them change
# layout according to the configuration: lv_global_t has fields under
# "#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN" and under "#if LV_USE_FS_POSIX".
# If the two configurations do not match, every inline LVGL function the app
# compiles writes into the wrong field: silent corruption, which neither heap
# poisoning nor the stack watchpoint detects. Measured: it hung claudito and
# 2043 after a second of drawing correctly.
# That is why the apps' LVGL configuration is DERIVED from the firmware's.
LVCFG=$ROOT/build/aos_lvgl_sync.defaults
if [ ! -f $FW/sdkconfig ]; then
    echo "$FW/sdkconfig is missing: run 'tools/build_fw.sh $PROFILE' on the firmware first."
    exit 1
fi
mkdir -p $ROOT/build
grep -E '^CONFIG_LV_' $FW/sdkconfig > $LVCFG
grep -E '^# CONFIG_LV_[A-Z0-9_]+ is not set$' $FW/sdkconfig |
    sed -E 's/^# (CONFIG_LV_[A-Z0-9_]+) is not set$/\1=n/' >> $LVCFG
echo "LVGL configuration synced from the firmware ($(wc -l < $LVCFG | tr -d ' ') options)"
# the target, after each app's own defaults (which still say esp32s3)
TGT=$ROOT/build/aos_target.defaults
echo 'CONFIG_IDF_TARGET="esp32p4"' > $TGT

apps=${@:-$(ls $ROOT/apps | grep -v '\.cmake$')}
problemas=0

for app in ${(z)apps}; do
    dir=$ROOT/apps/$app
    [ -d $dir/main ] || continue
    echo "=== $app ==="
    cd $dir

    rm -rf build/so_objs
    # The sdkconfig is only deleted when the LVGL config has gone stale:
    # regenerating it forces a full rebuild of LVGL, which is several minutes
    # per app.
    if ! diff -q <(grep -E '^CONFIG_LV_' sdkconfig 2>/dev/null | sort) \
                 <(sort $LVCFG | grep -E '^CONFIG_LV_.*=[^n]|^CONFIG_LV_.*=n') \
                 > /dev/null 2>&1; then
        rm -f sdkconfig
    fi
    # The first time an app is built there is no build/, and the idf log is
    # written inside it: without this, a new app fails with "no such file or
    # directory" and the message says nothing about what is really going on.
    mkdir -p $dir/build
    log=$dir/build/idf_so.log
    # A build/ left from the S3 has the wrong target: start it again
    grep -qs 'CONFIG_IDF_TARGET="esp32p4"' sdkconfig || rm -rf sdkconfig build/CMakeCache.txt
    mkdir -p $dir/build
    if ! idf.py -DIDF_TARGET=esp32p4 \
            -DSDKCONFIG_DEFAULTS="$dir/sdkconfig.defaults;$ROOT/sdkconfig.defaults.$PROFILE;$TGT;$LVCFG" so > $log 2>&1 ||
       ! grep -qE "Linking .*\.so completed" $log; then
        echo "  BUILD FAILED. Last lines of $log:"
        tail -12 $log | sed 's/^/    /'
        problemas=$((problemas + 1))
        continue
    fi

    so=$(ls build/*.so | head -1)

    # The float ABI has to be the firmware's (ilp32f): a soft-float .so passes
    # every float in the wrong registers, and nothing fails until the numbers
    # come out wrong.
    # (readelf and not file(1): the CI's container has no file)
    flags=$($RE -h $so 2>/dev/null | grep Flags)
    if ! echo "$flags" | grep -q "single-float ABI"; then
        echo "  FAIL: $(basename $so) is not single-float ABI:$flags"
        problemas=$((problemas + 1))
        continue
    fi

    # The entry point is two functions, not a structure: elf_loader's dlsym()
    # only finds symbols of function type. The ABI number is therefore not in
    # the rodata but inside aos_app_abi()'s code, which compiles to a single
    # 'movi' with the constant.
    if ! $NM -D --defined-only $so | grep -qE ' aos_app_abi$' ||
       ! $NM -D --defined-only $so | grep -qE ' aos_app_init$'; then
        echo "  FAIL: it does not export aos_app_abi/aos_app_init"
        problemas=$((problemas + 1))
        continue
    fi
    # RISC-V: "li a0,100" (Xtensa said "movi a2, 100")
    abi=$($OD -d --disassemble=aos_app_abi $so |
          grep -oE '(c\.)?li[[:space:]]+a0,-?[0-9]+|movi(\.n)?[[:space:]]+a[0-9]+,[[:space:]]*-?[0-9]+' |
          head -1 | grep -oE '\-?[0-9]+$')
    if [ "$abi" != "$ABI" ]; then
        echo "  FAIL: the .so says ABI $abi and the firmware expects $ABI"
        problemas=$((problemas + 1))
        continue
    fi

    # printf/fprintf/vfprintf come from the table elf_loader brings on its own
    # (see the EXTRA_SYMBOLS comment in gen_symbols.py), not from
    # aos_symbols.c: without this list, the check marked them as missing even
    # though the resolver finds them on the board all the same.
    LOADER_BUILTIN=" printf fprintf vfprintf "
    faltan=""
    for s in $($NM -D -u $so | awk '{print $2}'); do
        case "$LOADER_BUILTIN" in
            *" $s "*) continue ;;
        esac
        grep -q "ESP_ELFSYM_EXPORT($s)" $TABLE || faltan="$faltan $s"
    done
    if [ -n "$faltan" ]; then
        echo "  FAIL: symbols outside the firmware table:$faltan"
        echo "        add them to EXTRA_SYMBOLS in tools/gen_symbols.py and regenerate"
        problemas=$((problemas + 1))
        continue
    fi

    # The .so goes out in the release's apps.zip: no path of this machine may
    # ride inside it. elf_loader.cmake maps them away with -ffile-prefix-map;
    # this catches whatever gets past the map (doom.so shipped /Users/... in
    # its __FILE__ strings from v0.5.6 to v0.7.0).
    rutas=$(strings -n 6 $so | grep -F -e "$ROOT" -e "$HOME" -e "$IDF_PATH" | head -3)
    if [ -n "$rutas" ]; then
        echo "  FAIL: the .so carries local paths:"
        echo "$rutas" | sed 's/^/        /'
        problemas=$((problemas + 1))
        continue
    fi

    printf "  ok  %s  %s  ABI %s  %s symbols\n" \
        $(basename $so) $(du -h $so | cut -f1) $abi $($NM -D -u $so | wc -l | tr -d ' ')
done

echo
if [ $problemas -eq 0 ]; then
    echo "Built. NOT INSTALLED YET: the new .so files are still in apps/*/build/"
    echo "  over wifi:  ./tools/install_apps.sh <board-ip>"
    echo "  by hand:    cp apps/*/build/*.so /Volumes/<sd>/apps/"
    echo ""
    echo "  Without this step the board keeps running the old binaries, and the"
    echo "  symptom misleads: the firmware looks updated and the apps do not."
else
    echo "$problemas app(s) with problems"
    exit 1
fi
