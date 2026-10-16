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
#include "Core/CoreParameter.h"
#include "Core/System.h"
#include "Core/MIPS/MIPS.h"
#include "unittest/UnitTest.h"

bool TestVideoMemoryHost() {
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
	EXPECT_TRUE(!Memory::IsActive());
	const auto oldCore = PSP_CoreParameter().cpuCore;
	const u32 oldSize = Memory::g_MemorySize;
	PSP_CoreParameter().cpuCore = CPUCore::INTERPRETER;
	Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE;
	EXPECT_TRUE(Memory::Init(Memory::MemMapSetupFlags::Default));
	struct Cleanup {
		CPUCore core;
		u32 size;
		~Cleanup() { Memory::Shutdown(); PSP_CoreParameter().cpuCore = core; Memory::g_MemorySize = size; }
	} cleanup{oldCore, oldSize};
	EXPECT_TRUE(CanDeferVideoMemory());
	struct Pixels {
		int copies = 0;
		u32 address = 0x08810080;
		static bool Materialize(void *owner) {
			auto *pixels = (Pixels *)owner;
			++pixels->copies;
			for (int i = 0; i < 64; ++i)
				Memory::WriteUnchecked_U32(0x12340000 + i, pixels->address + i * 4);
			return true;
		}
	} pixels;
	const u32 aliases[]{0, 0x40000000, 0x80000000, 0xC0000000};
	for (u32 alias : aliases) {
		EXPECT_TRUE(TrackVideoMemory(pixels.address, 256, &pixels, Pixels::Materialize));
		// A direct native load uses the same protected mapping as generated code.
		const u32 value = *(const u32 *)(Memory::base + (pixels.address | alias));
		EXPECT_EQ_HEX(value, 0x12340000);
		EXPECT_TRUE(!HasVideoMemory(pixels.address, 1));
	}
	EXPECT_EQ_INT(pixels.copies, 4);
	EXPECT_TRUE(TrackVideoMemory(pixels.address, 256, &pixels, Pixels::Materialize));
	*(u32 *)(Memory::base + (pixels.address | 0x40000000)) = 0x87654321;
	EXPECT_TRUE(!HasVideoMemory(pixels.address, 1));
	EXPECT_EQ_INT(pixels.copies, 5);
	EXPECT_EQ_HEX(Memory::ReadUnchecked_U32(pixels.address), 0x87654321);
	EXPECT_EQ_HEX(Memory::ReadUnchecked_U32(pixels.address + 4), 0x12340001);
	EXPECT_TRUE(TrackVideoMemory(pixels.address, 256, &pixels, Pixels::Materialize));
	EXPECT_TRUE(PrepareVideoMemoryOverwrite(pixels.address, 256));
	EXPECT_TRUE(!HasVideoMemory(pixels.address, 1));
	EXPECT_EQ_INT(pixels.copies, 5);
	EXPECT_TRUE(TrackVideoMemory(pixels.address, 256, &pixels, Pixels::Materialize));
	EXPECT_TRUE(ResolveAllVideoMemory());
	EXPECT_EQ_INT(pixels.copies, 6);
	pixels.address = 0x04010080;
	for (u32 mirror = 0; mirror < 4; ++mirror) {
		for (u32 uncached : {0u, 0x40000000u}) {
			EXPECT_TRUE(TrackVideoMemory(pixels.address, 256, &pixels, Pixels::Materialize));
			const u32 value = *(const u32 *)(Memory::base + pixels.address + mirror * 0x00200000 + uncached);
			EXPECT_TRUE(!HasVideoMemory(pixels.address, 1));
			EXPECT_EQ_HEX(value, 0x12340000);
		}
	}
	EXPECT_EQ_INT(pixels.copies, 14);
	ResetVideoMemory();
#else
	EXPECT_TRUE(!CanDeferVideoMemory());
#endif
	return true;
}
