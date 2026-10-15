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
#include <wrl/client.h>

#include "Common/Log.h"
#include "Common/Profiler/Profiler.h"

#include "Common/GPU/D3D9/D3D9StateCache.h"

#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"

#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/TransformCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/SoftwareTransformCommon.h"
#include "GPU/Directx9/TextureCacheDX9.h"
#include "GPU/Directx9/DrawEngineDX9.h"
#include "GPU/Directx9/ShaderManagerDX9.h"
#include "GPU/Directx9/FramebufferManagerDX9.h"

using Microsoft::WRL::ComPtr;

static const D3DPRIMITIVETYPE d3d_prim[8] = {
	// Points, which are expanded to triangles.
	D3DPT_TRIANGLELIST,
	// Lines and line strips, which are also expanded to triangles.
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLESTRIP,
	D3DPT_TRIANGLEFAN,
	// Rectangles, which are expanded to triangles.
	D3DPT_TRIANGLELIST,
};

static const int D3DPRIMITIVEVERTEXCOUNT[8][2] = {
	{0, 0}, // invalid
	{1, 0}, // 1 = D3DPT_POINTLIST,
	{2, 0}, // 2 = D3DPT_LINELIST,
	{2, 1}, // 3 = D3DPT_LINESTRIP,
	{3, 0}, // 4 = D3DPT_TRIANGLELIST,
	{1, 2}, // 5 = D3DPT_TRIANGLESTRIP,
	{1, 2}, // 6 = D3DPT_TRIANGLEFAN,
};

inline int D3DPrimCount(D3DPRIMITIVETYPE prim, int size) {
	return (size / D3DPRIMITIVEVERTEXCOUNT[prim][0]) - D3DPRIMITIVEVERTEXCOUNT[prim][1];
}

enum {
	TRANSFORMED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * sizeof(TransformedVertex)
};

static const D3DVERTEXELEMENT9 TransformedVertexElements[] = {
	{ 0, offsetof(TransformedVertex, pos), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, offsetof(TransformedVertex, uv), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	{ 0, offsetof(TransformedVertex, color0_32), D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
	{ 0, offsetof(TransformedVertex, color1_32), D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1 },
	{ 0, offsetof(TransformedVertex, fog), D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
	D3DDECL_END()
};

DrawEngineDX9::DrawEngineDX9(Draw::DrawContext *draw) : draw_(draw), vertexDeclMap_(64) {
	device_ = (LPDIRECT3DDEVICE9)draw->GetNativeObject(Draw::NativeObject::DEVICE);
	decOptions_.expand8BitNormalsToFloat = true;

	InitDeviceObjects();


}

DrawEngineDX9::~DrawEngineDX9() {
	DestroyDeviceObjects();
	vertexDeclMap_.Clear();
}

void DrawEngineDX9::InitDeviceObjects() {
	device_->CreateVertexDeclaration(TransformedVertexElements, &transformedVertexDecl_);
	draw_->SetInvalidationCallback(std::bind(&DrawEngineDX9::Invalidate, this, std::placeholders::_1));
}

void DrawEngineDX9::DestroyDeviceObjects() {
	vertexBuffer_.Reset();
	indexBuffer_.Reset();
	vertexOffset_ = indexOffset_ = vertexCapacity_ = indexCapacity_ = 0;
	transformedVertexDecl_.Reset();
	vertexDeclMap_.Clear();
	if (draw_) {
		draw_->SetInvalidationCallback(InvalidationCallback());
	}
}

struct DeclTypeInfo {
	u32 type;
	const char * name;
};

static const DeclTypeInfo VComp[] = {
	{ 0, "NULL" }, // DEC_NONE,
	{ D3DDECLTYPE_FLOAT1, "D3DDECLTYPE_FLOAT1 " },  // DEC_FLOAT_1,
	{ D3DDECLTYPE_FLOAT2, "D3DDECLTYPE_FLOAT2 " },  // DEC_FLOAT_2,
	{ D3DDECLTYPE_FLOAT3, "D3DDECLTYPE_FLOAT3 " },  // DEC_FLOAT_3,
	{ D3DDECLTYPE_FLOAT4, "D3DDECLTYPE_FLOAT4 " },  // DEC_FLOAT_4,

	{ 0, "UNUSED" }, // DEC_S8_3,

	{ D3DDECLTYPE_SHORT4N, "D3DDECLTYPE_SHORT4N	" },	// DEC_S16_3,
	{ D3DDECLTYPE_UBYTE4N, "D3DDECLTYPE_UBYTE4N	" },	// DEC_U8_1,
	{ D3DDECLTYPE_UBYTE4N, "D3DDECLTYPE_UBYTE4N	" },	// DEC_U8_2,
	{ D3DDECLTYPE_UBYTE4N, "D3DDECLTYPE_UBYTE4N	" },	// DEC_U8_3,
	{ D3DDECLTYPE_UBYTE4N, "D3DDECLTYPE_UBYTE4N	" },	// DEC_U8_4,
	{0, "UNUSED_DEC_U16_1" },	// 	DEC_U16_1,
	{0, "UNUSED_DEC_U16_2" },	// 	DEC_U16_2,
	{D3DDECLTYPE_USHORT4N	,"D3DDECLTYPE_USHORT4N "}, // DEC_U16_3,
	{D3DDECLTYPE_USHORT4N	,"D3DDECLTYPE_USHORT4N "}, // DEC_U16_4,
};

static void VertexAttribSetup(D3DVERTEXELEMENT9 * VertexElement, u8 fmt, u8 offset, u8 usage, u8 usage_index = 0) {
	memset(VertexElement, 0, sizeof(D3DVERTEXELEMENT9));
	VertexElement->Offset = offset;
	VertexElement->Type = VComp[fmt].type;
	VertexElement->Usage = usage;
	VertexElement->UsageIndex = usage_index;
}

HRESULT DrawEngineDX9::SetupDecFmtForDraw(const DecVtxFormat &decFmt, u32 pspFmt, IDirect3DVertexDeclaration9 **ppVertextDeclaration) {
	ComPtr<IDirect3DVertexDeclaration9> vertexDeclCached;
	if (vertexDeclMap_.Get(pspFmt, &vertexDeclCached)) {
		*ppVertextDeclaration = vertexDeclCached.Detach();
		return S_OK;
	} else {
		D3DVERTEXELEMENT9 VertexElements[8];
		D3DVERTEXELEMENT9 *VertexElement = &VertexElements[0];

		// Vertices Elements orders
		// WEIGHT
		if (decFmt.w0fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.w0fmt, decFmt.w0off, D3DDECLUSAGE_TEXCOORD, 1);
			VertexElement++;
		}

		if (decFmt.w1fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.w1fmt, decFmt.w1off, D3DDECLUSAGE_TEXCOORD, 2);
			VertexElement++;
		}

		// TC
		if (decFmt.uvfmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.uvfmt, decFmt.uvoff, D3DDECLUSAGE_TEXCOORD, 0);
			VertexElement++;
		}

		// COLOR
		if (decFmt.c0fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.c0fmt, decFmt.c0off, D3DDECLUSAGE_COLOR, 0);
			VertexElement++;
		}
		// Never used ?
		if (decFmt.c1fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.c1fmt, decFmt.c1off, D3DDECLUSAGE_COLOR, 1);
			VertexElement++;
		}

		// NORMAL
		if (decFmt.nrmfmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.nrmfmt, decFmt.nrmoff, D3DDECLUSAGE_NORMAL, 0);
			VertexElement++;
		}

		// POSITION
		// Always
		VertexAttribSetup(VertexElement, DecVtxFormat::PosFmt(), decFmt.posoff, D3DDECLUSAGE_POSITION, 0);
		VertexElement++;

		// End
		D3DVERTEXELEMENT9 end = D3DDECL_END();
		memcpy(VertexElement, &end, sizeof(D3DVERTEXELEMENT9));

		// Create declaration
		ComPtr<IDirect3DVertexDeclaration9> pHardwareVertexDecl;
		HRESULT hr = device_->CreateVertexDeclaration( VertexElements, &pHardwareVertexDecl );
		if (FAILED(hr)) {
			ERROR_LOG(Log::G3D, "Failed to create vertex declaration!");
		}

		// Add it to map
		vertexDeclMap_.Insert(pspFmt, pHardwareVertexDecl);
		*ppVertextDeclaration = pHardwareVertexDecl.Detach();
		return hr;
	}
}

static uint32_t SwapRB(uint32_t c) {
	return (c & 0xFF00FF00) | ((c >> 16) & 0xFF) | ((c << 16) & 0xFF0000);
}

void DrawEngineDX9::BeginFrame() {
	DrawEngineCommon::BeginFrame();
	lastRenderStepId_ = -1;
}

// In D3D, we're synchronous and state carries over so all we reset here on a new step is the viewport/scissor.
void DrawEngineDX9::Invalidate(InvalidationCallbackFlags flags) {
	if (flags & InvalidationCallbackFlags::COMMAND_BUFFER_STATE) {
		// D3D9 constant registers and render state are shared with thin3d draws.
		gstate_c.Dirty(DIRTY_ALL_UNIFORMS | DIRTY_ALL_RENDER_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE);
		if (textureCache_) textureCache_->ForgetLastTexture();
	}
	if (flags & InvalidationCallbackFlags::RENDER_PASS_STATE) {
		gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
	}
}

void DrawEngineDX9::Flush() {
	if (!numDrawVerts_) {
		return;
	}

	// This is not done on every drawcall, we collect vertex data
	// until critical state changes. That's when we draw (flush).

	GEPrimitiveType prim = prevPrim_;

	// Always use software for flat shading to fix the provoking index.
	bool useHWTransform = CanUseHardwareTransform(prim) && gstate.getShadeMode() != GE_SHADE_FLAT;
	if (clipInfoFlags_ & ClipInfoFlags::Valid) {
		if (clipInfoFlags_ & ClipInfoFlags::SoftClipCull) {
			useHWTransform = false;
		}
	}
	if (clipInfoFlags_ != lastClipInfoFlags_) {
		ClipInfoFlags changed = (ClipInfoFlags)((u32)clipInfoFlags_ ^ (u32)lastClipInfoFlags_);
		if (changed & (ClipInfoFlags::DepthClampFragment | ClipInfoFlags::MinMaxZDiscard)) {
			gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE);
		}
		if (changed & ClipInfoFlags::FlatZ) {
			gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
		}
		lastClipInfoFlags_ = clipInfoFlags_;
	}

	if (useHWTransform != lastUseHwTransform_) {
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE);
		lastUseHwTransform_ = useHWTransform;
	}

	if (useHWTransform) {

		int vertexCount;
		int maxIndex;
		bool useElements;
		DecodeVerts(dec_, decoded_);
		DecodeIndsAndGetData(&prim, &vertexCount, &maxIndex, &useElements, false);
		gpuStats.perFrame.numVertsDrawn += vertexCount;

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
			gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
			TextureApplyResult textureResult = textureCache_->ApplyTexture(true);
			textureCache_->ApplySampler(textureResult, clipInfoFlags_ & ClipInfoFlags::FlatZ, false);
		} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
			// This catches the case of clearing a texture. (#10957)
			gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
		}

		// Need to ApplyDrawState after ApplyTexture because depal can launch a render pass and that wrecks the state.
		ApplyDrawState(prim);
		ApplyDrawStateLate(false, 0);
		VSShader *vshader = shaderManager_->ApplyShader(true, dec_->VertexType(), pipelineState_, clipInfoFlags_, false, framebufferManager_->UseBufferedRendering());
		if (!vshader) {
			goto bail;
		}
		ComPtr<IDirect3DVertexDeclaration9> declaration;
		if (FAILED(SetupDecFmtForDraw(dec_->GetDecVtxFmt(), dec_->VertexType(), &declaration))) {
			goto bail;
		}
		device_->SetVertexDeclaration(declaration.Get());
		const D3DPRIMITIVETYPE nativePrim = d3d_prim[prim];
		Submit(nativePrim, decoded_, useElements ? maxIndex + 1 : vertexCount, dec_->GetDecVtxFmt().stride, useElements ? decIndex_ : nullptr, vertexCount);
		if (useDepthRaster_) {
			DepthRasterSubmitRaw(prim, dec_, dec_->VertexType(), vertexCount);
		}
	} else {
		PROFILE_THIS_SCOPE("soft");
		DecodeVerts(dec_, decoded_);
		int vertexCount = DecodeInds();

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		prim = IndexGenerator::GeneralPrim((GEPrimitiveType)drawInds_[0].prim);
		VERBOSE_LOG(Log::G3D, "Flush prim %i SW! %i verts in one go", prim, vertexCount);


		u16 *inds = decIndex_;
		if (gstate.getShadeMode() == GE_SHADE_FLAT) {
			// We need to rotate the index buffer to simulate a different provoking vertex.
			// We do this before line expansion etc.
			IndexBufferProvokingLastToFirst(prim, inds, vertexCount);
		}

		// At this point, rect and line primitives are still preserved as such. So, it's the best time to do software depth raster.
		// We could piggyback on the viewport transform below, but it gets complicated since it's different per-backend. Which we really
		// should clean up one day...
		if (useDepthRaster_) {
			DepthRasterPredecoded(prim, decoded_, numDecodedVerts_, dec_, vertexCount);
		}

		bool textureNeedsApply = false;
		TextureApplyResult textureResult;
		if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
			gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
			gstate_c.dstSquared = false;
			textureResult = textureCache_->ApplyTexture(true);
			textureNeedsApply = true;
		} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
			// This catches the case of clearing a texture. (#10957)
			gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
		}

		SoftwareTransformResult result{};
		SoftwareTransformParams params{};
		params.everUsedEqualDepth = everUsedEqualDepth_;
		params.decoded = decoded_;
		params.transformed = transformed_;
		params.transformedExpanded = transformedExpanded_;
		params.allowClear = true;
		params.allowSeparateAlphaClear = false;  // D3D9 doesn't support separate alpha clears
		params.clipInfoFlags = clipInfoFlags_;

		const SoftwareTransformAction action = RunSoftwareTransform(params, prim, dec_->VertexType(), dec_->GetDecVtxFmt(), numDecodedVerts_, VERTEX_BUFFER_MAX, vertexCount, inds, RemainingIndices(inds), &result);

		// TODO: This should be after BuildDrawingParams!
		if (textureNeedsApply) {
			textureCache_->ApplySampler(textureResult, clipInfoFlags_ & ClipInfoFlags::FlatZ, result.pixelMapped);
		}

		// Need to ApplyDrawState after ApplyTexture because depal can launch a render pass and that wrecks the state.
		ApplyDrawState(prim);
		ApplyDrawStateLate(result.setStencil, result.stencilValue);

		if (action == SW_DRAW_INDEXED) {
			VSShader *vshader = shaderManager_->ApplyShader(false, dec_->VertexType(), pipelineState_, clipInfoFlags_, result.pixelMapped, framebufferManager_->UseBufferedRendering());
			if (!vshader) {
				goto bail;
			}
			device_->SetVertexDeclaration(transformedVertexDecl_.Get());
			Submit(d3d_prim[prim], result.drawBuffer, result.drawVertexCount, sizeof(TransformedVertex), inds, result.drawIndexCount);
			gpuStats.perFrame.numVertsDrawn += result.drawIndexCount;
		} else if (action == SW_CLEAR) {
			u32 clearColor = result.color;
			float clearDepth = result.depth;

			Draw::Aspect clearFlag = Draw::Aspect::NO_BIT;

			if (gstate.isClearModeColorMask()) clearFlag |= Draw::Aspect::COLOR_BIT;
			if (gstate.isClearModeAlphaMask()) clearFlag |= Draw::Aspect::STENCIL_BIT;
			if (gstate.isClearModeDepthMask()) clearFlag |= Draw::Aspect::DEPTH_BIT;

			uint8_t clearStencil = clearColor >> 24;
			draw_->Clear(clearFlag, clearColor, clearDepth, clearStencil);

			if (gstate_c.Use(GPU_USE_CLEAR_RAM_HACK) && gstate.isClearModeColorMask() && (gstate.isClearModeAlphaMask() || gstate_c.framebufFormat == GE_FORMAT_565)) {
				int scissorX1 = gstate.getScissorX1();
				int scissorY1 = gstate.getScissorY1();
				int scissorX2 = gstate.getScissorX2() + 1;
				int scissorY2 = gstate.getScissorY2() + 1;
				framebufferManager_->ApplyClearToMemory(scissorX1, scissorY1, scissorX2, scissorY2, clearColor);
			}
		}
	}

bail:
	ResetAfterDrawInline();
	framebufferManager_->SetColorUpdated(gstate_c.skipDrawReason);
	gpuCommon_->NotifyFlush();
}

void DrawEngineDX9::DeviceLost() {
	DestroyDeviceObjects();
	draw_ = nullptr;
	device_ = nullptr;
}

void DrawEngineDX9::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	device_ = (LPDIRECT3DDEVICE9)draw->GetNativeObject(Draw::NativeObject::DEVICE);
	InitDeviceObjects();
}

bool DrawEngineDX9::Submit(D3DPRIMITIVETYPE prim, const void *vertices, int vertexCount, int stride, const u16 *indices, int indexCount) {
	const int primitiveCount = D3DPrimCount(prim, indexCount);
	if (primitiveCount <= 0 || vertexCount <= 0) return false;
	const unsigned vertexBytes = vertexCount * stride;
	const unsigned indexBytes = indices ? indexCount * sizeof(u16) : 0;
	if (vertexBytes > vertexCapacity_) {
		vertexBuffer_.Reset();
		vertexCapacity_ = std::max(vertexBytes, 4u * 1024u * 1024u);
		vertexOffset_ = 0;
		if (FAILED(device_->CreateVertexBuffer(vertexCapacity_, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vertexBuffer_, nullptr))) {
			vertexCapacity_ = 0;
			return false;
		}
	}
	if (indexBytes > indexCapacity_) {
		indexBuffer_.Reset();
		indexCapacity_ = std::max(indexBytes, 512u * 1024u);
		indexOffset_ = 0;
		if (FAILED(device_->CreateIndexBuffer(indexCapacity_, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indexBuffer_, nullptr))) {
			indexCapacity_ = 0;
			return false;
		}
	}
	vertexOffset_ = (vertexOffset_ + 15) & ~15u;
	if (vertexOffset_ + vertexBytes > vertexCapacity_) vertexOffset_ = 0;
	if (indexOffset_ + indexBytes > indexCapacity_) indexOffset_ = 0;
	void *dest;
	if (FAILED(vertexBuffer_->Lock(vertexOffset_, vertexBytes, &dest, vertexOffset_ ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD))) return false;
	memcpy(dest, vertices, vertexBytes);
	vertexBuffer_->Unlock();
	device_->SetStreamSource(0, vertexBuffer_.Get(), vertexOffset_, stride);
	vertexOffset_ += vertexBytes;
	HRESULT result;
	if (indices) {
		if (FAILED(indexBuffer_->Lock(indexOffset_, indexBytes, &dest, indexOffset_ ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD))) return false;
		memcpy(dest, indices, indexBytes);
		indexBuffer_->Unlock();
		device_->SetIndices(indexBuffer_.Get());
		result = device_->DrawIndexedPrimitive(prim, 0, 0, vertexCount, indexOffset_ / sizeof(u16), primitiveCount);
		indexOffset_ += indexBytes;
	} else {
		result = device_->DrawPrimitive(prim, 0, primitiveCount);
	}
	return SUCCEEDED(result);
}
