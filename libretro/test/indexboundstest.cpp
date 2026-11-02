// GetIndexBounds against a plain loop: 8, 16 and 32-bit indices, every length up to a few
// hundred at every alignment, values around the signed/unsigned edge the SIMD paths have to
// get right. INDEXBOUNDS_BENCH=1 times it instead.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "GPU/ge_constants.h"
#include "GPU/Common/VertexDecoderCommon.h"

static uint32_t rng = 2463534242u;
static uint32_t Next() {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void Reference(const void *inds, int count, int size, u16 *lower, u16 *upper) {
	u32 lo = size == 1 ? 0xFF : 0xFFFF, hi = 0;
	if (size == 4)
		lo = 0x7FFFFFFF;
	for (int i = 0; i < count; i++) {
		u32 v;
		if (size == 1)
			v = ((const u8 *)inds)[i];
		else if (size == 2)
			v = ((const u16 *)inds)[i];
		else
			v = (u16)((const u32 *)inds)[i];
		if (v > hi)
			hi = v;
		if (v < lo)
			lo = v;
	}
	*lower = (u16)lo;
	*upper = (u16)hi;
}

// Values from one of several ranges: all of it, the top, the bottom, either side of 0x80/0x8000.
static u32 Value(int size, int mode) {
	const u32 full = size == 1 ? 0xFF : 0xFFFF;
	const u32 mid = size == 1 ? 0x80 : 0x8000;
	switch (mode) {
	case 0: return Next() & full;
	case 1: return full - (Next() & 7);
	case 2: return Next() & 7;
	case 3: return mid - 4 + (Next() & 7);
	default: return (Next() & 1) ? 0 : full;
	}
}

static const u32 vtypes[3] = { GE_VTYPE_IDX_8BIT, GE_VTYPE_IDX_16BIT, GE_VTYPE_IDX_32BIT };
static const int sizes[3] = { 1, 2, 4 };

static void Bench() {
	std::vector<u8> buf(65536 * 4 + 64);
	for (size_t i = 0; i < buf.size(); i++)
		buf[i] = (u8)Next();
	for (int t = 0; t < 2; t++) {
		const int count = 1536;
		const int reps = 200000;
		u16 lo, hi;
		unsigned sum = 0;
		auto t0 = std::chrono::steady_clock::now();
		for (int r = 0; r < reps; r++) {
			GetIndexBounds(buf.data() + (r & 63) * 2, count, vtypes[t], &lo, &hi);
			sum += lo + hi;
		}
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		auto t1 = std::chrono::steady_clock::now();
		for (int r = 0; r < reps; r++) {
			Reference(buf.data() + (r & 63) * 2, count, sizes[t], &lo, &hi);
			sum += lo + hi;
		}
		const double s1 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
		printf("indexboundstest bench: %d-bit, %d indices: %.1f ns, plain loop %.1f ns (%u)\n", sizes[t] * 8, count,
			s * 1e9 / reps, s1 * 1e9 / reps, sum & 1);
	}
}

int main() {
	if (getenv("INDEXBOUNDS_BENCH")) {
		Bench();
		return 0;
	}
	int failures = 0;
	long checks = 0;
	std::vector<u8> buf(600 * 4 + 64);
	for (int t = 0; t < 3; t++) {
		for (int count = 0; count <= 520; count++) {
			for (int offset = 0; offset < 8; offset++) {
				for (int mode = 0; mode < 5; mode++) {
					u8 *p = buf.data() + offset * sizes[t];
					for (int i = 0; i < count; i++) {
						const u32 v = Value(sizes[t], mode);
						if (sizes[t] == 1)
							p[i] = (u8)v;
						else if (sizes[t] == 2)
							memcpy(p + i * 2, &v, 2);
						else
							memcpy(p + i * 4, &v, 4);
					}
					// Put the extremes at the ends now and then: the tails are handled apart.
					if (count > 0 && (Next() & 3) == 0) {
						const u32 v = (Next() & 1) ? Value(sizes[t], 1) : Value(sizes[t], 2);
						memcpy(p + (count - 1) * sizes[t], &v, sizes[t]);
					}
					u16 lo = 0x1234, hi = 0x5678, rlo, rhi;
					GetIndexBounds(p, count, vtypes[t], &lo, &hi);
					Reference(p, count, sizes[t], &rlo, &rhi);
					checks++;
					if (lo != rlo || hi != rhi) {
						if (failures++ < 5)
							printf("FAIL: %d-bit, count %d, offset %d, mode %d: got %04x-%04x, want %04x-%04x\n",
								sizes[t] * 8, count, offset, mode, lo, hi, rlo, rhi);
					}
				}
			}
		}
	}
	if (failures) {
		printf("indexboundstest: %d of %ld failed\n", failures, checks);
		return 1;
	}
	printf("indexboundstest: %ld index ranges match the plain loop\n", checks);
	return 0;
}
