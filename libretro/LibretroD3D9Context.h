// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
#pragma once

#include <d3d9.h>
#include <libretro_d3d9.h>
#include "libretro/LibretroGraphicsContext.h"

class LibretroD3D9Context : public LibretroHWRenderContext {
public:
	LibretroD3D9Context() : LibretroHWRenderContext(RETRO_HW_CONTEXT_D3D9, 9) {}
	~LibretroD3D9Context() override { ContextDestroy(); }
	bool InitAPI(void *, std::string *, std::string *) override;
	void ContextReset() override;
	void ContextDestroy() override;
	void CreateDrawContext() override;
	void DestroyDrawContext() override;
	void GotBackbuffer() override {}
	void LostBackbuffer() override;
	void FrameBegin() override;
	void SwapBuffers() override;
	void LockGPU() override;
	void UnlockGPU() override;
	GPUCore GetGPUCore() override { return GPUCORE_DIRECTX9; }
	const char *Ident() override { return "DirectX 9"; }

private:
	struct Slot {
		IDirect3DTexture9 *texture = nullptr;
		IDirect3DSurface9 *color = nullptr;
		IDirect3DSurface9 *depth = nullptr;
		unsigned width = 0;
		unsigned height = 0;
	};
	static void FreeSlot(Slot &slot);
	bool MakeSlot(Slot &slot, unsigned width, unsigned height);
	retro_hw_render_interface_d3d9 iface_{};
	Slot slots_[32]{};
	IDirect3D9 *d3d_ = nullptr;
	IDirect3D9Ex *d3dEx_ = nullptr;
	IDirect3DDevice9Ex *deviceEx_ = nullptr;
	GPUCommon *lostGpu_ = nullptr;
	int frameSlot_ = -1;
	int boundSlot_ = -1;
	bool live_ = false;
	bool scene_ = false;
};
