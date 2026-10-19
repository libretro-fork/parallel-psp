#!/bin/bash
# Image, codec and boot checks for the core. Run from the repo root after
# "make -C libretro test-tools". Needs what make_vectors.py needs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
data=${1:-$(mktemp -d)}
python3 "$here/make_vectors.py" "$data" "$root/pspautotests/tests/cpu/cpu_alu/cpu_alu.prx"

"$here/atrac3ptest"
"$here/codectest" "$data"
for img in test.iso test.cso test16k.cso test.chd test_zstd.chd test_zlib.chd test_lzma.chd test_huff.chd test_big.chd stored.zip deflated.zip; do
	"$here/blocktest" "$data/test.iso" "$data/$img"
done

# Every container boots to the same exit, frames and output as the ISO.
sys=$(mktemp -d)
mkdir -p "$sys/PPSSPP"
cp -r "$root/assets/." "$sys/PPSSPP/"
ref=
for img in test.iso test.cso test.chd test_zstd.chd stored.zip deflated.zip; do
	save=$(mktemp -d)
	line=$(LRTEST_SYSTEM=$sys LRTEST_SAVE=$save "$here/lrtest" "$root/libretro/ppsspp_libretro.so" "$data/$img" 600 | grep '^@@' | sed -E 's/ dupe=[0-9]+//')
	rm -rf "$save"
	echo "$img $line"
	case "$line" in *exit=1*) ;; *) echo "FAIL: $img did not run to its exit"; exit 1;; esac
	[ -z "$ref" ] && ref=$line
	[ "$line" = "$ref" ] || { echo "FAIL: $img differs from test.iso"; exit 1; }
done
# The GL backend runs the emulator on a thread of its own. Drive it through every
# path that pauses or stops that thread; Mesa's llvmpipe is enough.
mkdir -p "$data/gl"
unzip -o -q "$root/frametests/dumps/Depth/15826 hot pixel ULUS10298.zip" -d "$data/gl"
save=$(mktemp -d)
LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 "$here/lrgltest" "$root/libretro/ppsspp_libretro.so" "$data/gl/15826 hot pixel ULUS10298.ppdmp" 300 \
	|| { echo "FAIL: GL backend run"; exit 1; }
rm -rf "$save"

# The Vulkan device, negotiated as RetroArch does (v2: made through the
# frontend's vkCreateDevice wrapper, which adds what the frontend needs, e.g.
# for Driver Pacing) and as a frontend without v2 does (v1), under the
# validation layer. Mesa's lavapipe is enough.
for offer in 2 1; do
	expect=0
	[ "$offer" -ge 2 ] && expect=1
	save=$(mktemp -d)
	VKN_OFFER=$offer VKN_EXPECT_WRAPPER=$expect VKN_DIR=$save timeout -k 5 300 "$here/vk_negotiate" \
		"$root/libretro/ppsspp_libretro.so" "$data/test.iso" ppsspp_backend=vulkan \
		|| { echo "FAIL: Vulkan negotiation v$offer"; exit 1; }
	rm -rf "$save"
done

echo "All image and codec checks passed."
