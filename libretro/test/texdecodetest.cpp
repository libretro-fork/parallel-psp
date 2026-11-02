/* The texture decoder on swizzled textures against the same textures unswizzled up front: every
 * format, CLUT format, decode flag, size and buffer width must decode to the same texels and
 * alpha. Then times decoding large swizzled textures (TEXDECODE_BENCH=1). */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Core/Config.h"
#include "Core/MemMap.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/GPUState.h"

static uint32_t rngState = 0x9E3779B9u;
static uint32_t Next() {
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return rngState;
}

class TestTexCache : public TextureCacheCommon {
public:
	TestTexCache() : TextureCacheCommon(nullptr, nullptr) {}
	void ForgetLastTexture() override {}
	void *GetNativeTextureView(const TexCacheEntry *entry, bool flat) const override { return nullptr; }
	void BindTexture(TexCacheEntry *entry) override {}
	void Unbind() override {}
	void BuildTexture(TexCacheEntry *const entry) override {}
	void ReleaseTexture(TexCacheEntry *entry, bool delete_them) override {}
	void ApplySamplerByKey(const SamplerCacheKey &key) override {}

	void RandomClut() {
		for (int i = 0; i < 512; i++) {
			clutBufRaw_[i] = Next();
			clutBufConverted_[i] = Next();
		}
		clutBuf_ = clutBufConverted_;
	}
	TextureAlpha Decode(u8 *out, int outPitch, GETextureFormat format, GEPaletteFormat clutformat, u32 texaddr, int bufw, TexDecodeFlags flags) {
		return DecodeTextureLevel(out, outPitch, format, clutformat, texaddr, 0, bufw, flags);
	}
	static void Unswizzle(u32 *dest, u32 destPitch, const u8 *src, u32 bufw, u32 h, u32 bpp) {
		UnswizzleFromMem(dest, destPitch, src, bufw, h, bpp);
	}
};

static const int bitsPerTexel[8] = { 16, 16, 16, 32, 4, 8, 16, 32 };

static void SetState(GETextureFormat fmt, GEPaletteFormat clutFmt, int wLog, int hLog, int bufw, u32 addr, bool swizzled) {
	gstate.texformat = (GE_CMD_TEXFORMAT << 24) | fmt;
	gstate.texmode = (GE_CMD_TEXMODE << 24) | (swizzled ? 1 : 0);
	gstate.texsize[0] = (GE_CMD_TEXSIZE0 << 24) | (hLog << 8) | wLog;
	gstate.texbufwidth[0] = (GE_CMD_TEXBUFWIDTH0 << 24) | ((addr >> 8) & 0x0F0000) | bufw;
	gstate.texaddr[0] = (GE_CMD_TEXADDR0 << 24) | (addr & 0xFFFFF0);
	gstate.clutformat = (GE_CMD_CLUTFORMAT << 24) | 0x00FF00 | clutFmt;
}

int main() {
	Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE;
	if (!Memory::Init(Memory::MemMapSetupFlags::Default)) {
		printf("texdecodetest: no memory map: FAIL\n");
		return 1;
	}
	TestTexCache cache;
	const u32 swzAddr = 0x08800000, linAddr = 0x08C00000;
	std::vector<u8> outA(1024 * 1024 * 4), outB(1024 * 1024 * 4);
	std::vector<u32> unswizzled(1024 * 1024);

	long cases = 0, bad = 0;
	for (int fmt = 0; fmt < 8; fmt++) {
		const int bpp = bitsPerTexel[fmt];
		const bool clut = fmt >= GE_TFMT_CLUT4;
		for (int clutFmt = 0; clutFmt < (clut ? 4 : 1); clutFmt++) {
			for (int flagBits = 0; flagBits < 8; flagBits++) {
				const TexDecodeFlags flags = (TexDecodeFlags)flagBits;
				if ((flagBits & 4) && fmt != GE_TFMT_CLUT4 && fmt != GE_TFMT_CLUT8)
					continue;
				for (int wLog = 0; wLog <= 9; wLog++) {
					for (int hLog = 0; hLog <= 9; hLog += (hLog < 4 ? 1 : 2)) {
						const int w = 1 << wLog, h = 1 << hLog;
						/* bufw in 16-byte steps, from the smallest that holds w up to a couple more. */
						const int step = 128 / bpp;
						const int minBufw = ((w + step - 1) / step) * step;
						for (int extra = 0; extra < 3; extra++) {
							const int bufw = minBufw + extra * step * (1 + (int)(Next() % 3));
							if (bufw > 2047)
								continue;
							const int rowBytes = bufw * bpp / 8;
							const int rows = (h + 7) & ~7;
							const u32 bytes = rowBytes * rows;
							u8 *swz = Memory::GetPointerWriteUnchecked(swzAddr);
							for (u32 i = 0; i < bytes; i++)
								swz[i] = (u8)Next();
							TestTexCache::Unswizzle(unswizzled.data(), rowBytes, swz, bufw, h, bpp == 4 ? 0 : bpp / 8);
							memcpy(Memory::GetPointerWriteUnchecked(linAddr), unswizzled.data(), bytes);
							cache.RandomClut();

							const int outBpp = ((flagBits & 1) || fmt == GE_TFMT_8888 || fmt == GE_TFMT_CLUT32 || (clut && clutFmt == GE_CMODE_32BIT_ABGR8888)) ? 4 : 2;
							const int outPitch = w * 4 + 16;
							memset(outA.data(), 0xCD, outPitch * h);
							memset(outB.data(), 0xCD, outPitch * h);
							SetState((GETextureFormat)fmt, (GEPaletteFormat)clutFmt, wLog, hLog, bufw, swzAddr, true);
							const TextureAlpha a = cache.Decode(outA.data(), outPitch, (GETextureFormat)fmt, (GEPaletteFormat)clutFmt, swzAddr, bufw, flags);
							SetState((GETextureFormat)fmt, (GEPaletteFormat)clutFmt, wLog, hLog, bufw, linAddr, false);
							const TextureAlpha b = cache.Decode(outB.data(), outPitch, (GETextureFormat)fmt, (GEPaletteFormat)clutFmt, linAddr, bufw, flags);
							cases++;
							bool same = a == b;
							const int rowOut = (flagBits & 4) ? w : w * outBpp;
							for (int y = 0; y < h && same; y++)
								same = memcmp(outA.data() + outPitch * y, outB.data() + outPitch * y, rowOut) == 0;
							if (!same) {
								if (bad < 8)
									printf("  differs: fmt %d clut %d flags %d %dx%d bufw %d\n", fmt, clutFmt, flagBits, w, h, bufw);
								bad++;
							}
						}
					}
				}
			}
		}
	}
	printf("texdecodetest: %ld of %ld swizzled decodes differ: %s\n", bad, cases, bad ? "FAIL" : "ok");

	if (getenv("TEXDECODE_BENCH")) {
		struct { GETextureFormat fmt; GEPaletteFormat clut; TexDecodeFlags flags; const char *name; } benches[] = {
			{ GE_TFMT_8888, GE_CMODE_16BIT_BGR5650, (TexDecodeFlags)0, "8888" },
			{ GE_TFMT_5551, GE_CMODE_16BIT_BGR5650, (TexDecodeFlags)0, "5551" },
			{ GE_TFMT_5551, GE_CMODE_16BIT_BGR5650, TexDecodeFlags::EXPAND32, "5551->8888" },
			{ GE_TFMT_CLUT8, GE_CMODE_32BIT_ABGR8888, (TexDecodeFlags)0, "CLUT8/8888" },
			{ GE_TFMT_CLUT4, GE_CMODE_32BIT_ABGR8888, (TexDecodeFlags)0, "CLUT4/8888" },
			{ GE_TFMT_CLUT8, GE_CMODE_16BIT_ABGR5551, TexDecodeFlags::TO_CLUT8, "CLUT8 indices" },
		};
		for (auto &bnch : benches) {
			for (int size = 64; size <= 512; size *= 2) {
				int log = 0;
				while ((1 << log) < size) log++;
				SetState(bnch.fmt, bnch.clut, log, log, size, swzAddr, true);
				const int iters = (512 / size) * (512 / size) * 16;
				double ns = 1e9;
				for (int rep = 0; rep < 9; rep++) {
					auto t0 = std::chrono::steady_clock::now();
					for (int i = 0; i < iters; i++)
						cache.Decode(outA.data(), size * 4, bnch.fmt, bnch.clut, swzAddr, size, bnch.flags);
					auto t1 = std::chrono::steady_clock::now();
					ns = std::min(ns, std::chrono::duration<double, std::nano>(t1 - t0).count() / ((double)iters * size * size));
				}
				printf("  %-14s %3dx%-3d %.3f ns/texel\n", bnch.name, size, size, ns);
			}
		}
	}
	Memory::Shutdown();
	return bad ? 1 : 0;
}
