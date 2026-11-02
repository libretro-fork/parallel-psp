#!/bin/bash
# Every pspautotests gpu test through the software renderer, built with
#   make -C libretro SOFTGPU_SANITIZE="-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all" ppsspp_libretro.so test/lrtest
# Fails on any undefined behaviour UBSan finds in GPU/Software, or a test that doesn't run its frames.
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/system" "$work/save"
export UBSAN_OPTIONS=print_stacktrace=1 LRTEST_SYSTEM="$work/system"
run() {
	save=$(mktemp -d -p "$work/save")
	out=$(cd "$(dirname "$1")" && LRTEST_SAVE=$save timeout -k 5 300 "$here/lrtest" "$root/libretro/ppsspp_libretro.so" "$1" 60 2>&1)
	name=${1#$root/pspautotests/tests/}
	if echo "$out" | grep -q "runtime error"; then
		echo "FAIL: $name"; echo "$out" | grep -A8 "runtime error" | head -12
	elif ! echo "$out" | grep -q '^@@ frames='; then
		echo "FAIL: $name did not run"; echo "$out" | tail -5
	else
		echo "ok: $name"
	fi
}
export -f run
export here root work
find "$root/pspautotests/tests/gpu" -name '*.prx' | sort | xargs -P"$(nproc)" -I{} bash -c 'run "$@"' _ {} > "$work/results"
grep -v '^ok: ' "$work/results"
n=$(grep -c '^ok: ' "$work/results")
echo "softgpu ubsan: $n gpu tests clean"
! grep -q '^FAIL' "$work/results"
