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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// One CPU-thread owner. Protection covers every alias, including page padding.
class VideoMemory {
public:
	using Protect = bool (*)(void *, uint32_t, uint32_t, bool);
	using Materialize = bool (*)(void *);

	VideoMemory(Protect protect, void *context, uint32_t pageSize);
	bool Track(uint32_t address, uint32_t size, void *owner, Materialize materialize);
	bool Resolve(uint32_t address, uint32_t size);
	bool ResolveAll();
	bool ResolveOwner(void *owner);
	bool DiscardOwner(void *owner);
	// Only a full overwrite can discard old pixels; partial writes resolve first.
	bool PrepareOverwrite(uint32_t address, uint32_t size);
	void ForgetAll();
	bool Contains(uint32_t address, uint32_t size) const;
	bool ContainsOwner(void *owner) const;
	size_t Count() const { return count_; }
	static uint32_t CanonicalAddress(uint32_t address);

private:
	struct Entry {
		uint32_t address, size, pageAddress, pageSize;
		void *owner;
		Materialize materialize;
	};
	bool Remove(size_t index, bool materialize);
	std::array<Entry, 64> entries_{};
	size_t count_ = 0;
	Protect protect_;
	void *context_;
	uint32_t pageSize_;
};

bool CanDeferVideoMemory();
bool TrackVideoMemory(uint32_t address, uint32_t size, void *owner, VideoMemory::Materialize materialize);
bool ResolveVideoMemory(uint32_t address, uint32_t size);
bool ResolveAllVideoMemory();
bool ResolveVideoMemoryOwner(void *owner);
bool DiscardVideoMemoryOwner(void *owner);
bool PrepareVideoMemoryOverwrite(uint32_t address, uint32_t size);
bool HasVideoMemory(uint32_t address, uint32_t size);
bool HasVideoMemoryOwner(void *owner);
void ResetVideoMemory();
