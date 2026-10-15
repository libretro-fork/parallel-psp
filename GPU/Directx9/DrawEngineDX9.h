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

#pragma once

#include <d3d9.h>
#include <wrl/client.h>

#include "Common/Data/Collections/Hashmaps.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/MiscTypes.h"

struct DecVtxFormat;
struct UVScale;

class VSShader;
class ShaderManagerDX9;
class TextureCacheDX9;
class FramebufferManagerDX9;

// Handles transform, lighting and drawing.
class DrawEngineDX9 : public DrawEngineCommon {
public:
	DrawEngineDX9(Draw::DrawContext *draw);
	~DrawEngineDX9();

	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

	void SetShaderManager(ShaderManagerDX9 *shaderManager) {
		shaderManager_ = shaderManager;
	}
	void SetTextureCache(TextureCacheDX9 *textureCache) {
		textureCache_ = textureCache;
	}
	void SetFramebufferManager(FramebufferManagerDX9 *fbManager) {
		framebufferManager_ = fbManager;
	}
	void InitDeviceObjects();
	void DestroyDeviceObjects();

	void BeginFrame() override;

	// So that this can be inlined
	void Flush() override;

	void FinishDeferred() {
		DecodeVerts(dec_, decoded_);
	}

private:
	void Invalidate(InvalidationCallbackFlags flags);

	bool Submit(D3DPRIMITIVETYPE prim, const void *vertices, int vertexCount, int stride, const u16 *indices, int indexCount);
	void ApplyDrawState(int prim);
	void ApplyDrawStateLate(bool setStencil, uint8_t stencilValue);

	HRESULT SetupDecFmtForDraw(const DecVtxFormat &decFmt, u32 pspFmt, IDirect3DVertexDeclaration9 **ppVertexDeclaration);

	Microsoft::WRL::ComPtr<IDirect3DVertexBuffer9> vertexBuffer_;
	Microsoft::WRL::ComPtr<IDirect3DIndexBuffer9> indexBuffer_;
	unsigned vertexOffset_ = 0;
	unsigned indexOffset_ = 0;
	unsigned vertexCapacity_ = 0;
	unsigned indexCapacity_ = 0;
	LPDIRECT3DDEVICE9 device_ = nullptr;
	Draw::DrawContext *draw_;

	DenseHashMap<u32, Microsoft::WRL::ComPtr<IDirect3DVertexDeclaration9>> vertexDeclMap_;

	// SimpleVertex
	Microsoft::WRL::ComPtr<IDirect3DVertexDeclaration9> transformedVertexDecl_;

	// Other
	ShaderManagerDX9 *shaderManager_ = nullptr;
	TextureCacheDX9 *textureCache_ = nullptr;
	FramebufferManagerDX9 *framebufferManager_ = nullptr;

	ClipInfoFlags lastClipInfoFlags_ = (ClipInfoFlags)0;
	bool lastUseHwTransform_ = false;

	FBOTexState fboTexBindState_ = FBO_TEX_NONE;

	int lastRenderStepId_ = -1;

	bool fboTexNeedsBind_ = false;
};
