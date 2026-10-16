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

#include "Common/CommonTypes.h"
namespace Draw { class Texture; }
class AvcDecoder;

// Tiled YUV frames in separate ME memory or in eight protected guest RAM buffers.
bool DeferTiledVideoFrame(AvcDecoder &decoder, const u32 buffers[8], int width, int height);
bool RememberTiledVideoSurface(Draw::Texture *texture, const u32 buffers[8], int width, int height, bool meMemory);
Draw::Texture *FindTiledVideoSurface(const u32 buffers[8], int width, int height);
bool ResolveTiledVideoMemory(u32 address, u32 size);
bool ResolveAllTiledVideoSurfaces();
void DiscardTiledVideoMemory(u32 address, u32 size);
void ClearTiledVideoSurfaces();
