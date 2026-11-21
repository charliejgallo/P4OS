#!/bin/bash
#
# Builds one share of the apps for the CI, and does not build again what it
# already built from the same sources.
#
#   tools/ci_apps.sh <shard> <shards> [<cache folder>]    build that share
#   tools/ci_apps.sh --list <shard> <shards>              only say which apps
#
# The apps took 101 of the CI's 108 minutes (2026-11-21): sixty ESP-IDF
# projects built one after another, each compiling LVGL and the IDF's
# components again to make a .so of a few kilobytes. Two things here:
#
#  1. Shares. The apps, in the order of `ls`, are dealt out to <shards> jobs
#     that run at the same time (the workflow's matrix); this builds the ones
#     whose position leaves <shard> when divided by <shards>.
#
#  2. A cache, by what an app is made from. For each app a hash of everything
#     that can change its .so: its own files, the headers and the symbol table
#     it is built against, the build's configuration, the tools. When the
#     cache folder has that app under that hash, its .so (and the modules it
#     loads itself) are copied back and nothing is built. The same .so, byte
#     for byte, is also what keeps a package's version in the Tienda's
#     catalogue from one release to the next.
#
# What goes into the hash is on the generous side: a change in any header of
# components/ builds every app again, though most apps include few of them.
# Better an hour lost than a stale .so in a release. What is NOT in it and
# should not matter: the commit, the date, the machine, and this script, which
# decides what is built and not how.
#
# Without a cache folder it only deals out and builds, as build_apps.sh would.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
PROFILE=${PROFILE:-rev1_3}

list() {
    ls apps | grep -v '\.cmake$' | while read -r a; do [ -d "apps/$a/main" ] && echo "$a"; done |
        awk -v s="$1" -v n="$2" '(NR - 1) % n == s'
}

if [ "$1" = "--list" ]; then
    list "$2" "$3"
    exit 0
fi
shard=$1; shards=$2; cache=$3
[ -n "$shards" ] || { echo "usage: $0 <shard> <shards> [<cache folder>]"; exit 2; }

sha() { if command -v sha256sum >/dev/null; then sha256sum | cut -c1-64; else shasum -a 256 | cut -c1-64; fi; }

# What every app is built against: git's own hashes of what is committed,
# and what differs from it in the folder, so an edit that was not committed
# does not go unseen. Not the dependencies.lock files: the component manager
# rewrites them while it builds, and what they pin is already in the commit.
LOCKS=':(exclude,glob)**/dependencies.lock'
common() {
    {
        git ls-tree -r HEAD -- tools/build_apps.sh apps/common.cmake components/elf_loader \
            components/aos_dynapp/aos_symbols.c sdkconfig.defaults "sdkconfig.defaults.$PROFILE" \
            main/idf_component.yml dependencies.lock
        git ls-tree -r HEAD -- components | grep -E '\.(h|hpp|inc)$|Kconfig|\.cmake$|CMakeLists\.txt$'
        git diff HEAD --stat -- tools/build_apps.sh apps/common.cmake components sdkconfig.defaults \
            "sdkconfig.defaults.$PROFILE" main/idf_component.yml "$LOCKS"
        # the tools: an IDF or a compiler of another version makes another .so
        cat "$IDF_PATH/version.txt" 2>/dev/null || git -C "$IDF_PATH" describe --tags 2>/dev/null || echo "idf?"
        riscv32-esp-elf-gcc --version 2>/dev/null | head -1 || echo "gcc?"
        echo "profile $PROFILE"
    } | sha
}

app_hash() {
    {
        echo "$COMMON"
        git ls-tree -r HEAD -- "apps/$1"
        git diff HEAD --stat -- "apps/$1" "$LOCKS"
        # CAS links an engine that is built apart and is not in git
        [ "$1" = cas ] && [ -f apps/cas/engine/lib/libcasengine.a ] && sha < apps/cas/engine/lib/libcasengine.a
    } | sha
}

COMMON=$(common)
apps=$(list "$shard" "$shards")
echo "share $shard of $shards: $(echo $apps | wc -w | tr -d ' ') apps; built against $COMMON"

# Each app's hash is taken ONCE, here, before anything is built. The first
# version of this took it again to keep the .so, after the build had
# rewritten the app's dependencies.lock: every app was kept under a name the
# next run never asked for.
hashes=$(mktemp)
build=""
for a in $apps; do
    h=$(app_hash "$a")
    echo "$a $h" >> "$hashes"
    kept="$cache/$a/$h"
    if [ -n "$cache" ] && ls "$kept"/*.so >/dev/null 2>&1; then
        mkdir -p "apps/$a/build"
        cp "$kept"/*.so "apps/$a/build/"
        if [ -d "$kept/modules" ]; then
            mkdir -p "apps/$a/build/modules"
            cp "$kept"/modules/*.so "apps/$a/build/modules/"
        fi
        echo "  as before  $a  ($(ls "$kept"/*.so | xargs -n1 basename | tr '\n' ' '))"
    else
        build="$build $a"
        # for whoever reads the log wondering why: what the cache had of it
        [ -n "$cache" ] && [ -d "$cache/$a" ] && echo "  changed    $a  (kept $(ls "$cache/$a" | cut -c1-12 | tr '\n' ' '), now ${h:0:12})"
    fi
done

if [ -z "$build" ]; then
    echo "nothing to build in this share"
    exit 0
fi
echo "to build:$build"

# build_apps.sh takes the LVGL configuration from the firmware's build
# folder: its sdkconfig is enough, and making it is a minute, not a build
if [ ! -f "build/$PROFILE/sdkconfig" ]; then
    echo "the firmware's configuration ($PROFILE)..."
    tools/build_fw.sh "$PROFILE" reconfigure > "/tmp/ci_apps_fw.log" 2>&1 || { tail -30 /tmp/ci_apps_fw.log; exit 1; }
fi

# One at a time: build_apps.sh refuses a .so that would not load (the ABI,
# a symbol the firmware does not lend, a local path), and only one it let
# through may be kept for the next run. Its failure is this script's.
rc=0
for a in $build; do
    if tools/build_apps.sh "$a"; then
        [ -n "$cache" ] || continue
        h=$(awk -v a="$a" '$1 == a { print $2 }' "$hashes")
        # the workflow keeps the cache only when something new went into it
        [ -n "$GITHUB_OUTPUT" ] && echo "built=true" >> "$GITHUB_OUTPUT"
        # one copy an app: the one of these sources
        rm -rf "$cache/$a"
        mkdir -p "$cache/$a/$h"
        cp "apps/$a/build"/*.so "$cache/$a/$h/"
        if ls "apps/$a/build/modules"/*.so >/dev/null 2>&1; then
            mkdir -p "$cache/$a/$h/modules"
            cp "apps/$a/build/modules"/*.so "$cache/$a/$h/modules/"
        fi
        echo "  kept       $a  ${h:0:12}"
    else
        rc=1
        # nothing of a failed build goes on to the release
        rm -f "apps/$a/build"/*.so
    fi
done
exit $rc
