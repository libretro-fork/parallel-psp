/* The software renderer's jitted pixel and sampler functions against the C++ ones they stand in
 * for, called through the same function pointer types the rasterizer calls them through, so with
 * the compiler's own calling convention. Windows builds without __vectorcall (MinGW GCC) pass the
 * vector color argument by reference, and the jitted code read it from an XMM register that held
 * garbage: wrong colors and crashes in every game (libretro-fork/parallel-psp#1). The outputs
 * must match exactly for random function IDs, colors, textures and framebuffer contents. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Common/Data/Random/Rng.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/Software/BinManager.h"
#include "GPU/Software/DrawPixel.h"
#include "GPU/Software/Rasterizer.h"
#include "GPU/Software/Sampler.h"

static int samplerJitted, pixelJitted;
static uint32_t rngState = 0x2545F491u;
static uint32_t Next() {
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return rngState;
}

static void ToInts(const Rasterizer::Vec4IntResult &r, int out[4]) {
	Math3D::Vec4<int> v(r);
	for (int i = 0; i < 4; i++)
		out[i] = v[i];
}

static Rasterizer::Vec4IntResult RandomColor() {
	return Rasterizer::ToVec4IntResult(Math3D::Vec4<int>(Next() & 255, Next() & 255, Next() & 255, Next() & 255));
}

/* Returns the number of IDs whose jitted function disagreed, or -1 when nothing was jitted. */
static int TestSampler(BinManager &binner) {
	enum { LEVELS = 8, TEXBYTES = 64 * 64 * 4 };
	static u8 tex[LEVELS][TEXBYTES];
	static u8 clut[4096];
	u8 *tptr[LEVELS];
	uint16_t bufw[LEVELS];
	for (int l = 0; l < LEVELS; l++) {
		for (int i = 0; i < TEXBYTES; i++)
			tex[l][i] = (u8)Next();
		tptr[l] = tex[l];
		bufw[l] = 4;
	}
	for (int i = 0; i < (int)sizeof(clut); i++)
		clut[i] = (u8)Next();

	int bad = 0, jitted = 0;
	for (int n = 0; n < 2000; ) {
		SamplerID id;
		memset(&id, 0, sizeof(id));
		/* A consistent ID, as ComputeSamplerID would make: a direct color format, no CLUT. */
		id.fullKey = Next();
		id.texfmt = Next() & 3;
		id.clutfmt = 0;
		id.useSharedClut = false;
		id.hasClutMask = false;
		id.hasClutShift = false;
		id.hasClutOffset = false;
		id.hasInvalidPtr = false;
		id.useStandardBufw = false;
		id.width0Shift = Next() % 7;
		id.height0Shift = Next() % 7;
		id.hasAnyMips = false;
		id.fetch = false;
		id.cached.clut = clut;
		id.cached.texBlendColor = Next() & 0xFFFFFF;
		for (int l = 0; l < LEVELS; l++) {
			id.cached.sizes[l].w = 1 << id.width0Shift;
			id.cached.sizes[l].h = 1 << id.height0Shift;
			bufw[l] = id.cached.sizes[l].w < 4 ? 4 : id.cached.sizes[l].w;
		}
		if (startsWith(DescribeSamplerID(id), "INVALID"))
			continue;
		n++;

		for (int linear = 0; linear < 2; linear++) {
			g_Config.bSoftwareRenderingJit = true;
			Sampler::NearestFunc jn = Sampler::GetNearestFunc(id, &binner);
			Sampler::LinearFunc jl = Sampler::GetLinearFunc(id, &binner);
			g_Config.bSoftwareRenderingJit = false;
			Sampler::NearestFunc cn = Sampler::GetNearestFunc(id, &binner);
			Sampler::LinearFunc cl = Sampler::GetLinearFunc(id, &binner);
			if (linear ? jl == cl : jn == cn)
				continue;
			jitted++;
			bool differs = false;
			for (int k = 0; k < 16; k++) {
				const float s = (float)(Next() & 0xFFFF) / 65536.0f, t = (float)(Next() & 0xFFFF) / 65536.0f;
				const Rasterizer::Vec4IntResult prim = RandomColor();
				const int level = 0, levelFrac = 0;
				int a[4], b[4];
				if (linear) {
					ToInts(jl(s, t, Rasterizer::ToVec4IntArg(prim), tptr, bufw, level, levelFrac, id), a);
					ToInts(cl(s, t, Rasterizer::ToVec4IntArg(prim), tptr, bufw, level, levelFrac, id), b);
				} else {
					ToInts(jn(s, t, Rasterizer::ToVec4IntArg(prim), tptr, bufw, level, levelFrac, id), a);
					ToInts(cn(s, t, Rasterizer::ToVec4IntArg(prim), tptr, bufw, level, levelFrac, id), b);
				}
				if (memcmp(a, b, sizeof(a)) != 0)
					differs = true;
			}
			if (differs) {
				if (bad < 5)
					printf("  sampler %s %s differs\n", linear ? "linear" : "nearest", DescribeSamplerID(id).c_str());
				bad++;
			}
		}
	}
	samplerJitted = jitted;
	return jitted ? bad : -1;
}

static int TestPixel(BinManager &binner) {
	enum { W = 64, H = 4 };
	static u32 fbJit[W * H], fbCpp[W * H];
	static u16 zbJit[W * H], zbCpp[W * H];

	int bad = 0, jitted = 0;
	for (int n = 0; n < 2000; ) {
		/* A random GE state, and the ID the renderer would draw it with. */
		for (int c = 0; c < 256; c++)
			gstate.cmdmem[c] = ((u32)c << 24) | (Next() & 0xFFFFFF);
		gstate.cmdmem[GE_CMD_FRAMEBUFWIDTH] = (GE_CMD_FRAMEBUFWIDTH << 24) | W;
		gstate.cmdmem[GE_CMD_ZBUFWIDTH] = (GE_CMD_ZBUFWIDTH << 24) | W;
		PixelFuncID id;
		ComputePixelFuncID(&id);
		n++;

		g_Config.bSoftwareRenderingJit = true;
		Rasterizer::SingleFunc j = Rasterizer::GetSingleFunc(id, &binner);
		const Rasterizer::SingleFunc c = Rasterizer::PixelJitCache::GenericSingle(id);
		if (j == c)
			continue;
		jitted++;

		for (int i = 0; i < W * H; i++) {
			fbJit[i] = fbCpp[i] = Next();
			zbJit[i] = zbCpp[i] = (u16)Next();
		}
		for (int k = 0; k < 32; k++) {
			const int x = Next() % W, y = Next() % H, z = Next() & 0xFFFF, fog = Next() & 255;
			const Rasterizer::Vec4IntResult color = RandomColor();
			fb.as32 = fbJit;
			depthbuf.as16 = zbJit;
			j(x, y, z, fog, Rasterizer::ToVec4IntArg(color), id);
			fb.as32 = fbCpp;
			depthbuf.as16 = zbCpp;
			c(x, y, z, fog, Rasterizer::ToVec4IntArg(color), id);
		}
		if (memcmp(fbJit, fbCpp, sizeof(fbJit)) != 0 || memcmp(zbJit, zbCpp, sizeof(zbJit)) != 0) {
			if (bad < 5)
				printf("  pixel %s differs\n", DescribePixelFuncID(id).c_str());
			bad++;
		}
	}
	pixelJitted = jitted;
	return jitted ? bad : -1;
}

int main() {
	Rasterizer::Init();
	Sampler::Init();
	BinManager binner;

	const int sampler = TestSampler(binner);
	const int pixel = TestPixel(binner);
	printf("softjitabitest: sampler %d of %d differ, pixel %d of %d differ (-1: no jit here): %s\n",
		sampler, samplerJitted, pixel, pixelJitted, sampler > 0 || pixel > 0 ? "FAIL" : "ok");

	Sampler::Shutdown();
	Rasterizer::Shutdown();
	return sampler > 0 || pixel > 0 ? 1 : 0;
}
