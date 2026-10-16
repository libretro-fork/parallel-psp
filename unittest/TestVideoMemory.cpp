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
#include "unittest/UnitTest.h"

namespace {
struct Protection {
	int protects = 0, unprotects = 0;
	bool fail = false;
};
struct Pixels {
	int copies = 0;
	bool fail = false;
	static bool Copy(void *owner) {
		auto *pixels = (Pixels *)owner;
		++pixels->copies;
		return !pixels->fail;
	}
};
bool Protect(void *context, uint32_t address, uint32_t size, bool protect) {
	auto *state = (Protection *)context;
	if (state->fail || (address & 4095) || (size & 4095))
		return false;
	if (protect) ++state->protects;
	else ++state->unprotects;
	return true;
}
}

bool TestVideoMemory() {
	Protection protection;
	Pixels pixels[10];
	VideoMemory memory(Protect, &protection, 4096);
	const uint32_t address = 0x08801080;
	EXPECT_TRUE(memory.Track(address, 512, &pixels[0], Pixels::Copy));
	EXPECT_TRUE(memory.Contains(address, 1));
	EXPECT_TRUE(memory.Contains(address | 0xC0000000, 1));
	EXPECT_TRUE(!memory.Contains(address + 8192, 1));
	EXPECT_EQ_INT(pixels[0].copies, 0);
	// A partial write first makes the whole old surface CPU-visible.
	EXPECT_TRUE(memory.PrepareOverwrite(address + 20, 1));
	EXPECT_EQ_INT(pixels[0].copies, 1);
	EXPECT_EQ_INT(memory.Count(), 0);
	EXPECT_TRUE(memory.Resolve(address, 512));
	EXPECT_EQ_INT(pixels[0].copies, 1);
	EXPECT_TRUE(memory.Track(address, 512, &pixels[0], Pixels::Copy));
	EXPECT_TRUE(memory.PrepareOverwrite(address | 0x40000000, 512));
	EXPECT_EQ_INT(pixels[0].copies, 1);
	EXPECT_EQ_INT(memory.Count(), 0);
	// Unrelated bytes sharing a protected page must remain accessible too.
	EXPECT_TRUE(memory.Track(address, 512, &pixels[0], Pixels::Copy));
	EXPECT_TRUE(memory.Resolve(address - 1, 1));
	EXPECT_EQ_INT(pixels[0].copies, 2);
	EXPECT_TRUE(memory.Track(address, 512, &pixels[0], Pixels::Copy));
	EXPECT_TRUE(memory.Track(address + 600, 32, &pixels[1], Pixels::Copy));
	EXPECT_EQ_INT(pixels[0].copies, 3);
	EXPECT_EQ_INT(memory.Count(), 1);
	EXPECT_TRUE(memory.ResolveOwner(&pixels[1]));
	EXPECT_EQ_INT(pixels[1].copies, 1);
	// A failed download must retain protection and the pending owner.
	pixels[2].fail = true;
	EXPECT_TRUE(memory.Track(address, 512, &pixels[2], Pixels::Copy));
	EXPECT_TRUE(!memory.ResolveAll());
	EXPECT_TRUE(memory.Contains(address, 1));
	EXPECT_EQ_INT(memory.Count(), 1);
	pixels[2].fail = false;
	EXPECT_TRUE(memory.ResolveAll());
	EXPECT_EQ_INT(pixels[2].copies, 2);
	protection.fail = true;
	EXPECT_TRUE(!memory.Track(address, 512, &pixels[0], Pixels::Copy));
	EXPECT_EQ_INT(memory.Count(), 0);
	protection.fail = false;
	// Bounded storage resolves an older surface rather than losing its pixels.
	Pixels capacity[65];
	for (int i = 0; i < 65; ++i)
		EXPECT_TRUE(memory.Track(address + i * 8192, 512, &capacity[i], Pixels::Copy));
	EXPECT_EQ_INT(memory.Count(), 64);
	EXPECT_EQ_INT(capacity[0].copies, 1);
	EXPECT_TRUE(memory.ResolveAll());
	for (const auto &item : capacity) EXPECT_EQ_INT(item.copies, 1);
	EXPECT_TRUE(memory.Track(0x04201080, 32, &pixels[4], Pixels::Copy));
	EXPECT_TRUE(memory.Resolve(0x44001080, 1));
	EXPECT_EQ_INT(pixels[4].copies, 1);
	EXPECT_TRUE(memory.Track(0x04000080, 32, &pixels[4], Pixels::Copy));
	EXPECT_TRUE(memory.Contains(0x041FFFF0, 0x100));
	EXPECT_TRUE(memory.Resolve(0x041FFFF0, 0x100));
	EXPECT_EQ_INT(pixels[4].copies, 2);
	EXPECT_TRUE(!memory.Track(address, 0, &pixels[0], Pixels::Copy));
	EXPECT_TRUE(!memory.Track(address, 512, nullptr, Pixels::Copy));
	EXPECT_TRUE(!memory.Track(address, 512, &pixels[0], nullptr));
	EXPECT_TRUE(!memory.Track(0x3FFFFFF0, 0xFFFFFFFF, &pixels[0], Pixels::Copy));
	EXPECT_TRUE(memory.Track(address, 512, &pixels[5], Pixels::Copy));
	memory.ForgetAll();
	EXPECT_EQ_INT(pixels[5].copies, 0);
	Pixels group;
	EXPECT_TRUE(memory.Track(address, 512, &group, Pixels::Copy));
	EXPECT_TRUE(memory.Track(address + 600, 32, &group, Pixels::Copy));
	EXPECT_EQ_INT(group.copies, 0);
	EXPECT_TRUE(memory.Resolve(address, 1));
	EXPECT_EQ_INT(group.copies, 1);
	EXPECT_EQ_INT(memory.Count(), 0);
	EXPECT_TRUE(memory.Track(address, 512, &group, Pixels::Copy));
	EXPECT_TRUE(memory.Track(address + 8192, 32, &group, Pixels::Copy));
	EXPECT_TRUE(memory.PrepareOverwrite(address, 512));
	EXPECT_EQ_INT(group.copies, 2);
	EXPECT_EQ_INT(memory.Count(), 0);
	EXPECT_TRUE(memory.Track(address, 512, &group, Pixels::Copy));
	EXPECT_TRUE(memory.Track(address + 8192, 32, &group, Pixels::Copy));
	EXPECT_TRUE(memory.PrepareOverwrite(address, 8192 + 32));
	EXPECT_EQ_INT(group.copies, 2);
	EXPECT_TRUE(!memory.ContainsOwner(&group));
	EXPECT_EQ_INT(protection.protects, protection.unprotects);
	return true;
}
