// Memory block info maps against a per-address model: random notifications at several scales,
// savestate round trips, index work in proportion to the marks, and no heap traffic once the
// maps have warmed up. MBITEST_BENCH=1 times notifications instead.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "Common/Serialize/Serializer.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/MIPS/MIPS.h"

static unsigned long long g_newCount;

void *operator new(size_t n) {
	++g_newCount;
	void *p = malloc(n ? n : 1);
	if (!p)
		throw std::bad_alloc();
	return p;
}
void *operator new[](size_t n) {
	++g_newCount;
	void *p = malloc(n ? n : 1);
	if (!p)
		throw std::bad_alloc();
	return p;
}
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

void FlushPendingMemInfo();

// The model covers UNITS units from BASE. Small units keep everything within one slice of the
// maps' index, large ones spread marks over many slices, cut at points between slice starts.
static uint32_t BASE = 0x08800000;
static uint32_t UNIT = 0x10;
static const uint32_t UNITS = 512;

struct Cell {
	bool allocated = false;
	uint32_t pc = 0;
	std::string tag;
};

enum { MAP_ALLOC, MAP_SUB, MAP_WRITE, MAP_TEX, MAPS };
static Cell model[MAPS][UNITS];
static const MemBlockFlags mapFlag[MAPS] = { MemBlockFlags::ALLOC, MemBlockFlags::SUB_ALLOC, MemBlockFlags::WRITE, MemBlockFlags::TEXTURE };
static uint32_t rng = 12345;
static int failures;

static uint32_t Next() {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void MarkModel(int m, uint32_t u0, uint32_t n, uint32_t pc, bool allocated, const char *tag) {
	for (uint32_t u = u0; u < u0 + n; ++u) {
		Cell &c = model[m][u];
		c.allocated = allocated;
		if (pc != 0)
			c.pc = pc;
		if (tag)
			c.tag = tag;
	}
}

// What FormatMemWriteTagAt gives for a unit before the copy lands.
static std::string CopyTag(const char *prefix, uint32_t u0, uint32_t n) {
	for (uint32_t u = u0; u < u0 + n; ++u) {
		const Cell &c = model[MAP_WRITE][u];
		if (c.pc != 0 || !c.tag.empty()) {
			if (c.tag != "MemInit")
				return std::string(prefix) + c.tag;
			break;
		}
	}
	for (int m : { MAP_ALLOC, MAP_TEX }) {
		for (uint32_t u = u0; u < u0 + n; ++u) {
			const Cell &c = model[m][u];
			if (c.pc != 0 || !c.tag.empty())
				return std::string(prefix) + c.tag;
		}
	}
	char buf[64];
	snprintf(buf, sizeof(buf), "%s%08x_size_%08x", prefix, BASE + u0 * UNIT, n * UNIT);
	return buf;
}

static void Op(int i) {
	static const char *tags[] = { "Tex", "Vtx", "MemInit", "Stack", "A", "B" };
	uint32_t u0 = Next() % UNITS;
	uint32_t n = 1 + Next() % 24;
	if (u0 + n > UNITS)
		n = UNITS - u0;
	uint32_t pc = (Next() & 3) ? 0x08804000 + (Next() % 4) * 4 : 0;
	const char *tag = tags[Next() % 6];
	currentMIPS->pc = pc;
	uint32_t kind = Next() % 7;
	uint32_t addr = BASE + u0 * UNIT;
	if (kind == 6) {
		// Copies take their tag from what the source held when they flush.
		uint32_t s0 = Next() % (UNITS - n + 1);
		std::string t = CopyTag("Copy/", s0, n);
		NotifyMemInfoCopy(addr, BASE + s0 * UNIT, n * UNIT, "Copy/");
		FlushPendingMemInfo();
		MarkModel(MAP_WRITE, u0, n, pc, true, t.c_str());
		return;
	}
	MemBlockFlags f = MemBlockFlags::SKIP_MEMCHECK;
	switch (kind) {
	case 0: f |= MemBlockFlags::ALLOC; MarkModel(MAP_ALLOC, u0, n, pc, true, tag); break;
	case 1: f |= MemBlockFlags::FREE; MarkModel(MAP_ALLOC, u0, n, 0, false, nullptr); MarkModel(MAP_SUB, u0, n, 0, false, nullptr); break;
	case 2: f |= MemBlockFlags::SUB_ALLOC; MarkModel(MAP_SUB, u0, n, pc, true, tag); break;
	case 3: f |= MemBlockFlags::SUB_FREE; MarkModel(MAP_SUB, u0, n, 0, false, nullptr); break;
	case 4: f |= MemBlockFlags::TEXTURE; MarkModel(MAP_TEX, u0, n, pc, true, tag); break;
	default: f |= MemBlockFlags::WRITE; MarkModel(MAP_WRITE, u0, n, pc, true, tag); break;
	}
	NotifyMemInfoPC(f, addr, n * UNIT, pc, tag, strlen(tag));
	// Batches of notifications flush together, like a frame does.
	if ((i & 7) == 7)
		FlushPendingMemInfo();
}

static void Check(const char *when) {
	for (int m = 0; m < MAPS; ++m) {
		for (uint32_t u = 0; u < UNITS; ++u) {
			std::vector<MemBlockInfo> r = FindMemInfoByFlag(mapFlag[m], BASE + u * UNIT, 1);
			const Cell &c = model[m][u];
			bool want = c.pc != 0 || !c.tag.empty();
			bool ok = r.size() == (want ? 1u : 0u);
			if (ok && want) {
				const MemBlockInfo &b = r[0];
				ok = b.allocated == c.allocated && b.pc == c.pc && b.tag == c.tag &&
					b.start <= BASE + u * UNIT && b.start + b.size > BASE + u * UNIT;
			}
			if (!ok) {
				if (failures++ < 5)
					printf("FAIL %s: map %d at %08x: got %d results%s%s, want pc=%08x tag=%s allocated=%d\n", when, m, BASE + u * UNIT,
						(int)r.size(), r.empty() ? "" : " tag=", r.empty() ? "" : r[0].tag.c_str(), c.pc, c.tag.c_str(), c.allocated);
				return;
			}
		}
	}
}

static void RoundTrip() {
	FlushPendingMemInfo();
	std::vector<u8> buf;
	u8 *ptr = nullptr;
	PointerWrap measure(&ptr, PointerWrap::MODE_MEASURE);
	MemBlockInfoDoState(measure);
	buf.resize(measure.Offset());
	ptr = buf.data();
	PointerWrap save(&ptr, PointerWrap::MODE_WRITE);
	MemBlockInfoDoState(save);
	ptr = buf.data();
	PointerWrap load(&ptr, PointerWrap::MODE_READ);
	MemBlockInfoDoState(load);
	if (load.Failed()) {
		printf("FAIL: savestate round trip\n");
		++failures;
	}
}

// MBITEST_BENCH=1: the cost of notifications the way a game or a GE dump replay makes them -
// writes of a few hundred bytes to a few hundred kB anywhere in user RAM, a handful of tags.
static void Bench() {
	static const char *tags[] = { "ReplayTex", "ReplayMemcpy", "GPUBlockTransfer/x", "IoRead", "Memcpy" };
	MemBlockInfoInit();
	const int marks = 200000;
	uint32_t r = 777;
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < marks; ++i) {
		r ^= r << 13; r ^= r >> 17; r ^= r << 5;
		const uint32_t size = 0x100u << (r % 11);
		const uint32_t addr = 0x08800000 + ((r >> 8) % (0x01800000 - size)) & ~0xFFu;
		const char *tag = tags[(r >> 4) % 5];
		NotifyMemInfoPC(MemBlockFlags::WRITE | MemBlockFlags::SKIP_MEMCHECK, addr, size, 0x08804000, tag, strlen(tag));
		if ((i & 63) == 63)
			FlushPendingMemInfo();
	}
	FlushPendingMemInfo();
	double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	printf("mbitest bench: scattered, %d marks, %.1f ns each\n", marks, s * 1e9 / marks);
	MemBlockInfoShutdown();

	// A replay's frame: the same buffers written again every frame, by the same code.
	MemBlockInfoInit();
	t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < marks; ++i) {
		const uint32_t addr = 0x08800000 + (uint32_t)(i % 64) * 0x20000;
		NotifyMemInfoPC(MemBlockFlags::WRITE | MemBlockFlags::SKIP_MEMCHECK, addr + 0x1000, 0x8000, 0x08804000, "ReplayTex", 9);
		if ((i & 63) == 63)
			FlushPendingMemInfo();
	}
	FlushPendingMemInfo();
	s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	printf("mbitest bench: repeated, %d marks, %.1f ns each\n", marks, s * 1e9 / marks);
	MemBlockInfoShutdown();

	// Allocations made and freed again at the top of what has been used: each one is cut out
	// of the unmarked rest of the address space and merged back into it.
	MemBlockInfoInit();
	NotifyMemInfoPC(MemBlockFlags::ALLOC | MemBlockFlags::SKIP_MEMCHECK, 0x08800000, 0x00400000, 0x08804000, "Game", 4);
	t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < marks; ++i) {
		const uint32_t addr = 0x08C00000 + (uint32_t)(i % 16) * 0x10000;
		NotifyMemInfoPC(MemBlockFlags::ALLOC | MemBlockFlags::SKIP_MEMCHECK, addr, 0x10000, 0x08804000, "Slab", 4);
		NotifyMemInfoPC(MemBlockFlags::FREE | MemBlockFlags::SKIP_MEMCHECK, addr, 0x10000, 0x08804000, "", 0);
		if ((i & 31) == 31)
			FlushPendingMemInfo();
	}
	FlushPendingMemInfo();
	s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	printf("mbitest bench: alloc/free at the top, %d pairs, %.1f ns each\n", marks, s * 1e9 / marks);
	MemBlockInfoShutdown();
}

int main() {
	if (getenv("MBITEST_BENCH")) {
		Bench();
		return 0;
	}
	MemBlockOverrideDetailed();

	static const uint32_t scales[][2] = {
		{ 0x08800000, 0x10 },
		{ 0x08802a00, 0x1300 },
		{ 0x00010000, 0x10000 },
		{ 0x30000000, 0x80000 },
	};
	for (const auto &scale : scales) {
		BASE = scale[0];
		UNIT = scale[1];
		MemBlockInfoInit();
		for (int m = 0; m < MAPS; ++m)
			for (uint32_t u = 0; u < UNITS; ++u)
				model[m][u] = Cell();
		for (int round = 0; round < 40 && failures == 0; ++round) {
			for (int i = 0; i < 400; ++i)
				Op(i);
			FlushPendingMemInfo();
			Check("ops");
			// Loading rebuilds the maps from one array per map; work continues on top.
			if (round % 4 == 3) {
				RoundTrip();
				Check("load");
			}
		}
		MemBlockInfoShutdown();
	}

	// Marks cost the index what they cover, not what the slabs around them span: the first slab
	// spans the whole address space, so allocations at the top of what is in use, freed again,
	// used to rewrite the index for everything above them, twice each.
	MemBlockInfoInit();
	NotifyMemInfoPC(MemBlockFlags::ALLOC | MemBlockFlags::SKIP_MEMCHECK, 0x08800000, 0x00400000, 0x08804000, "Game", 4);
	NotifyMemInfoPC(MemBlockFlags::WRITE | MemBlockFlags::SKIP_MEMCHECK, 0x08800000, 0x00400000, 0x08804000, "Load", 4);
	FlushPendingMemInfo();
	const uint64_t indexBefore = MemBlockInfoIndexUpdates();
	for (int i = 0; i < 1000; ++i) {
		const uint32_t addr = 0x08C00000 + (uint32_t)(i % 16) * 0x10000;
		NotifyMemInfoPC(MemBlockFlags::ALLOC | MemBlockFlags::SKIP_MEMCHECK, addr, 0x10000, 0x08804000, "Slab", 4);
		NotifyMemInfoPC(MemBlockFlags::WRITE | MemBlockFlags::SKIP_MEMCHECK, addr + 0x100, 0x400, 0x08804100, "Fill", 4);
		NotifyMemInfoPC(MemBlockFlags::FREE | MemBlockFlags::SKIP_MEMCHECK, addr, 0x10000, 0x08804000, "", 0);
		// The same buffer written by the same code every frame.
		NotifyMemInfoPC(MemBlockFlags::WRITE | MemBlockFlags::SKIP_MEMCHECK, 0x08900000, 0x800, 0x08804200, "Load", 4);
		FlushPendingMemInfo();
	}
	const uint64_t indexWork = MemBlockInfoIndexUpdates() - indexBefore;
	if (getenv("MBITEST_VERBOSE"))
		printf("index updates: %llu\n", (unsigned long long)indexWork);
	// A 64 KiB block is four 16 KiB slices. Freed blocks merge, so allocating one again cuts it
	// out of up to fifteen others: around 40 a round. It was hundreds of thousands.
	if (indexWork > 1000 * 100) {
		printf("FAIL: %llu index updates for 1000 small allocations\n", (unsigned long long)indexWork);
		++failures;
	}
	{
		std::vector<MemBlockInfo> r = FindMemInfoByFlag(MemBlockFlags::ALLOC, 0x08C00000, 0x100000);
		for (const MemBlockInfo &b : r) {
			if (b.allocated) {
				printf("FAIL: freed block %08x still allocated\n", b.start);
				++failures;
			}
		}
		r = FindMemInfoByFlag(MemBlockFlags::WRITE, 0x08900000, 0x800);
		if (r.size() != 1 || r[0].tag != "Load" || r[0].pc != 0x08804000 && r[0].pc != 0x08804200) {
			printf("FAIL: rewritten buffer lost its tag\n");
			++failures;
		}
	}
	MemBlockInfoShutdown();

	MemBlockInfoInit();

	// A repeating pattern of marks splits and merges slabs every pass. Once the
	// maps have seen it, it must not touch the heap.
	unsigned long long steady = 0;
	for (int pass = 0; pass < 64; ++pass) {
		if (pass == 8)
			steady = g_newCount;
		for (uint32_t k = 0; k < 32; ++k) {
			uint32_t addr = BASE + (k * 13 % UNITS) * UNIT;
			NotifyMemInfoPC(MemBlockFlags::WRITE | MemBlockFlags::SKIP_MEMCHECK, addr, UNIT * 3, 0x08804000 + (pass & 1) * 4, "Frame", 5);
			NotifyMemInfoPC(MemBlockFlags::WRITE | MemBlockFlags::SKIP_MEMCHECK, addr + UNIT, UNIT, 0x08804000, "Frame", 5);
		}
		FlushPendingMemInfo();
	}
	unsigned long long churn = g_newCount - steady;
	if (churn != 0) {
		printf("FAIL: %llu heap allocations in the steady state\n", churn);
		++failures;
	}

	MemBlockReleaseDetailed();
	MemBlockInfoShutdown();
	if (failures)
		return 1;
	printf("mbitest: model at four scales, savestate, index cost and steady-state checks pass\n");
	return 0;
}
