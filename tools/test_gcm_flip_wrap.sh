#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
case "$(uname -s)" in
 Darwin) linkflags='-Wl,-dead_strip';;
 *) linkflags='-Wl,--gc-sections';;
esac
${CC:-cc} -std=gnu17 -ffunction-sections -fdata-sections -I "$root/include" -I "$root/libs/video" \
 "$root/libs/video/tests/test_gcm_flip_wrap.c" "$root/libs/video/tests/gcm_deferred_fixture_stubs.c" $linkflags -pthread -o "$work/test"
"$work/test"
