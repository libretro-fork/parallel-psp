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

#include "Core/HW/VideoMemory.h"

#include <limits>
#include <algorithm>

VideoMemory::VideoMemory(Protect protect, void *context, uint32_t pageSize)
	: protect_(protect), context_(context), pageSize_(pageSize) {}

uint32_t VideoMemory::CanonicalAddress(uint32_t address) {
	address &= 0x3FFFFFFF;
	if ((address & 0x3F800000) == 0x04000000)
		address &= 0x041FFFFF;
	return address;
}

// VRAM mirrors wrap every 2 MiB, including ranges crossing a mirror boundary.
static bool OverlapsGuestRange(uint32_t address, uint32_t size, uint32_t other, uint32_t otherSize) {
	auto overlap = [](uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
		return n && m && (uint64_t)a < (uint64_t)b + m && (uint64_t)b < (uint64_t)a + n;
	};
	if (address >= 0x04000000 && address < 0x04200000) {
		const uint32_t first = std::min(size, 0x04200000 - address);
		if (overlap(address, first, other, otherSize)) return true;
		return overlap(0x04000000, std::min(size - first, 0x00200000U), other, otherSize);
	}
	return overlap(address, size, other, otherSize);
}

bool VideoMemory::Track(uint32_t address, uint32_t size, void *owner, Materialize materialize) {
	address = CanonicalAddress(address);
	if (!size || !owner || !materialize || !protect_ || !pageSize_ || (pageSize_ & (pageSize_ - 1)))
		return false;
	const uint64_t end = (uint64_t)address + size;
	const uint64_t pageEnd = (end + pageSize_ - 1) & ~(uint64_t)(pageSize_ - 1);
	if (end > std::numeric_limits<uint32_t>::max() || pageEnd > std::numeric_limits<uint32_t>::max())
		return false;
	const uint32_t pageAddress = address & ~(pageSize_ - 1);
	const uint32_t pageBytes = (uint32_t)pageEnd - pageAddress;
	// Shared pages cannot have independent protection lifetimes.
	for (size_t i = 0; i < count_;) {
		if (entries_[i].owner != owner && OverlapsGuestRange(pageAddress, pageBytes, entries_[i].pageAddress, entries_[i].pageSize)) {
			if (!ResolveOwner(entries_[i].owner)) return false;
			i = 0;
		} else ++i;
	}
	if (count_ == entries_.size() && !ResolveOwner(entries_[0].owner))
		return false;
	if (!protect_(context_, pageAddress, pageBytes, true))
		return false;
	entries_[count_++] = {address, size, pageAddress, pageBytes, owner, materialize};
	return true;
}

bool VideoMemory::Remove(size_t index, bool materialize) {
	const Entry entry = entries_[index];
	if (!protect_(context_, entry.pageAddress, entry.pageSize, false))
		return false;
	entries_[index] = entries_[--count_];
	if (materialize && !entry.materialize(entry.owner)) {
		// Failed materialization must never make stale RAM available to the CPU.
		entries_[count_++] = entry;
		protect_(context_, entry.pageAddress, entry.pageSize, true);
		return false;
	}
	return true;
}

bool VideoMemory::Resolve(uint32_t address, uint32_t size) {
	address = CanonicalAddress(address);
	for (size_t i = 0; i < count_;) {
		if (OverlapsGuestRange(address, size, entries_[i].pageAddress, entries_[i].pageSize)) {
			if (!ResolveOwner(entries_[i].owner))
				return false;
			i = 0;
		} else {
			++i;
		}
	}
	return true;
}

bool VideoMemory::ResolveAll() {
	while (count_)
		if (!ResolveOwner(entries_[count_ - 1].owner))
			return false;
	return true;
}

bool VideoMemory::DiscardOwner(void *owner) {
	for (size_t i = 0; i < count_;) {
		if (entries_[i].owner == owner) {
			if (!Remove(i, false)) return false;
		} else ++i;
	}
	return true;
}

bool VideoMemory::ResolveOwner(void *owner) {
	// All components of a tiled frame must be writable before its callback runs.
	std::array<Entry, 64> removed;
	size_t count = 0;
	for (size_t i = 0; i < count_;) {
		if (entries_[i].owner == owner) {
			if (!protect_(context_, entries_[i].pageAddress, entries_[i].pageSize, false)) {
				for (size_t j = 0; j < count; ++j) {
					entries_[count_++] = removed[j];
					protect_(context_, removed[j].pageAddress, removed[j].pageSize, true);
				}
				return false;
			}
			removed[count++] = entries_[i];
			entries_[i] = entries_[--count_];
		} else ++i;
	}
	if (!count || removed[0].materialize(owner)) return true;
	for (size_t i = 0; i < count; ++i) {
		entries_[count_++] = removed[i];
		protect_(context_, removed[i].pageAddress, removed[i].pageSize, true);
	}
	return false;
}

bool VideoMemory::PrepareOverwrite(uint32_t address, uint32_t size) {
	address = CanonicalAddress(address);
	for (size_t i = 0; i < count_;) {
		const Entry &entry = entries_[i];
		if (!OverlapsGuestRange(address, size, entry.pageAddress, entry.pageSize)) {
			++i;
			continue;
		}
		const bool wrapped = address >= 0x04000000 && address < 0x04200000 && (uint64_t)address + size > 0x04200000;
		void *owner = entry.owner;
		bool covered = !wrapped;
		for (size_t j = 0; j < count_; ++j) {
			if (entries_[j].owner == owner && !(address <= entries_[j].address && (uint64_t)address + size >= (uint64_t)entries_[j].address + entries_[j].size)) covered = false;
		}
		if (!(covered ? DiscardOwner(owner) : ResolveOwner(owner))) return false;
		i = 0;
	}
	return true;
}

void VideoMemory::ForgetAll() {
	while (count_) {
		const Entry &entry = entries_[--count_];
		protect_(context_, entry.pageAddress, entry.pageSize, false);
	}
}

bool VideoMemory::Contains(uint32_t address, uint32_t size) const {
	address = CanonicalAddress(address);
	for (size_t i = 0; i < count_; ++i)
		if (OverlapsGuestRange(address, size, entries_[i].pageAddress, entries_[i].pageSize))
			return true;
	return false;
}

bool VideoMemory::ContainsOwner(void *owner) const {
	for (size_t i = 0; i < count_; ++i) if (entries_[i].owner == owner) return true;
	return false;
}
