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

#ifdef _WIN32
//#define SHADERLOG
#endif

#include <cmath>
#include <map>

#include "Common/Data/Convert/SmallDataConvert.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Math/lin/matrix4x4.h"
#include "Common/Math/math_util.h"
#include "Common/GPU/D3D9/D3D9ShaderCompiler.h"
#include "Common/GPU/thin3d.h"
#include "Common/System/OSD.h"
#include "Common/System/Display.h"

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"

#include "Core/Config.h"
#include "GPU/Math3D.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/ShaderUniforms.h"
#include "GPU/Common/FragmentShaderGenerator.h"
#include "GPU/Directx9/ShaderManagerDX9.h"
#include "GPU/Directx9/ShaderConstantsDX9.h"
#include "GPU/Directx9/DrawEngineDX9.h"
#include "GPU/Directx9/FramebufferManagerDX9.h"

using namespace Lin;

PSShader::PSShader(LPDIRECT3DDEVICE9 device, FShaderID id, const char *code) : id_(id) {
	source_ = code;
#ifdef SHADERLOG
	OutputDebugString(ConvertUTF8ToWString(code).c_str());
#endif
	bool success;
	std::string errorMessage;

	success = CompilePixelShaderD3D9(device, code, &shader, &errorMessage);

	if (!errorMessage.empty()) {
		if (success) {
			ERROR_LOG(Log::G3D, "Warnings in shader compilation!");
		} else {
			ERROR_LOG(Log::G3D, "Error in shader compilation!");
		}
		ERROR_LOG(Log::G3D, "Messages: %s", errorMessage.c_str());
		ERROR_LOG(Log::G3D, "Shader source:\n%s", LineNumberString(code).c_str());
		OutputDebugStringUTF8("Messages:\n");
		OutputDebugStringUTF8(errorMessage.c_str());

	}

	if (!success) {
		failed_ = true;
		shader = nullptr;
		return;
	} else {
		VERBOSE_LOG(Log::G3D, "Compiled pixel shader:\n%s\n", (const char *)code);
	}
}

PSShader::~PSShader() {
}

std::string PSShader::GetShaderString(DebugShaderStringType type) const {
	switch (type) {
	case SHADER_STRING_SOURCE_CODE:
		return source_;
	case SHADER_STRING_SHORT_DESC:
		return id_.Description();
	default:
		return "N/A";
	}
}

VSShader::VSShader(LPDIRECT3DDEVICE9 device, VShaderID id, const char *code, bool useHWTransform) : useHWTransform_(useHWTransform), id_(id) {
	source_ = code;
#ifdef SHADERLOG
	OutputDebugString(ConvertUTF8ToWString(code).c_str());
#endif
	bool success;
	std::string errorMessage;

	success = CompileVertexShaderD3D9(device, code, &shader, &errorMessage);
	if (!errorMessage.empty()) {
		if (success) {
			ERROR_LOG(Log::G3D, "Warnings in shader compilation!");
		} else {
			ERROR_LOG(Log::G3D, "Error in shader compilation!");
		}
		ERROR_LOG(Log::G3D, "Messages: %s", errorMessage.c_str());
		ERROR_LOG(Log::G3D, "Shader source:\n%s", code);
		OutputDebugStringUTF8("Messages:\n");
		OutputDebugStringUTF8(errorMessage.c_str());

	}

	if (!success) {
		failed_ = true;
		shader = nullptr;
		return;
	} else {
		VERBOSE_LOG(Log::G3D, "Compiled vertex shader:\n%s\n", (const char *)code);
	}
}

VSShader::~VSShader() {
}

std::string VSShader::GetShaderString(DebugShaderStringType type) const {
	switch (type) {
	case SHADER_STRING_SOURCE_CODE:
		return source_;
	case SHADER_STRING_SHORT_DESC:
		return id_.Description();
	default:
		return "N/A";
	}
}

void ShaderManagerDX9::PSSetColorUniform3(int creg, u32 color) {
	float f[4];
	Uint8x3ToFloat4(f, color);
	device_->SetPixelShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::PSSetColorUniform3Alpha255(int creg, u32 color, u8 alpha) {
	const float col[4] = {
		(float)((color & 0xFF)),
		(float)((color & 0xFF00) >> 8),
		(float)((color & 0xFF0000) >> 16),
		(float)alpha,
	};
	device_->SetPixelShaderConstantF(creg, col, 1);
}

void ShaderManagerDX9::PSSetFloat(int creg, float value) {
	const float f[4] = { value, 0.0f, 0.0f, 0.0f };
	device_->SetPixelShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::PSSetFloatArray(int creg, const float *value, int count) {
	float f[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < count; i++) {
		f[i] = value[i];
	}
	device_->SetPixelShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::VSSetFloat(int creg, float value) {
	const float f[4] = { value, 0.0f, 0.0f, 0.0f };
	device_->SetVertexShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::VSSetFloatArray(int creg, const float *value, int count) {
	float f[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < count; i++) {
		f[i] = value[i];
	}
	device_->SetVertexShaderConstantF(creg, f, 1);
}

// Utility
void ShaderManagerDX9::VSSetColorUniform3(int creg, u32 color) {
	float f[4];
	Uint8x3ToFloat4(f, color);
	device_->SetVertexShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::VSSetFloatUniform4(int creg, const float data[4]) {
	device_->SetVertexShaderConstantF(creg, data, 1);
}

void ShaderManagerDX9::VSSetFloat24Uniform3(int creg, const u32 data[3]) {
	float f[4];
	ExpandFloat24x3ToFloat4(f, data);
	device_->SetVertexShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::VSSetFloat24Uniform3Normalized(int creg, const u32 data[3]) {
	float f[4];
	ExpandFloat24x3ToFloat4AndNormalize(f, data);
	device_->SetVertexShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::VSSetColorUniform3Alpha(int creg, u32 color, u8 alpha) {
	float f[4];
	Uint8x3ToFloat4_AlphaUint8(f, color, alpha);
	device_->SetVertexShaderConstantF(creg, f, 1);
}

void ShaderManagerDX9::VSSetColorUniform3ExtraFloat(int creg, u32 color, float extra) {
	const float col[4] = {
		((color & 0xFF)) / 255.0f,
		((color & 0xFF00) >> 8) / 255.0f,
		((color & 0xFF0000) >> 16) / 255.0f,
		extra
	};
	device_->SetVertexShaderConstantF(creg, col, 1);
}

void ShaderManagerDX9::VSSetMatrix4x3_3(int creg, const float *m4x3) {
	float m3x4[12];
	ConvertMatrix4x3To3x4Transposed(m3x4, m4x3);
	device_->SetVertexShaderConstantF(creg, m3x4, 3);
}

void ShaderManagerDX9::VSSetMatrix(int creg, const float* pMatrix) {
	device_->SetVertexShaderConstantF(creg, pMatrix, 4);
}

// Depth in ogl is between -1;1 we need between 0;1 and optionally reverse it
const uint64_t psUniforms = DIRTY_TEXENV | DIRTY_TEX_ALPHA_MUL | DIRTY_ALPHACOLORREF | DIRTY_ALPHACOLORMASK | DIRTY_FOGCOLOR | DIRTY_STENCILREPLACEVALUE | DIRTY_SHADERBLEND | DIRTY_TEXCLAMP | DIRTY_MIPBIAS | DIRTY_RASTER_OFFSET;

void ShaderManagerDX9::PSUpdateUniforms(u64 dirtyUniforms) {
	const float minmax[4] = { (float)gstate.getDepthRangeMin(), (float)gstate.getDepthRangeMax(), 0, 0 };
	device_->SetPixelShaderConstantF(12, minmax, 1);
	if (dirtyUniforms & DIRTY_TEXENV) {
		PSSetColorUniform3(CONST_PS_TEXENV, gstate.texenvcolor);
	}
	if (dirtyUniforms & DIRTY_ALPHACOLORREF) {
		PSSetColorUniform3Alpha255(CONST_PS_ALPHACOLORREF, (gstate.getColorTestRef() & gstate.getColorTestMask()), gstate.getAlphaTestRef() & gstate.getAlphaTestMask());
	}
	if (dirtyUniforms & DIRTY_ALPHACOLORMASK) {
		PSSetColorUniform3Alpha255(CONST_PS_ALPHACOLORMASK, gstate.colortestmask, gstate.getAlphaTestMask());
	}
	if (dirtyUniforms & DIRTY_FOGCOLOR) {
		PSSetColorUniform3(CONST_PS_FOGCOLOR, gstate.fogcolor);
	}
	if (dirtyUniforms & DIRTY_STENCILREPLACEVALUE) {
		PSSetFloat(CONST_PS_STENCILREPLACE, (float)gstate.getStencilTestRef() * (1.0f / 255.0f));
	}
	if (dirtyUniforms & DIRTY_TEX_ALPHA_MUL) {
		bool doTextureAlpha = gstate.isTextureAlphaUsed();
		if (gstate_c.textureSolidAlpha && gstate.getTextureFunction() != GE_TEXFUNC_REPLACE) {
			doTextureAlpha = false;
		}
		// NOTE: Reversed value, more efficient in shader.
		float noAlphaMul[2] = { doTextureAlpha ? 0.0f : 1.0f, gstate.isColorDoublingEnabled() ? 2.0f : 1.0f };
		PSSetFloatArray(CONST_PS_TEX_NO_ALPHA_MUL, noAlphaMul, 2);
	}
	if (dirtyUniforms & DIRTY_SHADERBLEND) {
		PSSetColorUniform3(CONST_PS_BLENDFIXA, gstate.getFixA());
		PSSetColorUniform3(CONST_PS_BLENDFIXB, gstate.getFixB());

		const float fbotexSize[2] = {
			1.0f / (float)gstate_c.curRTRenderWidth,
			1.0f / (float)gstate_c.curRTRenderHeight,
		};
		PSSetFloatArray(CONST_PS_FBOTEXSIZE, fbotexSize, 2);
	}

	if (dirtyUniforms & DIRTY_TEXCLAMP) {
		const float invW = 1.0f / (float)gstate_c.curTextureWidth;
		const float invH = 1.0f / (float)gstate_c.curTextureHeight;
		const int w = gstate.getTextureWidth(0);
		const int h = gstate.getTextureHeight(0);
		const float widthFactor = (float)w * invW;
		const float heightFactor = (float)h * invH;

		// First wrap xy, then half texel xy (for clamp.)
		const float texclamp[4] = {
			widthFactor,
			heightFactor,
			invW * 0.5f,
			invH * 0.5f,
		};
		const float texclampoff[2] = {
			gstate_c.curTextureXOffset * invW,
			gstate_c.curTextureYOffset * invH,
		};
		PSSetFloatArray(CONST_PS_TEXCLAMP, texclamp, 4);
		PSSetFloatArray(CONST_PS_TEXCLAMPOFF, texclampoff, 2);
	}

	if (dirtyUniforms & DIRTY_MIPBIAS) {
		float mipBias = (float)gstate.getTexLevelOffset16() * (1.0 / 16.0f);

		// NOTE: This equation needs some adjustment in D3D9. Can't get it to look completely smooth :(
		mipBias = (mipBias + 0.25f) / (float)(gstate.getTextureMaxLevel() + 1);
		PSSetFloatArray(CONST_PS_MIPBIAS, &mipBias, 1);
	}
}

const uint64_t vsUniforms = DIRTY_BASE_UNIFORMS | DIRTY_LIGHT_UNIFORMS;

void ShaderManagerDX9::VSUpdateUniforms(u64 dirtyUniforms) {
	static_assert(sizeof(UB_VS_FS_Base) == 27 * 16, "D3D9 vertex constant layout");
	if (dirtyUniforms & DIRTY_BASE_UNIFORMS) {
		BaseUpdateUniforms(&baseUniforms_.base, dirtyUniforms, useBufferedRendering_, pixelMapped_);
		baseUniforms_.halfPixel[0] = -1.0f / gstate_c.curRTRenderWidth;
		baseUniforms_.halfPixel[1] = 1.0f / gstate_c.curRTRenderHeight;
		device_->SetVertexShaderConstantF(0, reinterpret_cast<const float *>(&baseUniforms_), sizeof(baseUniforms_) / 16);
	}
	if (dirtyUniforms & DIRTY_LIGHT_UNIFORMS) {
		LightUpdateUniforms(&lightUniforms_, dirtyUniforms);
		device_->SetVertexShaderConstantF(32, reinterpret_cast<const float *>(&lightUniforms_), sizeof(lightUniforms_) / 16);
	}
}

ShaderManagerDX9::ShaderManagerDX9(Draw::DrawContext *draw, LPDIRECT3DDEVICE9 device)
	: ShaderManagerCommon(draw), device_(device) {
	codeBuffer_ = new char[32768];
}

ShaderManagerDX9::~ShaderManagerDX9() {
	Clear();
	delete [] codeBuffer_;
}

void ShaderManagerDX9::Clear() {
	for (auto iter = fsCache_.begin(); iter != fsCache_.end(); ++iter)	{
		delete iter->second;
	}
	for (auto iter = vsCache_.begin(); iter != vsCache_.end(); ++iter)	{
		delete iter->second;
	}
	fsCache_.clear();
	vsCache_.clear();
	DirtyLastShader();
}

void ShaderManagerDX9::ClearShaders() {
	Clear();
}

void ShaderManagerDX9::DirtyLastShader() {
	// Forget the last shader ID
	lastFSID_.set_invalid();
	lastVSID_.set_invalid();
	lastVShader_ = nullptr;
	lastPShader_ = nullptr;
	// TODO: Probably not necessary to dirty uniforms here on DX9.
	gstate_c.Dirty(DIRTY_ALL_UNIFORMS | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE);
}

VSShader *ShaderManagerDX9::ApplyShader(bool useHWTransform, u32 vertexType, const ComputedPipelineState &pipelineState, ClipInfoFlags clipInfoFlags, bool pixelMapped, bool useBufferedRendering) {
	pixelMapped_ = pixelMapped;
	useBufferedRendering_ = useBufferedRendering;
	VShaderID VSID;
	if (gstate_c.IsDirty(DIRTY_VERTEXSHADER_STATE)) {
		gstate_c.Clean(DIRTY_VERTEXSHADER_STATE);
		ComputeVertexShaderID(&VSID, vertexType, useHWTransform, clipInfoFlags);
	} else {
		VSID = lastVSID_;
	}

	FShaderID FSID;
	if (gstate_c.IsDirty(DIRTY_FRAGMENTSHADER_STATE)) {
		gstate_c.Clean(DIRTY_FRAGMENTSHADER_STATE);
		ComputeFragmentShaderID(&FSID, pipelineState, draw_->GetBugs(), clipInfoFlags);
	} else {
		FSID = lastFSID_;
	}

	// Just update uniforms if this is the same shader as last time.
	if (lastVShader_ != nullptr && lastPShader_ != nullptr && VSID == lastVSID_ && FSID == lastFSID_) {
		uint64_t dirtyUniforms = gstate_c.GetDirtyUniforms();
		if (dirtyUniforms) {
			if (dirtyUniforms & psUniforms)
				PSUpdateUniforms(dirtyUniforms);
			if (dirtyUniforms & vsUniforms)
				VSUpdateUniforms(dirtyUniforms);
			gstate_c.CleanUniforms();
		}
		device_->SetVertexShader(lastVShader_->shader.Get());
		device_->SetPixelShader(lastPShader_->shader.Get());
		return lastVShader_;
	}

	lastVSID_ = VSID;
	lastFSID_ = FSID;
	VSCache::iterator vsIter = vsCache_.find(VSID);
	VSShader *vs = nullptr;
	if (vsIter == vsCache_.end())	{
		// Vertex shader not in cache. Let's compile it.
		std::string genErrorString;
		uint32_t attrMask;
		uint64_t uniformMask;
		VertexShaderFlags flags;
		if (GenerateVertexShader(VSID, codeBuffer_, draw_->GetShaderLanguageDesc(), draw_->GetBugs(), &attrMask, &uniformMask, &flags, &genErrorString)) {
			vs = new VSShader(device_, VSID, codeBuffer_, useHWTransform);
		}
		if (!vs || vs->Failed()) {
			vsCache_[VSID] = vs;
			lastVShader_ = nullptr;
			lastPShader_ = nullptr;
			ERROR_LOG(Log::G3D, "D3D9 vertex shader failed: %s", genErrorString.c_str());
			return nullptr;
		}

		vsCache_[VSID] = vs;
	} else {
		vs = vsIter->second;
	}
	if (!vs || vs->Failed()) {
		lastVShader_ = nullptr;
		lastPShader_ = nullptr;
		return nullptr;
	}
	lastVSID_ = VSID;

	FSCache::iterator fsIter = fsCache_.find(FSID);
	PSShader *fs;
	if (fsIter == fsCache_.end())	{
		// Fragment shader not in cache. Let's compile it.
		std::string errorString;
		uint64_t uniformMask;
		FragmentShaderFlags flags;
		bool success = GenerateFragmentShader(FSID, codeBuffer_, draw_->GetShaderLanguageDesc(), draw_->GetBugs(), &uniformMask, &flags, &errorString);
		if (!success) {
			ERROR_LOG(Log::G3D, "D3D9 fragment shader generation failed: %s", errorString.c_str());
			fsCache_[FSID] = nullptr;
			lastVShader_ = nullptr;
			lastPShader_ = nullptr;
			return nullptr;
		}
		fs = new PSShader(device_, FSID, codeBuffer_);
		fsCache_[FSID] = fs;
	} else {
		fs = fsIter->second;
	}

	lastFSID_ = FSID;

	uint64_t dirtyUniforms = gstate_c.GetDirtyUniforms();
	if (dirtyUniforms) {
		if (dirtyUniforms & psUniforms)
			PSUpdateUniforms(dirtyUniforms);
		if (dirtyUniforms & vsUniforms)
			VSUpdateUniforms(dirtyUniforms);
		gstate_c.CleanUniforms();
	}

	if (!fs || fs->Failed()) {
		lastVShader_ = nullptr;
		lastPShader_ = nullptr;
		return nullptr;
	}
	device_->SetPixelShader(fs->shader.Get());
	device_->SetVertexShader(vs->shader.Get());

	lastPShader_ = fs;
	lastVShader_ = vs;
	return vs;
}

std::vector<std::string> ShaderManagerDX9::DebugGetShaderIDs(DebugShaderType type) {
	std::string id;
	std::vector<std::string> ids;
	switch (type) {
	case SHADER_TYPE_VERTEX:
		for (auto iter : vsCache_) {
			iter.first.ToString(&id);
			ids.push_back(id);
		}
		break;
	case SHADER_TYPE_FRAGMENT:
		for (auto iter : fsCache_) {
			iter.first.ToString(&id);
			ids.push_back(id);
		}
		break;
	case SHADER_TYPE_PIPELINE:
	case SHADER_TYPE_SAMPLER:
	case SHADER_TYPE_TEXTURE:
	case SHADER_TYPE_VERTEXLOADER:
		break;
	}
	return ids;
}

std::string ShaderManagerDX9::DebugGetShaderString(std::string id, DebugShaderType type, DebugShaderStringType stringType) {
	ShaderID shaderId;
	shaderId.FromString(id);
	switch (type) {
	case SHADER_TYPE_VERTEX:
	{
		auto iter = vsCache_.find(VShaderID(shaderId));
		if (iter == vsCache_.end()) {
			return "";
		}
		return iter->second ? iter->second->GetShaderString(stringType) : "Shader generation failed";
	}

	case SHADER_TYPE_FRAGMENT:
	{
		auto iter = fsCache_.find(FShaderID(shaderId));
		if (iter == fsCache_.end()) {
			return "";
		}
		return iter->second ? iter->second->GetShaderString(stringType) : "Shader generation failed";
	}
	default:
		return "N/A";
	}
}
