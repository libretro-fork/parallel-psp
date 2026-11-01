// Memory block info maps against a per-address model: random notifications,
// savestate round trips, and no heap traffic once the maps have warmed up.
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

static const uint32_t BASE = 0x08800000;
static const uint32_t UNIT = 0x10;
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

int main() {
	MemBlockInfoInit();
	MemBlockOverrideDetailed();

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
	printf("mbitest: model, savestate and steady-state checks pass\n");
	return 0;
}
