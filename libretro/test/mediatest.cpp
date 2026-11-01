/* Copyright (c) 2012- PPSSPP Project.
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.0 or later versions.
 */

/* MediaEngine::WriteVideoRows against a scalar reference: every pixel mode,
 * clipped and offset ranges, destination lines narrower than the frame and
 * heights that are not a multiple of the 8-row swizzle block. Guard bytes
 * around each destination catch any write past what the caller owns. */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Core/HW/MediaEngine.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/ge_constants.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const int GUARD = 64;
static const unsigned char GUARD_BYTE = 0xA5;

static int Bpp(int mode) {
	return mode == GE_CMODE_32BIT_ABGR8888 ? 4 : 2;
}

/* What a PSP video row write produces: the source pixel with alpha cleared. */
static void RefPixel(unsigned char *d, const unsigned char *s, int mode) {
	switch (mode) {
	case GE_CMODE_32BIT_ABGR8888:
		d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0;
		break;
	case GE_CMODE_16BIT_BGR5650:
		d[0] = s[0]; d[1] = s[1];
		break;
	case GE_CMODE_16BIT_ABGR5551:
		d[0] = s[0]; d[1] = s[1] & 0x7F;
		break;
	case GE_CMODE_16BIT_ABGR4444:
		d[0] = s[0]; d[1] = s[1] & 0x0F;
		break;
	}
}

static void RunCase(int mode, bool swizzle, int frameWidth, int srcW, int srcH,
		int x, int y, int w, int h) {
	const int bpp = Bpp(mode);
	const int line = frameWidth * bpp;
	/* The caller owns line * h bytes; a swizzled write covers whole 8-row blocks. */
	const int rows = swizzle ? (h + 7) / 8 * 8 : h;
	const size_t owned = (size_t)line * rows;
	if (owned == 0) {
		/* Nothing is owned, so nothing may be written. */
		unsigned char guard[GUARD];
		memset(guard, GUARD_BYTE, sizeof(guard));
		std::vector<unsigned char> src0((size_t)srcW * srcH * bpp, 1);
		const int ret0 = MediaEngine::WriteVideoRows(guard, swizzle, frameWidth, mode,
			src0.data(), srcW, srcH, x, y, w, h);
		CHECK(ret0 == 0, "empty write returned %d", ret0);
		for (int i = 0; i < GUARD; i++)
			CHECK(guard[i] == GUARD_BYTE, "empty write touched the destination");
		return;
	}

	std::vector<unsigned char> src((size_t)srcW * srcH * bpp);
	for (size_t i = 0; i < src.size(); i++)
		src[i] = (unsigned char)(i * 131 + 7);

	/* The destination is exactly what the caller owns, with guards either side. */
	std::vector<unsigned char> buf(owned + 2 * GUARD, GUARD_BYTE);
	unsigned char *dest = buf.data() + GUARD;
	std::vector<unsigned char> before(dest, dest + owned);

	const int ret = MediaEngine::WriteVideoRows(dest, swizzle, frameWidth, mode,
		src.data(), srcW, srcH, x, y, w, h);
	CHECK(ret == line * h, "mode %d swz %d fw %d (%d,%d %dx%d): returned %d, want %d",
		mode, swizzle, frameWidth, x, y, w, h, ret, line * h);

	for (int i = 0; i < GUARD; i++) {
		if (buf[i] != GUARD_BYTE || buf[GUARD + owned + i] != GUARD_BYTE) {
			CHECK(false, "mode %d swz %d fw %d (%d,%d %dx%d): wrote outside the destination",
				mode, swizzle, frameWidth, x, y, w, h);
			return;
		}
	}

	/* Clip exactly as the PSP rect rules say, independently of the code under test. */
	int cw = w, ch = h;
	if (cw > srcW - x) cw = srcW - x;
	if (ch > srcH - y) ch = srcH - y;
	if (cw > frameWidth) cw = frameWidth;
	if (x < 0 || y < 0 || cw < 0 || ch < 0) cw = ch = 0;

	/* Linear expectation: written pixels, untouched bytes elsewhere (zero in the swizzle temp). */
	std::vector<unsigned char> linear(owned);
	if (swizzle)
		memset(linear.data(), 0, owned);
	else
		memcpy(linear.data(), before.data(), owned);
	for (int r = 0; r < ch && cw > 0; r++)
		for (int c = 0; c < cw; c++)
			RefPixel(&linear[(size_t)r * line + (size_t)c * bpp],
				&src[((size_t)(y + r) * srcW + (x + c)) * bpp], mode);

	std::vector<unsigned char> expect(owned);
	if (swizzle) {
		memcpy(expect.data(), before.data(), owned);
		/* Only the clipped rows' blocks are written; an empty clip writes nothing. */
		if (cw > 0 && ch > 0)
			DoSwizzleTex16((const u32 *)linear.data(), expect.data(), line / 16, (ch + 7) / 8, line);
	} else {
		expect = linear;
	}
	CHECK(memcmp(expect.data(), dest, owned) == 0,
		"mode %d swz %d fw %d (%d,%d %dx%d): pixels differ from the reference",
		mode, swizzle, frameWidth, x, y, w, h);
}

int main() {
	static const int modes[] = {
		GE_CMODE_16BIT_BGR5650, GE_CMODE_16BIT_ABGR5551,
		GE_CMODE_16BIT_ABGR4444, GE_CMODE_32BIT_ABGR8888,
	};
	for (int m = 0; m < 4; m++) {
		const int mode = modes[m];
		for (int s = 0; s < 2; s++) {
			const bool swz = s != 0;
			/* The usual video: 480x272 into 512-wide lines. */
			RunCase(mode, swz, 512, 480, 272, 0, 0, 480, 272);
			/* Ranges inside the frame, odd heights included. */
			RunCase(mode, swz, 512, 480, 272, 16, 8, 64, 64);
			RunCase(mode, swz, 512, 480, 272, 3, 5, 77, 13);
			RunCase(mode, swz, 256, 480, 272, 100, 200, 120, 100);
			/* Lines narrower than the frame. */
			RunCase(mode, swz, 256, 480, 272, 0, 0, 480, 272);
			RunCase(mode, swz, 128, 480, 270, 0, 0, 480, 270);
			/* Origins past the frame edge write nothing. */
			RunCase(mode, swz, 512, 480, 272, 500, 0, 16, 16);
			RunCase(mode, swz, 512, 480, 272, 0, 300, 16, 16);
			RunCase(mode, swz, 512, 480, 272, -4, 0, 16, 16);
			/* Empty requests. */
			RunCase(mode, swz, 512, 480, 272, 0, 0, 0, 0);
		}
	}
	if (failures) {
		printf("mediatest: %d failures\n", failures);
		return 1;
	}
	printf("mediatest: all video row writes match the reference\n");
	return 0;
}
