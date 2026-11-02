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
#elif PPSSPP_VIDEO_MEMORY_FAULTS
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

#if PPSSPP_VIDEO_MEMORY_FAULTS
retro_atomic_int_t g_videoMemoryPending;
#endif
uint32_t g_videoMemoryPageSizeOverride;

namespace Memory {
bool ProtectVideoPages(uint32_t address, uint32_t size, bool protect);
bool VideoGuestAddress(uintptr_t hostAddress, uint32_t *guestAddress);
}

namespace {
bool Protect(void *, uint32_t address, uint32_t size, bool protect) {
	return Memory::ProtectVideoPages(address, size, protect);
}
VideoMemory pending(Protect, nullptr, 4096);

// Pages up to 64 KiB: 4 KiB on most systems, 16 KiB on Apple silicon and some Android devices.
// A bigger page only means an access next to a video buffer fills it sooner.
bool UsePageSize(uint32_t hostPageSize) {
	const uint32_t size = g_videoMemoryPageSizeOverride ? g_videoMemoryPageSizeOverride : hostPageSize;
	if (size < 4096 || size > 65536 || (size & (size - 1)) || size % hostPageSize)
		return false;
	return pending.PageSize() == size || pending.SetPageSize(size);
}
void PublishPending() {
#if PPSSPP_VIDEO_MEMORY_FAULTS
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
#elif PPSSPP_VIDEO_MEMORY_FAULTS
// The handler goes in front of whatever was there (the JIT's fault handler, say), and hands
// anything that isn't a pending video page on to it. It runs on the faulting thread's own
// stack, not an alternate one: filling the pages can go deep into the GPU driver.
bool installed = false;
pthread_t ownerThread;
volatile sig_atomic_t handlingFault = 0;
volatile sig_atomic_t chaining = 0;
struct sigaction oldSegv, oldBus;

void VideoAccess(int sig, siginfo_t *info, void *context);

bool Current(int sig) {
	struct sigaction current{};
	return sigaction(sig, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) && current.sa_sigaction == &VideoAccess;
}

void Chain(int sig, siginfo_t *info, void *context) {
	const struct sigaction &old = sig == SIGBUS ? oldBus : oldSegv;
	// Reached again from the handler it hands on to (one that kept an earlier install of this
	// one as its own previous): give the fault the default action instead of going round.
	if (chaining) {
		signal(sig, SIG_DFL);
		return;
	}
	chaining = 1;
	if (old.sa_flags & SA_SIGINFO) {
		old.sa_sigaction(sig, info, context);
	} else if (old.sa_handler == SIG_DFL) {
		// Returning re-runs the access, which now gets the default action.
		signal(sig, SIG_DFL);
	} else if (old.sa_handler != SIG_IGN) {
		old.sa_handler(sig);
	}
	chaining = 0;
}

void VideoAccess(int sig, siginfo_t *info, void *context) {
	// A fault, not a signal sent with kill(). Darwin reports some PROT_NONE accesses as SIGBUS.
	const bool access = info->si_code > 0;
	uint32_t guest;
	if (access && !handlingFault && pthread_equal(pthread_self(), ownerThread) && pending.Count()
		&& Memory::VideoGuestAddress((uintptr_t)info->si_addr, &guest) && pending.Contains(guest, 1)) {
		handlingFault = 1;
		const bool resolved = pending.Resolve(guest, 1);
		PublishPending();
		handlingFault = 0;
		if (resolved)
			return;
	}
	Chain(sig, info, context);
}
#endif
}

bool CanDeferVideoMemory() {
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
	if (!Memory::IsActive()) return false;
	SYSTEM_INFO info{};
	GetSystemInfo(&info);
	if (!UsePageSize(info.dwPageSize)) return false;
	if (!exceptionHandler) {
		ownerThread = GetCurrentThreadId();
		exceptionHandler = AddVectoredExceptionHandler(TRUE, VideoAccess);
	}
	return exceptionHandler && ownerThread == GetCurrentThreadId();
#elif PPSSPP_VIDEO_MEMORY_FAULTS
	if (!Memory::IsActive() || !UsePageSize((uint32_t)sysconf(_SC_PAGESIZE))) return false;
	// Again if something went in front of it, or took it out, since.
	if (!installed || !Current(SIGSEGV)) {
		struct sigaction sa{};
		sa.sa_sigaction = &VideoAccess;
		// A fault while filling pages reaches the handler again, which hands it on.
		sa.sa_flags = SA_SIGINFO | SA_NODEFER;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGSEGV, &sa, &oldSegv) != 0) return false;
#ifdef __APPLE__
		if (sigaction(SIGBUS, &sa, &oldBus) != 0) {
			sigaction(SIGSEGV, &oldSegv, nullptr);
			return false;
		}
#endif
		ownerThread = pthread_self();
		installed = true;
	}
	return pthread_equal(ownerThread, pthread_self()) != 0;
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
#elif PPSSPP_VIDEO_MEMORY_FAULTS
	if (installed) {
		// Only if nothing went in front of it since; otherwise it stays, with nothing pending.
		if (Current(SIGSEGV))
			sigaction(SIGSEGV, &oldSegv, nullptr);
#ifdef __APPLE__
		if (Current(SIGBUS))
			sigaction(SIGBUS, &oldBus, nullptr);
#endif
		installed = false;
	}
#endif
}
