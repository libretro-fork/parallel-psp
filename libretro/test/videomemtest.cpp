/* unittest/TestVideoMemoryHost.cpp on the core's own build: guest pages left to the GPU are
 * filled on their first access through every alias and mirror, by generated code's mapping
 * or a native load, and on Windows, Linux and macOS alike, with pages up to 64 KiB. */
#include <cstdint>
#include <cstdio>

#include "Common/ExceptionHandlerSetup.h"
#include "Core/CoreParameter.h"
#include "Core/HW/VideoMemory.h"
#include "Core/MemMap.h"
#include "Core/System.h"

bool TestVideoMemoryHost();

static int jitFaults;
static bool JitFault(uintptr_t address, void *context) {
	jitFaults++;
	return false;
}

static int fills;
static bool Fill(void *owner) {
	fills++;
	return true;
}

/* A load 12 KiB past a tracked buffer, in its 16 KiB page but not its 4 KiB one, fills it only
 * when the protection is that coarse. */
static bool Granularity(uint32_t pageSize, bool expectFill) {
	g_videoMemoryPageSizeOverride = pageSize;
	Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE;
	PSP_CoreParameter().cpuCore = CPUCore::INTERPRETER;
	if (!Memory::Init(Memory::MemMapSetupFlags::Default))
		return false;
	bool ok = true;
	if (CanDeferVideoMemory()) {
		static int owner;
		fills = 0;
		ok = TrackVideoMemory(0x08810080, 256, &owner, Fill);
		volatile uint32_t value = *(volatile const uint32_t *)(Memory::base + 0x08813000);
		(void)value;
		ok = ok && (fills == 1) == expectFill;
		ResolveAllVideoMemory();
	}
	Memory::Shutdown();
	g_videoMemoryPageSizeOverride = 0;
	return ok;
}

int main() {
	bool ok = TestVideoMemoryHost();
	/* Again behind the JIT's fault handler, as in a running game: the video pages are still
	 * filled, and none of their faults reach it. */
	InstallExceptionHandler(&JitFault);
	ok = ok && TestVideoMemoryHost() && jitFaults == 0;
	UninstallExceptionHandler();
	/* Protected in 16 KiB and 64 KiB pages, as on Apple silicon and some Android devices. */
	for (uint32_t pageSize : { 16384u, 65536u }) {
		g_videoMemoryPageSizeOverride = pageSize;
		ok = ok && TestVideoMemoryHost();
	}
	g_videoMemoryPageSizeOverride = 0;
	ok = ok && Granularity(4096, false) && Granularity(16384, true);
	printf("videomemtest: %s\n", ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
