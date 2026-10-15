// Copyright (c) 2012- PPSSPP Project.

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
#include <wrl/client.h>

#include "Common/TimeUtil.h"
#include "Common/Data/Convert/ColorConv.h"
#include "Core/MemMap.h"
#include "GPU/ge_constants.h"

#include "GPU/GPUState.h"
#include "GPU/Directx9/TextureCacheDX9.h"
#include "GPU/Directx9/FramebufferManagerDX9.h"
#include "GPU/Directx9/ShaderManagerDX9.h"
#include "Common/GPU/D3D9/D3D9StateCache.h"
#include "GPU/Common/TextureShaderCommon.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/Common/TextureDecoder.h"
#include "Core/Config.h"

#include "ext/xxhash.h"
#include "Common/Math/math_util.h"

// PSP texture data is decoded into CPU memory, then converted to the native D3D9 layout on upload.

using Microsoft::WRL::ComPtr;

Draw::DataFormat FromD3D9Format(u32 fmt) {
	switch (fmt) {
	case D3DFMT_A4R4G4B4: return Draw::DataFormat::B4G4R4A4_UNORM_PACK16;
	case D3DFMT_A1R5G5B5: return Draw::DataFormat::A1R5G5B5_UNORM_PACK16;
	case D3DFMT_R5G6B5: return Draw::DataFormat::R5G6B5_UNORM_PACK16;
	case D3DFMT_A8: return Draw::DataFormat::R8_UNORM;
	case D3DFMT_DXT1: return Draw::DataFormat::BC1_RGBA_UNORM_BLOCK;
	case D3DFMT_DXT3: return Draw::DataFormat::BC2_UNORM_BLOCK;
	case D3DFMT_DXT5: return Draw::DataFormat::BC3_UNORM_BLOCK;
	case D3DFMT_A8R8G8B8: default: return Draw::DataFormat::R8G8B8A8_UNORM;
	}
}

D3DFORMAT ToD3D9Format(Draw::DataFormat fmt) {
	switch (fmt) {
	case Draw::DataFormat::BC1_RGBA_UNORM_BLOCK: return D3DFMT_DXT1;
	case Draw::DataFormat::BC2_UNORM_BLOCK: return D3DFMT_DXT3;
	case Draw::DataFormat::BC3_UNORM_BLOCK: return D3DFMT_DXT5;
	case Draw::DataFormat::R8G8B8A8_UNORM: return D3DFMT_A8R8G8B8;
	default: _dbg_assert_(false); return D3DFMT_A8R8G8B8;
	}
}

#define INVALID_TEX (LPDIRECT3DTEXTURE9)(-1)

static const D3DVERTEXELEMENT9 g_FramebufferVertexElements[] = {
	{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	D3DDECL_END()
};

TextureCacheDX9::TextureCacheDX9(Draw::DrawContext *draw, Draw2D *draw2D)
	: TextureCacheCommon(draw, draw2D) {
	lastBoundTexture = INVALID_TEX;
	device_ = (LPDIRECT3DDEVICE9)draw->GetNativeObject(Draw::NativeObject::DEVICE);
	deviceEx_ = (LPDIRECT3DDEVICE9EX)draw->GetNativeObject(Draw::NativeObject::DEVICE_EX);
}

TextureCacheDX9::~TextureCacheDX9() {
	Clear(true);
}

void TextureCacheDX9::InitDeviceObjects() {
	D3DCAPS9 pCaps;
	ZeroMemory(&pCaps, sizeof(pCaps));
	HRESULT result = 0;
	if (deviceEx_) {
		result = deviceEx_->GetDeviceCaps(&pCaps);
	} else {
		result = device_->GetDeviceCaps(&pCaps);
	}
	if (FAILED(result)) {
		WARN_LOG(Log::G3D, "Failed to get the device caps!");
		maxAnisotropyLevel = 16;
	} else {
		maxAnisotropyLevel = pCaps.MaxAnisotropy;
	}

	device_->CreateVertexDeclaration(g_FramebufferVertexElements, &pFramebufferVertexDecl);
}

void TextureCacheDX9::SetFramebufferManager(FramebufferManagerDX9 *fbManager) {
	framebufferManager_ = fbManager;
}

void TextureCacheDX9::ReleaseTexture(TexCacheEntry *entry, bool delete_them) {
	LPDIRECT3DBASETEXTURE9 &texture = DxTex(entry);
	if (texture) {
		texture->Release();
		texture = nullptr;
	}
}

void TextureCacheDX9::ForgetLastTexture() {
	lastBoundTexture = INVALID_TEX;
}

static D3DFORMAT getClutDestFormat(GEPaletteFormat format) {
	switch (format) {
	case GE_CMODE_16BIT_ABGR4444:
		return D3DFMT_A4R4G4B4;
	case GE_CMODE_16BIT_ABGR5551:
		return D3DFMT_A1R5G5B5;
	case GE_CMODE_16BIT_BGR5650:
		return D3DFMT_R5G6B5;
	case GE_CMODE_32BIT_ABGR8888:
		return D3DFMT_A8R8G8B8;
	}
	// Should never be here !
	return D3DFMT_A8R8G8B8;
}

void TextureCacheDX9::ApplySamplerByKey(const SamplerCacheKey &key) {
	device_->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, std::max(1, std::min((int)maxAnisotropyLevel, 1 << key.anisoLevel)));
	D3DTEXTUREFILTERTYPE minFilt = (key.aniso ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR);
	dxstate.texMinFilter.set(key.minFilt ? minFilt : D3DTEXF_POINT);
	dxstate.texMipFilter.set(key.mipFilt ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	dxstate.texMagFilter.set(key.magFilt ? D3DTEXF_LINEAR : D3DTEXF_POINT);

	// DX9 mip levels are .. odd. The "max level" sets the LARGEST mip to use.
	// We can enforce only the top mip level by setting a massive negative lod bias.

	if (!key.mipEnable) {
		dxstate.texMaxMipLevel.set(0);
		dxstate.texMipLodBias.set(-100.0f);
	} else {
		dxstate.texMipLodBias.set((float)key.lodBias / 256.0f);
		dxstate.texMaxMipLevel.set(key.minLevel / 256);
	}

	dxstate.texAddressU.set(key.sClamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
	dxstate.texAddressV.set(key.tClamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
}

void TextureCacheDX9::StartFrame() {
	TextureCacheCommon::StartFrame();

	if (gstate_c.Use(GPU_USE_ANISOTROPY)) {
		// Just take the opportunity to set the global aniso level here, once per frame.
		DWORD aniso = 1 << g_Config.iAnisotropyLevel;
		DWORD anisotropyLevel = aniso > maxAnisotropyLevel ? maxAnisotropyLevel : aniso;
		device_->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, anisotropyLevel);
	}
}

void TextureCacheDX9::BindTexture(TexCacheEntry *entry) {
	if (!entry) {
		device_->SetTexture(0, nullptr);
		lastBoundTexture = nullptr;
		return;
	}
	IDirect3DBaseTexture9 *texture = DxTex(entry);
	if (texture != lastBoundTexture) {
		device_->SetTexture(0, texture);
		lastBoundTexture = texture;
	}

}

void TextureCacheDX9::Unbind() {
	device_->SetTexture(0, nullptr);
	ForgetLastTexture();
}

void TextureCacheDX9::BindAsClutTexture(Draw::Texture *tex, bool smooth) {
	LPDIRECT3DBASETEXTURE9 clutTexture = (LPDIRECT3DBASETEXTURE9)draw_->GetNativeObject(Draw::NativeObject::TEXTURE_VIEW, tex);
	device_->SetTexture(1, clutTexture);
	device_->SetSamplerState(1, D3DSAMP_MINFILTER, smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	device_->SetSamplerState(1, D3DSAMP_MAGFILTER, smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	device_->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}

void TextureCacheDX9::BuildTexture(TexCacheEntry *const entry) {
	BuildTexturePlan plan;
	if (!PrepareBuildTexture(plan, entry)) {
		// We're screwed?
		return;
	}

	D3DFORMAT dstFmt = GetDestFormat(GETextureFormat(entry->format), gstate.getClutPaletteFormat());
	if (plan.doReplace) {
		dstFmt = ToD3D9Format(plan.replaced->Format());
	} else if (plan.scaleFactor > 1 || plan.saveTexture) {
		dstFmt = D3DFMT_A8R8G8B8;
	} else if (plan.decodeToClut8) {
		dstFmt = D3DFMT_A8;
	}

	int levels;

	LPDIRECT3DBASETEXTURE9 &texture = DxTex(entry);
	D3DPOOL pool = D3DPOOL_DEFAULT;
	int usage = D3DUSAGE_DYNAMIC;

	int tw;
	int th;
	plan.GetMipSize(0, &tw, &th);

	HRESULT hr;
	if (plan.depth == 1) {
		// We don't yet have mip generation, so clamp the number of levels to the ones we can load directly.
		levels = std::min(plan.levelsToCreate, plan.levelsToLoad);

		LPDIRECT3DTEXTURE9 tex = nullptr;
		hr = device_->CreateTexture(tw, th, levels, usage, dstFmt, pool, &tex, nullptr);
		texture = tex;
	} else {
		LPDIRECT3DVOLUMETEXTURE9 tex = nullptr;
		hr = device_->CreateVolumeTexture(tw, th, plan.depth, 1, usage, dstFmt, pool, &tex, nullptr);
		texture = tex;

		levels = 1;
	}

	if (FAILED(hr)) {
		INFO_LOG(Log::G3D, "Failed to create D3D texture: %dx%d", tw, th);
		ReleaseTexture(entry, true);
		return;
	}

	if (!texture) {
		// What to do here?
		return;
	}

	if (plan.depth == 1) {
		// Regular loop.
		for (int i = 0; i < levels; i++) {
			int dstLevel = i;
			HRESULT result;
			uint32_t lockFlag = dstLevel == 0 ? D3DLOCK_DISCARD : 0;  // Can only discard the top level
			D3DLOCKED_RECT rect{};

			result = ((LPDIRECT3DTEXTURE9)texture)->LockRect(dstLevel, &rect, NULL, lockFlag);
			if (FAILED(result)) {
				ERROR_LOG(Log::G3D, "Failed to lock D3D 2D texture at level %d: %dx%d", i, plan.w, plan.h);
				return;
			}
			uint8_t *data = (uint8_t *)rect.pBits;
			int stride = rect.Pitch;
			UploadTextureLevel(*entry, data, stride, plan, i, dstFmt);
			((LPDIRECT3DTEXTURE9)texture)->UnlockRect(dstLevel);
		}
	} else {
		// 3D loop.
		D3DLOCKED_BOX box;
		HRESULT result = ((LPDIRECT3DVOLUMETEXTURE9)texture)->LockBox(0, &box, nullptr, D3DLOCK_DISCARD);
		if (FAILED(result)) {
			ERROR_LOG(Log::G3D, "Failed to lock D3D 2D texture: %dx%dx%d", plan.w, plan.h, plan.depth);
			return;
		}

		uint8_t *data = (uint8_t *)box.pBits;
		int stride = box.RowPitch;
		for (int i = 0; i < plan.depth; i++) {
			UploadTextureLevel(*entry, data, stride, plan, i, dstFmt);
			data += box.SlicePitch;
		}
		((LPDIRECT3DVOLUMETEXTURE9)texture)->UnlockBox(0);
	}

	// Signal that we support depth textures so use it as one.
	if (plan.depth > 1) {
		entry->status |= TexStatus::IS_3D;
	}

	if (levels == 1) {
		entry->status |= TexStatus::NO_MIPS;
	} else {
		entry->status &= ~TexStatus::NO_MIPS;
	}
	if (plan.doReplace) {
		entry->SetAlphaStatus(plan.replaced->AlphaStatus());
	}
}

D3DFORMAT TextureCacheDX9::GetDestFormat(GETextureFormat format, GEPaletteFormat clutFormat) const {
	switch (format) {
	case GE_TFMT_CLUT4:
	case GE_TFMT_CLUT8:
	case GE_TFMT_CLUT16:
	case GE_TFMT_CLUT32:
		return getClutDestFormat(clutFormat);
	case GE_TFMT_4444:
		return D3DFMT_A4R4G4B4;
	case GE_TFMT_5551:
		return D3DFMT_A1R5G5B5;
	case GE_TFMT_5650:
		return D3DFMT_R5G6B5;
	case GE_TFMT_8888:
	case GE_TFMT_DXT1:
	case GE_TFMT_DXT3:
	case GE_TFMT_DXT5:
	default:
		return D3DFMT_A8R8G8B8;
	}
}

bool TextureCacheDX9::GetCurrentTextureDebug(GPUDebugBuffer &buffer, int level, bool *isFramebuffer) {
	TextureApplyResult textureResult = ApplyTexture(true);
	if (textureResult.framebuffer) {
		*isFramebuffer = true;
		return GetFramebufferTextureDebug(textureResult.framebuffer, textureResult.framebufferTextureChannel, buffer);
	}
	if (!textureResult.texCacheEntry) {
		return false;
	}

	ComPtr<IDirect3DBaseTexture9> baseTex;
	ComPtr<IDirect3DTexture9> tex;
	ComPtr<IDirect3DSurface9> offscreen;
	HRESULT hr;

	bool success = false;
	hr = device_->GetTexture(0, &baseTex);
	if (SUCCEEDED(hr) && baseTex != NULL) {
		hr = baseTex.As(&tex);
		if (SUCCEEDED(hr)) {
			D3DSURFACE_DESC desc;
			D3DLOCKED_RECT locked;
			tex->GetLevelDesc(level, &desc);
			RECT rect = { 0, 0, (LONG)desc.Width, (LONG)desc.Height };
			hr = tex->LockRect(level, &locked, &rect, D3DLOCK_READONLY);

			// If it fails, this means it's a render-to-texture, so we have to get creative.
			if (FAILED(hr)) {
				ComPtr<IDirect3DSurface9> renderTarget;
				hr = tex->GetSurfaceLevel(level, &renderTarget);
				if (renderTarget && SUCCEEDED(hr)) {
					hr = device_->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &offscreen, NULL);
					if (SUCCEEDED(hr)) {
						hr = device_->GetRenderTargetData(renderTarget.Get(), offscreen.Get());
						if (SUCCEEDED(hr)) {
							hr = offscreen->LockRect(&locked, &rect, D3DLOCK_READONLY);
						}
					}
				}
				*isFramebuffer = true;
			} else {
				*isFramebuffer = false;
			}

			if (SUCCEEDED(hr)) {
				GPUDebugBufferFormat fmt;
				int pixelSize;
				switch (desc.Format) {
				case D3DFMT_A1R5G5B5:
					fmt = GPU_DBG_FORMAT_5551_BGRA;
					pixelSize = 2;
					break;
				case D3DFMT_A4R4G4B4:
					fmt = GPU_DBG_FORMAT_4444_BGRA;
					pixelSize = 2;
					break;
				case D3DFMT_R5G6B5:
					fmt = GPU_DBG_FORMAT_565_BGRA;
					pixelSize = 2;
					break;
				case D3DFMT_A8R8G8B8:
					fmt = GPU_DBG_FORMAT_8888_BGRA;
					pixelSize = 4;
					break;
				default:
					fmt = GPU_DBG_FORMAT_INVALID;
					break;
				}

				if (fmt != GPU_DBG_FORMAT_INVALID) {
					buffer.Allocate(locked.Pitch / pixelSize, desc.Height, fmt, false);
					memcpy(buffer.GetData(), locked.pBits, locked.Pitch * desc.Height);
					success = true;
				} else {
					success = false;
				}
				if (offscreen) {
					offscreen->UnlockRect();
				} else {
					tex->UnlockRect(level);
				}
			}
		}
	}

	return success;
}

void *TextureCacheDX9::GetNativeTextureView(const TexCacheEntry *entry, bool flat) const {
	LPDIRECT3DBASETEXTURE9 tex = DxTex(entry);
	return (void *)tex;
}

void TextureCacheDX9::DeviceLost() {
	Clear(true);
	pFramebufferVertexDecl.Reset();
	device_.Reset();
	deviceEx_.Reset();
	draw_ = nullptr;
}

void TextureCacheDX9::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	device_ = (LPDIRECT3DDEVICE9)draw->GetNativeObject(Draw::NativeObject::DEVICE);
	deviceEx_ = (LPDIRECT3DDEVICE9EX)draw->GetNativeObject(Draw::NativeObject::DEVICE_EX);
	InitDeviceObjects();
	ForgetLastTexture();
}

void TextureCacheDX9::UploadTextureLevel(TexCacheEntry &entry, uint8_t *dest, int pitch, BuildTexturePlan &plan, int level, D3DFORMAT format) {
	int width, height;
	plan.GetMipSize(plan.depth > 1 ? 0 : level, &width, &height);
	const bool compressed = format == D3DFMT_DXT1 || format == D3DFMT_DXT3 || format == D3DFMT_DXT5;
	const int rows = compressed ? (height + 3) / 4 : height;
	const size_t bytes = (size_t)pitch * rows;
	uploadBuffer_.resize(bytes);
	LoadTextureLevel(entry, uploadBuffer_.data(), bytes, pitch, plan, level == 0 ? plan.baseLevelSrc : level, FromD3D9Format(format), TexDecodeFlags{});
	for (int y = 0; y < rows; y++) {
		const uint8_t *src = uploadBuffer_.data() + y * pitch;
		uint8_t *out = dest + y * pitch;
		if (format == D3DFMT_A8R8G8B8) {
			ConvertRGBA8888ToBGRA8888((u32 *)out, (const u32 *)src, width);
		} else if (format == D3DFMT_A4R4G4B4 || format == D3DFMT_A1R5G5B5 || format == D3DFMT_R5G6B5) {
			for (int x = 0; x < width; x++) {
				const u16 value = ((const u16 *)src)[x];
				if (format == D3DFMT_A4R4G4B4) {
					((u16 *)out)[x] = (value & 0xF0F0) | ((value & 0xF) << 8) | ((value >> 8) & 0xF);
				} else if (format == D3DFMT_A1R5G5B5) {
					((u16 *)out)[x] = (value & 0x83E0) | ((value & 0x1F) << 10) | ((value >> 10) & 0x1F);
				} else {
					((u16 *)out)[x] = (value & 0x7E0) | ((value & 0x1F) << 11) | ((value >> 11) & 0x1F);
				}
			}
		} else {
			memcpy(out, src, compressed ? ((width + 3) / 4) * (format == D3DFMT_DXT1 ? 8 : 16) : width);
		}
	}
}
