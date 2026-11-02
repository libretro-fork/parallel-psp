// The software renderer's power-of-two scalings against the libm calls they stand in for: every
// integer the GE reciprocal tables give at every exponent, and plane values at every shift a
// triangle's UVs can need. Bit for bit, or the rendering would change.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "GPU/Software/GEMath.h"

static bool SameBits(float a, float b) {
	return memcmp(&a, &b, sizeof(a)) == 0;
}

int main() {
	long bad = 0, checked = 0;
	// GEScaleInt: q up to 2^17 (the tables' range, and some), k far into denormals and overflow.
	for (int k = -200; k <= 200; k++) {
		for (int32_t q = 0; q <= (1 << 17) + 16; q++) {
			if (!SameBits(GEScaleInt(q, k), ldexpf((float)q, k)))
				bad++;
			checked++;
		}
	}
	// A plane value times 2^-shift: SharedShift keeps shift in about -120..160 for float24 UVs.
	uint64_t x = 0x9E3779B97F4A7C15ull;
	for (int shift = -130; shift <= 170; shift++) {
		const double scale = std::ldexp(1.0, -shift);
		for (int i = 0; i < 20000; i++) {
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
			const int64_t v = (int64_t)(x >> (i % 40));
			const int64_t sv = (i & 1) ? -v : v;
			const double a = (double)sv * scale, b = std::ldexp((double)sv, -shift);
			if (memcmp(&a, &b, sizeof(a)) != 0)
				bad++;
			checked++;
		}
	}
	printf("swmathtest: %ld of %ld differ: %s\n", bad, checked, bad ? "FAIL" : "ok");
	return bad ? 1 : 0;
}
