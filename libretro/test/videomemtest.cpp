/* unittest/TestVideoMemoryHost.cpp on the core's own build: guest pages left to the GPU are
 * filled on their first access through every alias and mirror, by generated code's mapping
 * or a native load, and on Windows, Linux and macOS alike. */
#include <cstdio>

#include "Common/ExceptionHandlerSetup.h"

bool TestVideoMemoryHost();

static int jitFaults;
static bool JitFault(uintptr_t address, void *context) {
	jitFaults++;
	return false;
}

int main() {
	bool ok = TestVideoMemoryHost();
	/* Again behind the JIT's fault handler, as in a running game: the video pages are still
	 * filled, and none of their faults reach it. */
	InstallExceptionHandler(&JitFault);
	ok = ok && TestVideoMemoryHost() && jitFaults == 0;
	UninstallExceptionHandler();
	printf("videomemtest: %s\n", ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
