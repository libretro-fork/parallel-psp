// Copyright (c) 2021- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cstring>

#include <retro_atomic.h>

#include "Common/Log.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Data/Text/StringWriter.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Breakpoints.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/HLE/sceKernelModule.h"  // for DescribeAddress
#include "Core/MIPS/MIPS.h"
#include "Common/StringUtils.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/HW/Display.h"
#include "Common/Thread/MpscQueue.h"

class MemSlabMap {
public:
	MemSlabMap();
	~MemSlabMap();

	bool Mark(uint32_t addr, uint32_t size, uint64_t ticks, uint32_t pc, bool allocated, const char *tag, size_t tagLen);
	bool Find(MemBlockFlags flags, uint32_t addr, uint32_t size, std::vector<MemBlockInfo> &results);
	// Note that the returned pointer gets invalidated as soon as Mark is called.
	const char *FastFindWriteTag(MemBlockFlags flags, uint32_t addr, uint32_t size, size_t *tagLen);
	void Reset();
	void DoState(PointerWrap &p);

private:
	struct Slab {
		uint32_t start = 0;
		uint32_t end = 0;
		uint64_t ticks = 0;
		uint32_t pc = 0;
		bool allocated = false;
		// Intentionally not save stated.
		bool bulkStorage = false;
		uint8_t tagLen;
		char tag[128]{};
		Slab *prev = nullptr;
		Slab *next = nullptr;

		void DoState(PointerWrap &p);
	};

	static constexpr uint32_t MAX_SIZE = 0x40000000;
	static constexpr uint32_t SLICES = 65536;
	static constexpr uint32_t SLICE_SIZE = MAX_SIZE / SLICES;

	Slab *FindSlab(uint32_t addr);
	void Clear();
	// Cuts slab at size: slab becomes the part before, and the part after is returned. Either
	// may be the new slab - whichever is shorter, as that is the part the index is updated for.
	Slab *Split(Slab *&slab, uint32_t size);
	void MergeAdjacent(Slab *slab);
	static inline bool Same(const Slab *a, const Slab *b);
	// Whether marking within slab would leave it as it is, apart from the ticks.
	static bool Unchanged(const Slab *slab, uint32_t pc, bool allocated, const char *tag, size_t tagLen);
	// Returns the slab that remains, the longer one.
	Slab *Merge(Slab *a, Slab *b);
	void FillHeads(Slab *slab);
	void SetHeads(Slab *slab, uint32_t start, uint32_t end);
	Slab *NewSlab();
	void FreeSlab(Slab *slab);
	void FreeSpares();

	Slab *first_ = nullptr;
	// Slabs dropped by Merge, linked through next and reused by Split.
	Slab *spare_ = nullptr;
	Slab *lastFind_ = nullptr;
	std::vector<Slab *> heads_;
	Slab *bulkStorage_ = nullptr;
};

struct PendingNotifyMem {
	MemBlockFlags flags;
	uint32_t start;
	uint32_t size;
	uint32_t copySrc;
	uint64_t ticks;
	uint32_t pc;
	uint8_t tagLen;
	char tag[127];
};

// The slab maps belong to the emulation thread. Notifications come from any thread (the CPU
// thread, software rasterizer and SAS workers), so they queue up in pendingNotifies, which the
// emulation thread drains before every lookup, on each flip, and for savestates.
static MemSlabMap allocMap;
static MemSlabMap suballocMap;
static MemSlabMap writeMap;
static MemSlabMap textureMap;
static MpscQueue<PendingNotifyMem> pendingNotifies;
static uint64_t indexUpdates;
static retro_atomic_int_t detailedOverride{ 0 };

MemSlabMap::MemSlabMap() {
	Reset();
}

MemSlabMap::~MemSlabMap() {
	Clear();
}

bool MemSlabMap::Mark(uint32_t addr, uint32_t size, uint64_t ticks, uint32_t pc, bool allocated, const char *tag, size_t tagLen) {
	uint32_t end = addr + size;
	Slab *slab = FindSlab(addr);
	if (slab != nullptr && slab->end >= end && Unchanged(slab, pc, allocated, tag, tagLen)) {
		// Within one slab that already says this: cutting the range out would only merge it back
		// again, leaving the newer of the ticks. Games write the same buffers from the same code
		// over and over, and frees mostly land on what is already free.
		if (pc != 0)
			slab->ticks = slab->start == addr && slab->end == end ? ticks : std::max(slab->ticks, ticks);
		return true;
	}
	Slab *firstMatch = nullptr;
	while (slab != nullptr && slab->start < end) {
		if (slab->start < addr) {
			Slab *after = Split(slab, addr - slab->start);
			slab = after;
		}
		// slab stays the part before end, the one being marked.
		if (slab->end > end) {
			Split(slab, end - slab->start);
		}

		slab->allocated = allocated;
		if (pc != 0) {
			slab->ticks = ticks;
			slab->pc = pc;
		}
		if (tag)
			slab->tagLen = (uint8_t)truncate_cpy_len(slab->tag, tag, tagLen);

		// Move on to the next one.
		if (firstMatch == nullptr)
			firstMatch = slab;
		slab = slab->next;
	}

	if (firstMatch != nullptr) {
		// This will merge all those blocks to one.
		MergeAdjacent(firstMatch);
		return true;
	}
	return false;
}

bool MemSlabMap::Find(MemBlockFlags flags, uint32_t addr, uint32_t size, std::vector<MemBlockInfo> &results) {
	uint32_t end = addr + size;
	Slab *slab = FindSlab(addr);
	bool found = false;
	while (slab != nullptr && slab->start < end) {
		if (slab->pc != 0 || slab->tag[0] != '\0') {
			results.push_back({ flags, slab->start, slab->end - slab->start, slab->ticks, slab->pc, slab->tag, slab->allocated });
			found = true;
		}
		slab = slab->next;
	}
	return found;
}

const char *MemSlabMap::FastFindWriteTag(MemBlockFlags flags, uint32_t addr, uint32_t size, size_t *tagLen) {
	uint32_t end = addr + size;
	Slab *slab = FindSlab(addr);
	while (slab != nullptr && slab->start < end) {
		if (slab->pc != 0 || slab->tag[0] != '\0') {
			*tagLen = slab->tagLen;
			return slab->tag;
		}
		slab = slab->next;
	}
	return nullptr;
}

void MemSlabMap::Reset() {
	Clear();

	first_ = NewSlab();
	first_->end = MAX_SIZE;
	lastFind_ = first_;

	heads_.resize(SLICES, first_);
}

void MemSlabMap::DoState(PointerWrap &p) {
	auto s = p.Section("MemSlabMap", 1);
	if (!s)
		return;

	int count = 0;
	if (p.mode == p.MODE_READ) {
		// Since heads_ is a static size, let's avoid clearing it.
		// This helps in case a debugger call happens concurrently.
		Slab *old = first_;
		Slab *oldBulk = bulkStorage_;
		Do(p, count);

		// Spares may live in the old bulk array, which goes away below.
		FreeSpares();

		first_ = NewSlab();
		first_->DoState(p);
		lastFind_ = first_;
		--count;

		FillHeads(first_);

		bulkStorage_ = new Slab[count];

		Slab *slab = first_;
		for (int i = 0; i < count; ++i) {
			slab->next = &bulkStorage_[i];
			slab->next->bulkStorage = true;
			slab->next->DoState(p);

			slab->next->prev = slab;
			slab = slab->next;

			FillHeads(slab);
		}

		// Now that it's entirely disconnected, delete the old slabs.
		while (old != nullptr) {
			Slab *next = old->next;
			if (!old->bulkStorage)
				delete old;
			old = next;
		}
		delete [] oldBulk;
	} else {
		for (Slab *slab = first_; slab != nullptr; slab = slab->next)
			++count;
		Do(p, count);

		first_->DoState(p);
		--count;

		Slab *slab = first_;
		for (int i = 0; i < count; ++i) {
			if (slab->next) {
				slab->next->DoState(p);
				slab = slab->next;
			}
		}
	}
}

void MemSlabMap::Slab::DoState(PointerWrap &p) {
	auto s = p.Section("MemSlabMapSlab", 1, 3);
	if (!s)
		return;

	Do(p, start);
	Do(p, end);
	Do(p, ticks);
	Do(p, pc);
	Do(p, allocated);
	if (s >= 3) {
		Do(p, tag);
		tagLen = (uint8_t)strnlen(tag, sizeof(tag) - 1);
	} else if (s >= 2) {
		char shortTag[32];
		Do(p, shortTag);
		memcpy(tag, shortTag, sizeof(shortTag));
		tagLen = (uint8_t)strnlen(tag, sizeof(shortTag) - 1);
	} else {
		std::string stringTag;
		Do(p, stringTag);
		tagLen = (uint8_t)std::min(stringTag.size(), sizeof(tag) - 1);
		memcpy(tag, stringTag.data(), tagLen);
		tag[tagLen] = '\0';
	}
}

void MemSlabMap::Clear() {
	Slab *s = first_;
	while (s != nullptr) {
		Slab *next = s->next;
		if (!s->bulkStorage)
			delete s;
		s = next;
	}
	FreeSpares();
	delete [] bulkStorage_;
	bulkStorage_ = nullptr;
	first_ = nullptr;
	lastFind_ = nullptr;
	heads_.clear();
}

MemSlabMap::Slab *MemSlabMap::FindSlab(uint32_t addr) {
	// Jump ahead using our index.
	Slab *slab = heads_[addr / SLICE_SIZE];
	// We often move forward, so check the last find.
	if (lastFind_->start > slab->start && lastFind_->start <= addr)
		slab = lastFind_;

	while (slab != nullptr && slab->start <= addr) {
		if (slab->end > addr) {
			lastFind_ = slab;
			return slab;
		}
		slab = slab->next;
	}
	return nullptr;
}

MemSlabMap::Slab *MemSlabMap::NewSlab() {
	Slab *slab = spare_;
	if (slab == nullptr)
		return new Slab();
	spare_ = slab->next;
	// Bulk slabs stay owned by their array.
	bool bulk = slab->bulkStorage;
	*slab = Slab();
	slab->bulkStorage = bulk;
	return slab;
}

void MemSlabMap::FreeSlab(Slab *slab) {
	slab->prev = nullptr;
	slab->next = spare_;
	spare_ = slab;
}

void MemSlabMap::FreeSpares() {
	while (spare_ != nullptr) {
		Slab *next = spare_->next;
		if (!spare_->bulkStorage)
			delete spare_;
		spare_ = next;
	}
}

MemSlabMap::Slab *MemSlabMap::Split(Slab *&slab, uint32_t size) {
	Slab *piece = NewSlab();
	piece->ticks = slab->ticks;
	piece->pc = slab->pc;
	piece->allocated = slab->allocated;
	piece->tagLen = (uint8_t)std::min<size_t>(slab->tagLen, sizeof(piece->tag) - 1);
	memcpy(piece->tag, slab->tag, piece->tagLen);
	piece->tag[piece->tagLen] = '\0';

	// Only the index entries of the new slab's part change. The first slab spans the whole
	// address space, and marks cut small pieces out of large unmarked stretches, so making
	// the shorter part the new one keeps that to the size of the mark.
	const uint32_t at = slab->start + size;
	if (size <= slab->end - at) {
		piece->start = slab->start;
		piece->end = at;
		piece->prev = slab->prev;
		piece->next = slab;
		if (piece->prev)
			piece->prev->next = piece;
		else
			first_ = piece;
		slab->prev = piece;
		slab->start = at;
		SetHeads(piece, piece->start, piece->end);

		Slab *after = slab;
		slab = piece;
		return after;
	}

	piece->start = at;
	piece->end = slab->end;
	piece->prev = slab;
	piece->next = slab->next;
	slab->next = piece;
	if (piece->next)
		piece->next->prev = piece;
	slab->end = at;
	SetHeads(piece, piece->start, piece->end);
	return piece;
}

bool MemSlabMap::Same(const Slab *a, const Slab *b) {
	if (a->allocated != b->allocated)
		return false;
	if (a->pc != b->pc)
		return false;
	if (strcmp(a->tag, b->tag))
		return false;
	return true;
}

bool MemSlabMap::Unchanged(const Slab *slab, uint32_t pc, bool allocated, const char *tag, size_t tagLen) {
	if (slab->allocated != allocated || (pc != 0 && slab->pc != pc))
		return false;
	if (tag) {
		const size_t len = std::min(tagLen, sizeof(slab->tag) - 1);
		if (slab->tagLen != len || memcmp(slab->tag, tag, len) != 0)
			return false;
	}
	// Marking merges with like neighbours, which an unchanged slab would not.
	return !(slab->prev && Same(slab, slab->prev)) && !(slab->next && Same(slab, slab->next));
}

void MemSlabMap::MergeAdjacent(Slab *slab) {
	while (slab->next != nullptr && Same(slab, slab->next)) {
		slab = Merge(slab, slab->next);
	}
	while (slab->prev != nullptr && Same(slab, slab->prev)) {
		slab = Merge(slab, slab->prev);
	}
}

MemSlabMap::Slab *MemSlabMap::Merge(Slab *a, Slab *b) {
	// Same() holds, so the two only differ in their range and ticks: whichever object is
	// kept, the result is the same. Keep the longer one, so only the shorter one's index
	// entries need moving over.
	Slab *keep = b->end - b->start > a->end - a->start ? b : a;
	Slab *gone = keep == a ? b : a;
	if (keep->next == gone) {
		_assert_(keep->end == gone->start);
		keep->end = gone->end;
		keep->next = gone->next;

		if (keep->next)
			keep->next->prev = keep;
	} else if (keep->prev == gone) {
		_assert_(gone->end == keep->start);
		keep->start = gone->start;
		keep->prev = gone->prev;

		if (keep->prev)
			keep->prev->next = keep;
		else if (first_ == gone)
			first_ = keep;
	} else {
		_assert_(false);
	}
	SetHeads(keep, gone->start, gone->end);
	if (gone->ticks > keep->ticks) {
		keep->ticks = gone->ticks;
		// In case we ignore PC for same.
		keep->pc = gone->pc;
	}
	if (lastFind_ == gone)
		lastFind_ = keep;
	FreeSlab(gone);
	return keep;
}

void MemSlabMap::FillHeads(Slab *slab) {
	SetHeads(slab, slab->start, slab->end);
}

// The index holds, for each slice, the slab its first address is in: point every slice that
// starts within [start, end) at slab.
void MemSlabMap::SetHeads(Slab *slab, uint32_t start, uint32_t end) {
	const uint32_t first = (start + SLICE_SIZE - 1) / SLICE_SIZE;
	const uint32_t last = (end - 1) / SLICE_SIZE;
	if (first <= last)
		indexUpdates += last - first + 1;
	for (uint32_t i = first; i <= last; ++i) {
		heads_[i] = slab;
	}
}

size_t FormatMemWriteTagAtNoFlush(char *buf, size_t sz, const char *prefix, size_t prefixLen, uint32_t start, uint32_t size);

// Emulation thread.
void FlushPendingMemInfo() {
	pendingNotifies.Drain([](PendingNotifyMem &&info) {
		if (info.copySrc != 0) {
			char tagData[128];
			size_t tagSize = FormatMemWriteTagAtNoFlush(tagData, sizeof(tagData), info.tag, info.tagLen, info.copySrc, info.size);
			writeMap.Mark(info.start, info.size, info.ticks, info.pc, true, tagData, tagSize);
			return;
		}

		if (info.flags & MemBlockFlags::ALLOC) {
			allocMap.Mark(info.start, info.size, info.ticks, info.pc, true, info.tag, info.tagLen);
		} else if (info.flags & MemBlockFlags::FREE) {
			// Maintain the previous allocation tag for debugging.
			allocMap.Mark(info.start, info.size, info.ticks, 0, false, nullptr, 0);
			suballocMap.Mark(info.start, info.size, info.ticks, 0, false, nullptr, 0);
		}
		if (info.flags & MemBlockFlags::SUB_ALLOC) {
			suballocMap.Mark(info.start, info.size, info.ticks, info.pc, true, info.tag, info.tagLen);
		} else if (info.flags & MemBlockFlags::SUB_FREE) {
			// Maintain the previous allocation tag for debugging.
			suballocMap.Mark(info.start, info.size, info.ticks, 0, false, nullptr, 0);
		}
		if (info.flags & MemBlockFlags::TEXTURE) {
			textureMap.Mark(info.start, info.size, info.ticks, info.pc, true, info.tag, info.tagLen);
		}
		if (info.flags & MemBlockFlags::WRITE) {
			writeMap.Mark(info.start, info.size, info.ticks, info.pc, true, info.tag, info.tagLen);
		}
	});
}

static void FlushIfPending() {
	if (!pendingNotifies.Empty()) {
		FlushPendingMemInfo();
	}
}

static void FlushOnFlip(void *) {
	FlushPendingMemInfo();
}

static inline uint32_t NormalizeAddress(uint32_t addr) {
	if ((addr & 0x3F000000) == 0x04000000)
		return addr & 0x041FFFFF;
	return addr & 0x3FFFFFFF;
}

void NotifyMemInfoPC(MemBlockFlags flags, uint32_t start, uint32_t size, uint32_t pc, const char *tagStr, size_t strLength) {
	if (size == 0) {
		return;
	}
	// Clear the uncached and kernel bits.
	start = NormalizeAddress(start);

	// When the setting is off, we skip smaller info to keep things fast.
	if (MemBlockInfoDetailed(size) && flags != MemBlockFlags::READ) {
		PendingNotifyMem info{ flags, start, size };
		info.ticks = CoreTiming::GetTicks(currentMIPS);
		info.pc = pc;

		size_t copyLength = strLength;
		if (copyLength >= sizeof(info.tag)) {
			copyLength = sizeof(info.tag) - 1;
		}
		memcpy(info.tag, tagStr, copyLength);
		info.tag[copyLength] = 0;
		info.tagLen = (uint8_t)copyLength;

		pendingNotifies.Push(info);
	}

	if (!(flags & MemBlockFlags::SKIP_MEMCHECK)) {
		if (flags & MemBlockFlags::WRITE) {
			g_breakpoints.ExecMemCheck(start, true, size, pc, tagStr);
		} else if (flags & MemBlockFlags::READ) {
			g_breakpoints.ExecMemCheck(start, false, size, pc, tagStr);
		}
	}
}

void NotifyMemInfo(MemBlockFlags flags, uint32_t start, uint32_t size, const char *str, size_t strLength) {
	NotifyMemInfoPC(flags, start, size, currentMIPS->pc, str, strLength);
}

void NotifyMemInfoCopy(uint32_t destPtr, uint32_t srcPtr, uint32_t size, const char *prefix, size_t prefixLen) {
	if (size == 0)
		return;

	if (g_breakpoints.HasMemChecks()) {
		// This will cause a flush, but it's needed to trigger memchecks with proper data.
		char tagData[128];
		size_t tagSize = FormatMemWriteTagAt(tagData, sizeof(tagData), prefix, prefixLen, srcPtr, size);
		NotifyMemInfo(MemBlockFlags::READ, srcPtr, size, tagData, tagSize);
		NotifyMemInfo(MemBlockFlags::WRITE, destPtr, size, tagData, tagSize);
	} else if (MemBlockInfoDetailed(size)) {
		srcPtr = NormalizeAddress(srcPtr);
		destPtr = NormalizeAddress(destPtr);

		PendingNotifyMem info{ MemBlockFlags::WRITE, destPtr, size };
		info.copySrc = srcPtr;
		info.ticks = CoreTiming::GetTicks(currentMIPS);
		info.pc = currentMIPS->pc;

		// Store the prefix for now.  The correct tag will be calculated on flush.
		info.tagLen = (uint8_t)std::min(sizeof(info.tag), prefixLen);
		memcpy(info.tag, prefix, info.tagLen);

		pendingNotifies.Push(info);
	}
}

std::vector<MemBlockInfo> FindMemInfo(uint32_t start, uint32_t size) {
	start = NormalizeAddress(start);

	FlushIfPending();
	std::vector<MemBlockInfo> results;
	allocMap.Find(MemBlockFlags::ALLOC, start, size, results);
	suballocMap.Find(MemBlockFlags::SUB_ALLOC, start, size, results);
	writeMap.Find(MemBlockFlags::WRITE, start, size, results);
	textureMap.Find(MemBlockFlags::TEXTURE, start, size, results);
	return results;
}

std::vector<MemBlockInfo> FindMemInfoByFlag(MemBlockFlags flags, uint32_t start, uint32_t size) {
	start = NormalizeAddress(start);

	FlushIfPending();
	std::vector<MemBlockInfo> results;
	if (flags & MemBlockFlags::ALLOC)
		allocMap.Find(MemBlockFlags::ALLOC, start, size, results);
	if (flags & MemBlockFlags::SUB_ALLOC)
		suballocMap.Find(MemBlockFlags::SUB_ALLOC, start, size, results);
	if (flags & MemBlockFlags::WRITE)
		writeMap.Find(MemBlockFlags::WRITE, start, size, results);
	if (flags & MemBlockFlags::TEXTURE)
		textureMap.Find(MemBlockFlags::TEXTURE, start, size, results);
	return results;
}

static const char *FindWriteTagByFlag(MemBlockFlags flags, uint32_t start, uint32_t size, size_t *tagLen, bool flush = true) {
	start = NormalizeAddress(start);

	// The returned tag pointer is only valid until the next Mark(). flush=false is for
	// FormatMemWriteTagAtNoFlush(), called from inside FlushPendingMemInfo().
	if (flush) {
		FlushIfPending();
	}
	if (flags & MemBlockFlags::ALLOC) {
		const char *tag = allocMap.FastFindWriteTag(MemBlockFlags::ALLOC, start, size, tagLen);
		if (tag)
			return tag;
	}
	if (flags & MemBlockFlags::SUB_ALLOC) {
		const char *tag = suballocMap.FastFindWriteTag(MemBlockFlags::SUB_ALLOC, start, size, tagLen);
		if (tag)
			return tag;
	}
	if (flags & MemBlockFlags::WRITE) {
		const char *tag = writeMap.FastFindWriteTag(MemBlockFlags::WRITE, start, size, tagLen);
		if (tag)
			return tag;
	}
	if (flags & MemBlockFlags::TEXTURE) {
		const char *tag = textureMap.FastFindWriteTag(MemBlockFlags::TEXTURE, start, size, tagLen);
		if (tag)
			return tag;
	}
	*tagLen = 0;
	return nullptr;
}

size_t FormatMemWriteTagAt(char *buf, size_t sz, const char *prefix, size_t prefixLen, uint32_t start, uint32_t size) {
	size_t tagLen;
	const char *tag = FindWriteTagByFlag(MemBlockFlags::WRITE, start, size, &tagLen);
	if (tag && strcmp(tag, "MemInit") != 0) {
		return truncate_cat(buf, sz, prefix, prefixLen, tag, tagLen);
	}
	// Fall back to alloc and texture, especially for VRAM.  We prefer write above.
	tag = FindWriteTagByFlag(MemBlockFlags::ALLOC | MemBlockFlags::TEXTURE, start, size, &tagLen);
	if (tag) {
		return truncate_cat(buf, sz, prefix, prefixLen, tag, tagLen);
	}
	return snprintf(buf, sz, "%s%08x_size_%08x", prefix, start, size);
}

size_t FormatMemWriteTagAtNoFlush(char *buf, size_t sz, const char *prefix, size_t prefixLen, uint32_t start, uint32_t size) {
	size_t tagLen;
	const char *tag = FindWriteTagByFlag(MemBlockFlags::WRITE, start, size, &tagLen, false);
	if (tag && strcmp(tag, "MemInit") != 0) {
		return truncate_cat(buf, sz, prefix, prefixLen, tag, tagLen);
	}
	// Fall back to alloc and texture, especially for VRAM.  We prefer write above.
	tag = FindWriteTagByFlag(MemBlockFlags::ALLOC | MemBlockFlags::TEXTURE, start, size, &tagLen, false);
	if (tag) {
		return truncate_cat(buf, sz, prefix, prefixLen, tag, tagLen);
	}
	return snprintf(buf, sz, "%s%08x_size_%08x", prefix, start, size);
}

// Emulation thread, like the rest below.
void MemBlockInfoInit() {
	pendingNotifies.Drain([](PendingNotifyMem &&) {});
	__DisplayListenFlip(&FlushOnFlip, nullptr);
}

void MemBlockInfoShutdown() {
	__DisplayForgetFlip(&FlushOnFlip, nullptr);
	allocMap.Reset();
	suballocMap.Reset();
	writeMap.Reset();
	textureMap.Reset();
	pendingNotifies.Drain([](PendingNotifyMem &&) {});
}

void MemBlockInfoDoState(PointerWrap &p) {
	auto s = p.Section("MemBlockInfo", 0, 1);
	if (!s)
		return;

	FlushPendingMemInfo();
	allocMap.DoState(p);
	suballocMap.DoState(p);
	writeMap.DoState(p);
	textureMap.DoState(p);
}

uint64_t MemBlockInfoIndexUpdates() {
	return indexUpdates;
}

// Used by the debugger.
void MemBlockOverrideDetailed() {
	retro_atomic_fetch_add_int(&detailedOverride, 1);
}

void MemBlockReleaseDetailed() {
	retro_atomic_fetch_sub_int(&detailedOverride, 1);
}

// Any thread.
bool MemBlockInfoDetailed() {
	return g_Config.bDebugMemInfoDetailed || retro_atomic_load_relaxed_int(&detailedOverride) != 0;
}

void DescribeAddress(const MIPSDebugInterface *mips, u32 address, char *buffer, size_t bufferSize) {
	StringWriter w(buffer, bufferSize);
	const char *kernel = (address & 0x80000000) ? " (kernel)" : "";
	const char *uncached = (address & 0x40000000) ? " (uncached)" : "";
	char desc[512];
	if (!Memory::IsValidAddress(address)) {
		if (address == 0xdeadbeef) {
			w.C("(deadbeef)");
		} else {
			w.C("(invalid)");
		}
	} else if (DescribeModuleAddress(address, desc, sizeof(desc))) {
		std::string temp = g_symbolMap->GetDescription(address);
		w.F("[%s]%s%s: %s", desc, kernel, uncached, temp.c_str());
	} else if (Memory::IsVRAMAddress(address)) {
		w.F("[VRAM]%s", uncached);  // can't be kernel
	} else if (Memory::IsScratchpadAddress(address)) {
		w.F("[SCRATCH]%s%s", kernel, uncached);
	} else {
		// Look up in the memory tracker.
		w.F("[RAM]%s%s ", kernel, uncached);
		const std::vector<MemBlockInfo> memInfo = FindMemInfo(address, 4);
		for (const auto &info : memInfo) {
			w.F("%s, ", info.tag.c_str());
		}
		if (!memInfo.empty()) {
			w.Rewind(2);  // Remove the last comma
		}
	}
}
