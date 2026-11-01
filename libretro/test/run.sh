#!/bin/bash
# Image, codec and boot checks for the core. Run from the repo root after
# "make -C libretro test-tools". Needs what make_vectors.py needs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
data=${1:-$(mktemp -d)}
python3 "$here/make_vectors.py" "$data" "$root/pspautotests/tests/cpu/cpu_alu/cpu_alu.prx"

"$here/atrac3ptest"
"$here/mediatest"
"$here/vkheaptest"
# The Vulkan heap is C89 for MSVC: no C99 in it.
cc -std=c89 -ansi -pedantic -Werror=pedantic -Werror=declaration-after-statement -Wno-long-long \
	-Wall -Wextra -Werror -isystem "$root" -fsyntax-only "$root/Common/GPU/Vulkan/VulkanHeap.c"
"$here/codectest" "$data"
for img in test.iso test.cso test16k.cso test_align.cso test_pad.cso test.chd test_zstd.chd test_zlib.chd test_lzma.chd test_huff.chd test_big.chd stored.zip deflated.zip; do
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
# Savestates every 20 frames: a too-small frontend buffer is refused untouched,
# and every state round trips.
save=$(mktemp -d)
line=$(LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 "$here/lrtest" "$root/libretro/ppsspp_libretro.so" \
	"$data/gl/15826 hot pixel ULUS10298.ppdmp" 120 20 | grep -E '^(@@|FAIL)')
rm -rf "$save"
echo "states $line"
case "$line" in *FAIL*) echo "FAIL: savestates"; exit 1;; *frames=120*) ;; *) echo "FAIL: savestate run stopped early"; exit 1;; esac
# Software output converts the display once, into the frontend's own buffer
# when it lends one (with padded rows) and otherwise into the core's: the
# pictures must match, cropped or not.
for crop in disabled enabled; do
	ref=
	for swfb in "" 1; do
		save=$(mktemp -d)
		out=$(LRTEST_SWFB=$swfb LRTEST_OPT="ppsspp_cropto16x9=$crop" LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 "$here/lrtest" \
			"$root/libretro/ppsspp_libretro.so" "$data/gl/15826 hot pixel ULUS10298.ppdmp" 120 | grep '^@@')
		rm -rf "$save"
		line=$(echo "$out" | head -1 | sed -E 's/ dupe=[0-9]+//')
		echo "software crop=$crop swfb=${swfb:-0} $line"
		case "$line" in *frames=120*) ;; *) echo "FAIL: software output run"; exit 1;; esac
		[ -n "$swfb" ] && { echo "$out" | grep -q '^@@ swfb=120$' || { echo "FAIL: frontend buffer not used"; exit 1; }; }
		[ -z "$ref" ] && ref=$line
		[ "$line" = "$ref" ] || { echo "FAIL: software output differs with the frontend buffer"; exit 1; }
	done
done

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
# Frames on Vulkan, as a frontend runs them, under the validation layer:
# every frame shows a picture, at native and at an upscaled resolution, and
# recording on the render thread or without it gives the same pictures.
ref=
for run in "480x272 Up to 1" "480x272 No buffer" "1440x816 Up to 1"; do
	res=${run%% *}
	inflight=${run#* }
	out=$(VKN_FRAMES=120 VKN_DIR="$sys" timeout -k 5 600 "$here/vk_negotiate" \
		"$root/libretro/ppsspp_libretro.so" "$data/gl/15826 hot pixel ULUS10298.ppdmp" \
		ppsspp_backend=vulkan ppsspp_internal_resolution=$res "ppsspp_inflight_frames=$inflight") \
		|| { echo "FAIL: Vulkan frames, $run"; exit 1; }
	line=$(echo "$out" | grep 'frames=')
	echo "vulkan $run: $line"
	case "$line" in *"frames=120 shown=120 "*) ;; *) echo "FAIL: Vulkan frames, $run"; exit 1;; esac
	if [ "$res" = 480x272 ]; then
		[ -z "$ref" ] && ref=$line
		[ "$line" = "$ref" ] || { echo "FAIL: Vulkan pictures differ without the render thread"; exit 1; }
	fi
done

echo "All image and codec checks passed."
