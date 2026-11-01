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
"$here/gldelayedtest"
"$here/mbitest"
"$here/sasreverbtest"
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
# The same dump without the stops: it shows one picture, so every lit frame
# must be that one, with no GL errors.
save=$(mktemp -d)
out=$(LRGL_PLAIN=1 LRGL_FRAMES=1 LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 "$here/lrgltest" \
	"$root/libretro/ppsspp_libretro.so" "$data/gl/15826 hot pixel ULUS10298.ppdmp" 120) || { echo "FAIL: GL plain run"; exit 1; }
rm -rf "$save"
pictures=$(echo "$out" | grep '^@lit ' | sort -u | wc -l)
echo "gl plain: $(echo "$out" | grep '^@@') pictures=$pictures"
case "$out" in *"gl_errors=0"*) ;; *) echo "FAIL: GL errors"; exit 1;; esac
[ "$pictures" -eq 1 ] || { echo "FAIL: GL showed $pictures different pictures of one"; exit 1; }
# Nothing reads debug groups on a plain context, so the core must not push any;
# a debug context gets them, and the same picture.
case "$out" in *" debug_groups=0"*) ;; *) echo "FAIL: debug groups on a plain context"; exit 1;; esac
save=$(mktemp -d)
dbg=$(LRGL_DEBUG_CONTEXT=1 LRGL_PLAIN=1 LRGL_FRAMES=1 LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 "$here/lrgltest" \
	"$root/libretro/ppsspp_libretro.so" "$data/gl/15826 hot pixel ULUS10298.ppdmp" 120) || { echo "FAIL: GL debug context run"; exit 1; }
rm -rf "$save"
echo "gl debug: $(echo "$dbg" | grep '^@@')"
case "$dbg" in *" debug_groups=0"*) echo "FAIL: no debug groups on a debug context"; exit 1;; esac
[ "$(echo "$dbg" | grep '^@lit ' | sort -u)" = "$(echo "$out" | grep '^@lit ' | sort -u)" ] || { echo "FAIL: debug context picture differs"; exit 1; }
# A context loss recreates every GPU object; the picture after it must not change.
save=$(mktemp -d)
ctx=$(LRGL_CONTEXT_ONLY=1 LRGL_FRAMES=1 LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 "$here/lrgltest" \
	"$root/libretro/ppsspp_libretro.so" "$data/gl/15826 hot pixel ULUS10298.ppdmp" 160) || { echo "FAIL: GL context loss run"; exit 1; }
rm -rf "$save"
echo "gl context loss: $(echo "$ctx" | grep '^@@')"
[ "$(echo "$ctx" | grep '^@lit ' | sort -u)" = "$(echo "$out" | grep '^@lit ' | sort -u)" ] || { echo "FAIL: picture changed across a context loss"; exit 1; }
# Lost output goes dark rather than wrong. How long the new context takes to show
# anything depends on timing, but well after the loss every frame is lit again.
ctx_dark=$(echo "$ctx" | grep -E '^@(lit|dark)' | tail -n 20 | grep -c '^@dark' || true)
[ "$ctx_dark" -eq 0 ] || { echo "FAIL: $ctx_dark of the last 20 frames dark after a context loss"; exit 1; }
# Savestates every 20 frames: a too-small frontend buffer is refused untouched,
# and every state round trips.
save=$(mktemp -d)
line=$(LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 "$here/lrtest" "$root/libretro/ppsspp_libretro.so" \
	"$data/gl/15826 hot pixel ULUS10298.ppdmp" 120 20 | grep -E '^(@@|FAIL)')
rm -rf "$save"
echo "states $line"
case "$line" in *FAIL*) echo "FAIL: savestates"; exit 1;; *frames=120*) ;; *) echo "FAIL: savestate run stopped early"; exit 1;; esac
# Run-ahead and rewind load a state saved a moment before, so a save and load must
# change nothing: each test runs to its end without states and with a round trip
# every frame, and must show, play and print the same. These cover the kernel's
# waits and callbacks, the display set up at boot, GE transfers into VRAM, and the
# ATRAC and MP3 decoders' history.
for t in threads/callbacks/intrnotify threads/callbacks/waittypes threads/scheduling/syscallkinds \
		gpu/transfer/mirrors gpu/transfer/overlap gpu/ge/edramswizzle cpu/cpu_alu/cpu_alu \
		audio/atrac/replay audio/mp3/mp3test audio/mp3/stream; do
	dir="$root/pspautotests/tests/$(dirname "$t")"
	prx="$root/pspautotests/tests/$t.prx"
	for every in 0 1; do
		save=$(mktemp -d)
		res=$(cd "$dir" && LRTEST_LOG="$save/log" LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 \
			"$here/lrtest" "$root/libretro/ppsspp_libretro.so" "$prx" 900 $every | grep -E '^(@@ frames|FAIL)' | sed -E 's/ dupe=[0-9]+//')
		printed=$(grep -E '^\[(x|r| |n|-)\]|^\[sceIo\]' "$save/log" | md5sum)
		rm -rf "$save"
		if [ $every = 0 ]; then res0=$res; printed0=$printed; else res1=$res; printed1=$printed; fi
	done
	echo "round trip $t: $res1"
	case "$res1" in *exit=1*) ;; *) echo "FAIL: $t did not run to its end with savestates"; exit 1;; esac
	[ "$res0" = "$res1" ] && [ "$printed0" = "$printed1" ] || { echo "FAIL: $t changes across savestate round trips ($res0)"; exit 1; }
done
# Saving straight after a load gives back the state loaded, with video decoding.
save=$(mktemp -d)
line=$(cd "$root/pspautotests/tests/video/pmf" && LRTEST_IDEMPOTENT=1 LRTEST_SYSTEM=$sys LRTEST_SAVE=$save timeout -k 5 300 \
	"$here/lrtest" "$root/libretro/ppsspp_libretro.so" "$root/pspautotests/tests/video/pmf/pmf.prx" 120 1 | grep -E '^(@@ frames|FAIL)')
rm -rf "$save"
echo "state idempotence: $line"
case "$line" in *FAIL*) echo "FAIL: a state saved after loading differs"; exit 1;; *frames=120*) ;; *) echo "FAIL: idempotence run stopped early"; exit 1;; esac
# A load keeps the JIT's blocks whose code it leaves unchanged. Whatever the JIT then runs
# has to match the IR interpreter, which starts over at every load: going back over the
# same code (rewind), and loading a state from another program, whose code sits at the
# same addresses and must replace every block compiled before.
jit_vs_ir() {
	local out=
	for cpu in JIT "IR JIT"; do
		save=$(mktemp -d)
		res=$(cd "$1" && LRTEST_LOG="$save/log" LRTEST_OPT="ppsspp_cpu_core=$cpu" LRTEST_SYSTEM=$sys LRTEST_SAVE=$save \
			timeout -k 5 300 "$here/lrtest" "$root/libretro/ppsspp_libretro.so" "$2" "$3" | grep -E '^(@@|FAIL)' \
			| sed -E 's/ dupe=[0-9]+//; s/ video_digest=[0-9a-f]+//' | tr '\n' ' ')
		res="$res printed=$(grep -E '^\[(x|r| |n|-)\]|^\[sceIo\]' "$save/log" | md5sum | cut -c1-12)"
		rm -rf "$save"
		[ -z "$out" ] && out=$res
		[ "$res" = "$out" ] || { echo "FAIL: $4: JIT and IR interpreter differ ($out / $res)"; exit 1; }
	done
	echo "$4: $out"
	case "$out" in *FAIL*) echo "FAIL: $4 savestates"; exit 1;; esac
}
LRTEST_REWIND=16 jit_vs_ir "$root/pspautotests/tests/video/pmf" "$root/pspautotests/tests/video/pmf/pmf.prx" 300 "jit rewind"
other=$(mktemp -d)
(cd "$root/pspautotests/tests/gpu/ge" && LRTEST_STATE_DIR=$other LRTEST_OPT="ppsspp_cpu_core=JIT" LRTEST_SYSTEM=$sys LRTEST_SAVE=$other \
	timeout -k 5 300 "$here/lrtest" "$root/libretro/ppsspp_libretro.so" "$root/pspautotests/tests/gpu/ge/edramswizzle.prx" 12 10 >/dev/null)
[ -f "$other/state00010.bin" ] || { echo "FAIL: no state from edramswizzle"; exit 1; }
LRTEST_LOAD="3:$other/state00010.bin" jit_vs_ir "$root/pspautotests/tests/threads/callbacks" \
	"$root/pspautotests/tests/threads/callbacks/intrnotify.prx" 200 "jit other program's state"
rm -rf "$other"
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
