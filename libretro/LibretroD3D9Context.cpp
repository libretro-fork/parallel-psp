// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
#include <cstring>
#include "Common/Log.h"
#include "Common/GPU/D3D9/D3D9StateCache.h"
#include "Common/GPU/D3D9/D3DCompilerLoader.h"
#include "Core/ConfigValues.h"
#include "GPU/GPUCommon.h"
#include "libretro/LibretroD3D9Context.h"

#ifdef __MINGW32__
#undef __uuidof
#define __uuidof(type) IID_##type
#endif

bool LibretroD3D9Context::InitAPI(void *, std::string *, std::string *) {
	if (!InitHW(false)) {
		return false;
	}
	g_Config.iGPUBackend = (int)GPUBackend::DIRECT3D9;
	return true;
}

void LibretroD3D9Context::CreateDrawContext() {
	if (FAILED(iface_.device->GetDirect3D(&d3d_)) || !d3d_) {
		ERROR_LOG(Log::G3D, "Unable to query the frontend D3D9 device");
		return;
	}
	d3d_->QueryInterface(__uuidof(IDirect3D9Ex), (void **)&d3dEx_);
	iface_.device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&deviceEx_);
	D3DDEVICE_CREATION_PARAMETERS params{};
	iface_.device->GetCreationParameters(&params);
	pD3Ddevice9 = iface_.device;
	pD3DdeviceEx9 = deviceEx_;
	ptr_D3DCompile = reinterpret_cast<pD3DCompile>(iface_.D3DCompile);
	draw_ = Draw::T3DCreateDX9Context(d3d_, d3dEx_, params.AdapterOrdinal, iface_.device, deviceEx_);
}

void LibretroD3D9Context::LockGPU() {
	if (live_ && iface_.lock_context(iface_.handle) && draw_) {
		draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);
		dxstate.Restore();
	}
}

void LibretroD3D9Context::UnlockGPU() {
	if (live_) {
		iface_.unlock_context(iface_.handle);
	}
}

void LibretroD3D9Context::ContextReset() {
	retro_hw_render_interface_d3d9 *incoming = nullptr;
	if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &incoming) || !incoming
		|| incoming->interface_type != RETRO_HW_RENDER_INTERFACE_D3D9
		|| incoming->interface_version < RETRO_HW_RENDER_INTERFACE_D3D9_VERSION
		|| !incoming->device || !incoming->lock_context || !incoming->unlock_context
		|| !incoming->set_texture || !incoming->get_sync_index || !incoming->get_sync_index_mask
		|| !incoming->wait_sync_index) {
		ERROR_LOG(Log::G3D, "Missing or invalid libretro D3D9 render interface");
		return;
	}
	if (live_) {
		ContextDestroy();
	}
	iface_ = *incoming;
	live_ = true;
	LockGPU();
	CreateDrawContext();
	if (!draw_ || !draw_->CreatePresets()) {
		ERROR_LOG(Log::G3D, "Unable to create D3D9 drawing context");
		UnlockGPU();
		ContextDestroy();
		return;
	}
	if (gpu && lostGpu_ == gpu) {
		gpu->DeviceRestore(draw_);
	}
	lostGpu_ = nullptr;
	UnlockGPU();
}

void LibretroD3D9Context::FreeSlot(Slot &slot) {
	if (slot.depth) slot.depth->Release();
	if (slot.color) slot.color->Release();
	if (slot.texture) slot.texture->Release();
	slot = {};
}

bool LibretroD3D9Context::MakeSlot(Slot &slot, unsigned width, unsigned height) {
	if (slot.texture && slot.width == width && slot.height == height) {
		return true;
	}
	FreeSlot(slot);
	if (!width || !height
		|| FAILED(iface_.device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &slot.texture, nullptr))
		|| FAILED(slot.texture->GetSurfaceLevel(0, &slot.color))
		|| FAILED(iface_.device->CreateDepthStencilSurface(width, height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &slot.depth, nullptr))) {
		FreeSlot(slot);
		return false;
	}
	slot.width = width;
	slot.height = height;
	return true;
}

void LibretroD3D9Context::LostBackbuffer() {
	if (draw_ && boundSlot_ >= 0) {
		draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, -1, -1);
	}
	boundSlot_ = -1;
}

void LibretroD3D9Context::DestroyDrawContext() {
	LostBackbuffer();
	LibretroHWRenderContext::DestroyDrawContext();
	for (auto &slot : slots_) {
		FreeSlot(slot);
	}
	pD3Ddevice9.Reset();
	pD3DdeviceEx9.Reset();
	if (deviceEx_) deviceEx_->Release();
	if (d3dEx_) d3dEx_->Release();
	if (d3d_) d3d_->Release();
	deviceEx_ = nullptr;
	d3dEx_ = nullptr;
	d3d_ = nullptr;
	ptr_D3DCompile = nullptr;
}

void LibretroD3D9Context::ContextDestroy() {
	if (!live_) {
		return;
	}
	// A destroy callback can arrive from video_refresh, after SwapBuffers released the device.
	const bool held = frameSlot_ >= 0;
	if (!held) LockGPU();
	if (scene_) {
		iface_.device->EndScene();
		scene_ = false;
	}
	if (gpu && lostGpu_ != gpu) {
		gpu->DeviceLost();
		lostGpu_ = gpu;
	}
	DestroyDrawContext();
	frameSlot_ = -1;
	UnlockGPU();
	live_ = false;
	iface_ = {};
}

void LibretroD3D9Context::FrameBegin() {
	if (!live_ || !draw_ || frameSlot_ >= 0) return;
	const unsigned index = iface_.get_sync_index(iface_.handle);
	if (index >= 32 || !(iface_.get_sync_index_mask(iface_.handle) & (1u << index))) {
		ERROR_LOG(Log::G3D, "Invalid D3D9 sync index %u", index);
		return;
	}
	// The frontend must be able to finish its queued frame while we wait.
	iface_.wait_sync_index(iface_.handle);
	LockGPU();
	frameSlot_ = (int)index;
	Slot &slot = slots_[index];
	const unsigned width = PSP_CoreParameter().pixelWidth;
	const unsigned height = PSP_CoreParameter().pixelHeight;
	if ((slot.width != width || slot.height != height) && boundSlot_ == (int)index) {
		LostBackbuffer();
	}
	if (!MakeSlot(slot, width, height)) {
		ERROR_LOG(Log::G3D, "Failed to allocate D3D9 frame texture");
		return;
	}
	if (boundSlot_ != (int)index) {
		draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, width, height, slot.color, slot.depth);
		boundSlot_ = (int)index;
	}
	scene_ = SUCCEEDED(iface_.device->BeginScene());
}

void LibretroD3D9Context::SwapBuffers() {
	bool presented = false;
	if (frameSlot_ >= 0) {
		if (scene_) {
			iface_.device->EndScene();
			scene_ = false;
			if (boundSlot_ == frameSlot_) {
				iface_.set_texture(iface_.handle, slots_[frameSlot_].texture);
				draw_->HandleEvent(Draw::Event::PRESENTED, 0, 0);
				presented = true;
			}
		}
		frameSlot_ = -1;
		UnlockGPU();
	}
	video_cb(presented ? RETRO_HW_FRAME_BUFFER_VALID : nullptr, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight, 0);
}
