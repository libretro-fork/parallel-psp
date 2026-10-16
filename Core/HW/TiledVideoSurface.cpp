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

#include "Core/HW/TiledVideoSurface.h"
#include "Core/HW/VideoMemory.h"
#include "Core/HW/AvcDecoder.h"
#include "Core/HLE/sceVideocodec.h"
#include "Core/MemMap.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "Common/GPU/thin3d.h"
#include "Common/Log.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace {
struct Surface {
	Draw::Texture *texture = nullptr;
	std::array<u32, 8> buffers{};
	std::array<int, 8> sizes{};
	int width = 0, height = 0;
	bool me = false, pending = true, resolving = false;
	~Surface() { if (texture) texture->Release(); }
};
std::vector<std::unique_ptr<Surface>> surfaces;
bool Overlaps(u32 a, u32 size, u32 b, u32 bytes) {
	return size && bytes && (uint64_t)a < (uint64_t)b + bytes && (uint64_t)b < (uint64_t)a + size;
}
bool Matches(const Surface &frame, const u32 buffers[8], int width, int height) {
	if (!frame.pending || (!frame.me && !HasVideoMemoryOwner((void *)&frame)) || frame.width != width || frame.height != height) return false;
	for (int i = 0; i < 8; ++i)
		if (frame.buffers[i] != (frame.me ? buffers[i] : VideoMemory::CanonicalAddress(buffers[i]))) return false;
	return true;
}

bool Materialize(void *owner) {
	auto *frame = (Surface *)owner;
	if (!frame->pending || frame->resolving) return true;
	Draw::DrawContext *draw = gpu ? gpu->GetDrawContext() : nullptr;
	if (!draw || !frame->texture) return false;
	frame->resolving = true;
	std::vector<u8> original((size_t)frame->width * frame->height * 4);
	const bool temporaryFrame = draw->BeginVideoMemoryAccess();
	const bool copied = draw->ReadVideoPlanes(frame->texture, original.data(), frame->width);
	draw->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);
	gpu->GetFramebufferManagerCommon()->RebindFramebuffer("Tiled video CPU access");
	gstate_c.Dirty(DIRTY_ALL_RENDER_STATE);
	draw->EndVideoMemoryAccess(temporaryFrame);
	if (!copied) { frame->resolving = false; return false; }
	// All guest components were unprotected together before entering this callback.
	for (int b = 0; b < 8; ++b) {
		if (!frame->sizes[b]) continue;
		u8 *dest = frame->me ? MEGetPointerRange(frame->buffers[b], frame->sizes[b]) : Memory::GetPointerWriteUnchecked(frame->buffers[b]);
		if (!dest) { frame->resolving = false; return false; }
		const bool chroma = b >= 4;
		const int half = b & 3;
		const int sampleWidth = frame->width >> (chroma ? 1 : 0);
		const int sampleHeight = frame->height >> (chroma ? 1 : 0);
		const int run = chroma ? 8 : 16;
		const int step = chroma ? 16 : 32;
		int offset = 0;
		for (int band = (half & 1) ? run : 0; band < sampleWidth; band += step) {
			for (int row = (half >> 1) ? 1 : 0; row < sampleHeight; row += 2) {
				for (int x = 0; x < run; ++x) {
					const u8 *pixel = &original[((size_t)(row << (chroma ? 1 : 0)) * frame->width + ((band + x) << (chroma ? 1 : 0))) * 4];
					dest[offset++] = pixel[chroma ? 1 : 0];
					if (chroma) dest[offset++] = pixel[2];
				}
			}
		}
	}
	++gpuStats.perFrame.numVideoReadbacks;
	frame->pending = false;
	frame->resolving = false;
	frame->texture->Release(); frame->texture = nullptr;
	return true;
}
void Collect() {
	surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [](const auto &frame) { return !frame->pending || (!frame->me && !HasVideoMemoryOwner(frame.get())); }), surfaces.end());
}
}

Draw::Texture *FindTiledVideoSurface(const u32 buffers[8], int width, int height) {
	for (const auto &frame : surfaces)
		if (Matches(*frame, buffers, width, height)) return frame->texture;
	return nullptr;
}

bool RememberTiledVideoSurface(Draw::Texture *texture, const u32 buffers[8], int width, int height, bool meMemory) {
	if (!texture || !CanDeferVideoMemory() || width <= 0 || height <= 0 || width > 1024 || height > 1024 || (width & 15) || (height & 15)) return false;
	int sizes[8];
	VideocodecFrameBufferLayout(width, height, sizes, nullptr);
	for (int i = 0; i < 8; ++i) {
		if (sizes[i] && !(meMemory ? MEIsValidRange(buffers[i], sizes[i]) : Memory::IsRAMAddress(buffers[i]) && Memory::IsValidRange(buffers[i], sizes[i]))) return false;
	}
	texture->AddRef();  // The source can also be the overwritten destination.
	for (auto &frame : surfaces) {
		if (frame->me == meMemory && Matches(*frame, buffers, width, height)) {
			if (!frame->me && !DiscardVideoMemoryOwner(frame.get())) { texture->Release(); return false; }
			frame->pending = false;
		}
	}
	Collect();
	if (surfaces.size() >= 8) {
		auto *old = surfaces.front().get();
		if (!(old->me ? Materialize(old) : ResolveVideoMemoryOwner(old))) { texture->Release(); return false; }
		Collect();
	}
	auto frame = std::make_unique<Surface>();
	frame->texture = texture; frame->width = width; frame->height = height; frame->me = meMemory;
	for (int i = 0; i < 8; ++i) {
		frame->buffers[i] = meMemory ? buffers[i] : VideoMemory::CanonicalAddress(buffers[i]);
		frame->sizes[i] = sizes[i];
	}
	auto *owner = frame.get();
	surfaces.push_back(std::move(frame));
	if (!meMemory) {
		for (int i = 0; i < 8; ++i) {
			if (sizes[i] && !TrackVideoMemory(buffers[i], sizes[i], owner, Materialize)) {
				if (!ResolveVideoMemoryOwner(owner)) return false;
				Materialize(owner);
				return false;
			}
		}
		for (int i = 0; i < 8; ++i) if (sizes[i]) gpu->InvalidateCache(buffers[i], sizes[i], GPU_INVALIDATE_HINT);
	}
	return true;
}

bool DeferTiledVideoFrame(AvcDecoder &decoder, const u32 buffers[8], int width, int height) {
	Draw::DrawContext *draw = gpu ? gpu->GetDrawContext() : nullptr;
	if (!draw || !draw->SupportsVideoImport() || !CanDeferVideoMemory()) {
		static bool reported = false;
		if (!reported) { INFO_LOG(Log::ME, "GPU surface prerequisites unavailable (draw=%p, import=%d, memory=%d)", (void *)draw, draw && draw->SupportsVideoImport(), CanDeferVideoMemory()); reported = true; }
		return false;
	}
	video_hw_frame_t exported{};
	if (!decoder.ExportGpuFrame(&exported)) return false;
	Draw::Texture *texture = draw->ImportVideoFrame(exported);
	if (!texture) {
		static bool reported = false;
		if (!reported) { INFO_LOG(Log::ME, "GPU surface import unavailable (api=%u, format=%u, planes=%llx/%llx)", exported.api, exported.pixfmt, (unsigned long long)exported.plane_views[0], (unsigned long long)exported.plane_views[1]); reported = true; }
		video_hw_frame_abandon(&exported); return false;
	}
	const bool remembered = RememberTiledVideoSurface(texture, buffers, width, height, true);
	static bool reported = false;
	if (remembered && !reported) { INFO_LOG(Log::ME, "GPU-backed tiled video frames active (%dx%d)", width, height); reported = true; }
	texture->Release();
	return remembered;
}

bool ResolveTiledVideoMemory(u32 address, u32 size) {
	for (auto &frame : surfaces) {
		if (!frame->me || !frame->pending || frame->resolving) continue;
		for (int i = 0; i < 8; ++i) {
			if (Overlaps(address, size, frame->buffers[i], frame->sizes[i])) {
				if (!Materialize(frame.get())) return false;
				break;
			}
		}
	}
	return true;
}
bool ResolveAllTiledVideoSurfaces() {
	for (auto &frame : surfaces) {
		if (frame->pending && !(frame->me ? Materialize(frame.get()) : ResolveVideoMemoryOwner(frame.get()))) return false;
	}
	Collect();
	return true;
}
void DiscardTiledVideoMemory(u32 address, u32 size) {
	for (auto &frame : surfaces) {
		if (frame->me && frame->pending && address <= frame->buffers[0]
			&& (uint64_t)address + size >= (uint64_t)frame->buffers[7] + frame->sizes[7]) frame->pending = false;
	}
	Collect();
}
void ClearTiledVideoSurfaces() {
	if (!ResolveAllTiledVideoSurfaces()) {
		ERROR_LOG(Log::ME, "Could not preserve tiled video during context teardown");
		for (auto &frame : surfaces) {
			if (!frame->me) DiscardVideoMemoryOwner(frame.get());
		}
	}
	// No texture or protected-page callback may outlive its graphics context.
	surfaces.clear();
}
