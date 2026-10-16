// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include "ppsspp_config.h"
#include "Core/HW/VideoMemory.h"
#include "Core/MemMap.h"

#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
#include "Common/CommonWindows.h"
#endif

#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
retro_atomic_int_t g_videoMemoryPending;
#endif

namespace Memory {
bool ProtectVideoPages(uint32_t address, uint32_t size, bool protect);
bool VideoGuestAddress(uintptr_t hostAddress, uint32_t *guestAddress);
}

namespace {
bool Protect(void *, uint32_t address, uint32_t size, bool protect) {
	return Memory::ProtectVideoPages(address, size, protect);
}
VideoMemory pending(Protect, nullptr, 4096);
void PublishPending() {
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
	retro_atomic_store_release_int(&g_videoMemoryPending, pending.Count() != 0);
#endif
}

#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
PVOID exceptionHandler;
DWORD ownerThread;
thread_local bool handlingFault = false;

LONG NTAPI VideoAccess(PEXCEPTION_POINTERS exception) {
	if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
		|| exception->ExceptionRecord->NumberParameters < 2
		|| exception->ExceptionRecord->ExceptionInformation[0] == 8
		|| GetCurrentThreadId() != ownerThread || handlingFault)
		return EXCEPTION_CONTINUE_SEARCH;
	uint32_t guest;
	if (!pending.Count() || !Memory::VideoGuestAddress(exception->ExceptionRecord->ExceptionInformation[1], &guest)
		|| !pending.Contains(guest, 1))
		return EXCEPTION_CONTINUE_SEARCH;
	handlingFault = true;
	const bool resolved = pending.Resolve(guest, 1);
	PublishPending();
	handlingFault = false;
	return resolved ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#endif
}

bool CanDeferVideoMemory() {
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
	if (!Memory::IsActive()) return false;
	SYSTEM_INFO info{};
	GetSystemInfo(&info);
	if (info.dwPageSize != 4096) return false;
	if (!exceptionHandler) {
		ownerThread = GetCurrentThreadId();
		exceptionHandler = AddVectoredExceptionHandler(TRUE, VideoAccess);
	}
	return exceptionHandler && ownerThread == GetCurrentThreadId();
#else
	return false;
#endif
}

bool TrackVideoMemory(uint32_t address, uint32_t size, void *owner, VideoMemory::Materialize materialize) {
	const bool result = CanDeferVideoMemory() && pending.Track(address, size, owner, materialize);
	PublishPending();
	return result;
}
bool ResolveVideoMemory(uint32_t address, uint32_t size) { const bool result = pending.Resolve(address, size); PublishPending(); return result; }
bool ResolveAllVideoMemory() { const bool result = pending.ResolveAll(); PublishPending(); return result; }
bool DiscardVideoMemoryOwner(void *owner) { const bool result = pending.DiscardOwner(owner); PublishPending(); return result; }
bool ResolveVideoMemoryOwner(void *owner) { const bool result = pending.ResolveOwner(owner); PublishPending(); return result; }
bool PrepareVideoMemoryOverwrite(uint32_t address, uint32_t size) { const bool result = pending.PrepareOverwrite(address, size); PublishPending(); return result; }
bool HasVideoMemoryOwner(void *owner) { return pending.ContainsOwner(owner); }
bool HasVideoMemory(uint32_t address, uint32_t size) { return pending.Contains(address, size); }

void ResetVideoMemory() {
	pending.ForgetAll();
	PublishPending();
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
	if (exceptionHandler) RemoveVectoredExceptionHandler(exceptionHandler);
	exceptionHandler = nullptr;
	ownerThread = 0;
#endif
}
