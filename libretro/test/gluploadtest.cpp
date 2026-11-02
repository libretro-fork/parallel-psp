/* The GL backend's per-frame upload arena: aligned, separate allocations within a frame, the
 * same memory again after the reset with nothing allocated in a steady state, and no more than
 * the cap kept after a frame that uploaded far more. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Common/GPU/OpenGL/GLFrameData.h"

static int failures;
static void Check(bool ok, const char *what) {
	if (!ok) {
		printf("  failed: %s\n", what);
		failures++;
	}
}

/* One frame's uploads: sizes of texture levels, each filled and checked after the others. */
static std::vector<uint8_t *> Frame(GLUploadArena &arena, const std::vector<size_t> &sizes) {
	std::vector<uint8_t *> ptrs;
	for (size_t i = 0; i < sizes.size(); i++) {
		uint8_t *p = arena.Allocate(sizes[i]);
		Check(p != nullptr, "allocation");
		Check(((uintptr_t)p & 15) == 0, "16-byte alignment");
		if (p)
			memset(p, (int)(i + 1), sizes[i]);
		ptrs.push_back(p);
	}
	for (size_t i = 0; i < sizes.size(); i++) {
		bool intact = ptrs[i] != nullptr;
		for (size_t j = 0; intact && j < sizes[i]; j += 97)
			intact = ptrs[i][j] == (uint8_t)(i + 1);
		Check(intact, "allocations kept apart");
	}
	return ptrs;
}

int main() {
	GLUploadArena arena;
	/* A frame of a dozen levels, a 512x512 8888 one among them. */
	const std::vector<size_t> typical = { 1024 * 1024, 65536, 16384, 4096, 1024, 256, 64, 16, 1, 3, 4096 * 5, 300000 };
	const std::vector<uint8_t *> first = Frame(arena, typical);
	const size_t retained = arena.Retained();
	for (int frame = 0; frame < 100; frame++) {
		arena.Reset();
		const std::vector<uint8_t *> again = Frame(arena, typical);
		Check(again == first, "the same memory each frame");
		Check(arena.Retained() == retained, "nothing allocated in a steady state");
	}

	/* A frame of 40 MiB of uploads spills into more chunks, and keeps no more than the cap. */
	arena.Reset();
	std::vector<size_t> huge(40, 1024 * 1024);
	huge.push_back(9 * 1024 * 1024);
	Frame(arena, huge);
	arena.Reset();
	Check(arena.Retained() <= 16 * 1024 * 1024, "at most the cap kept after a big frame");
	Frame(arena, typical);
	const size_t afterBig = arena.Retained();
	arena.Reset();
	Frame(arena, typical);
	Check(arena.Retained() == afterBig, "steady again after a big frame");

	printf("gluploadtest: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
