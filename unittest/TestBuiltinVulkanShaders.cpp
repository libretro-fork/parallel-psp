// Copyright (c) 2026- PPSSPP Project. SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/GPU/ShaderWriter.h"
#include "Common/GPU/Vulkan/BuiltinVulkanShaders.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "GPU/Common/Draw2D.h"
#include "GPU/Common/DepalettizeShaderCommon.h"
#include <map>
#include "unittest/UnitTest.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include <memory>
#include "GPU/Common/ShaderId.h"
#include "GPU/Common/VertexShaderGenerator.h"
#include "GPU/Common/FragmentShaderGenerator.h"
#include "GPU/Common/ReinterpretFramebuffer.h"
#include "GPU/Common/StencilCommon.h"

void GenerateDraw2DVS(ShaderWriter &writer);
Draw2DPipelineInfo GenerateDraw2DCopyColorFs(ShaderWriter &writer);
Draw2DPipelineInfo GenerateDraw2DCopyColorRect2LinFs(ShaderWriter &writer);
Draw2DPipelineInfo GenerateDraw2DCopyDepthFs(ShaderWriter &writer);
Draw2DPipelineInfo GenerateDraw2DEncodeDepthFs(ShaderWriter &writer);
Draw2DPipelineInfo GenerateDraw2D565ToDepthFs(ShaderWriter &writer);
Draw2DPipelineInfo GenerateDraw2D565ToDepthDeswizzleFs(ShaderWriter &writer);

static bool CheckBuiltin(VkShaderStageFlagBits stage, const char *source) {
	std::vector<uint32_t> words;
	EXPECT_TRUE(LookupBuiltinVulkanShader(stage, source, words));
	EXPECT_TRUE(words.size() >= 5 && words[0] == 0x07230203);
	std::string error = "stale diagnostic";
	std::vector<uint32_t> routed;
	EXPECT_TRUE(GLSLtoSPV(stage, source, GLSLVariant::VULKAN, routed, &error));
	EXPECT_TRUE(error.empty() && words == routed);

	// A different stage or edited source must never receive a binary compiled
	// for the original. Unknown shaders continue through the existing compiler.
	std::vector<uint32_t> sentinel{0x12345678};
	auto wrongStage = stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
	EXPECT_FALSE(LookupBuiltinVulkanShader(wrongStage, source, sentinel));
	EXPECT_FALSE(LookupBuiltinVulkanShader(stage, (std::string(source) + "\n").c_str(), sentinel));
	EXPECT_FALSE(LookupBuiltinVulkanShader(stage, nullptr, sentinel));
	EXPECT_TRUE(sentinel.size() == 1 && sentinel[0] == 0x12345678);
	return true;
}

bool TestBuiltinVulkanShaders() {
	ShaderLanguageDesc language(GLSL_VULKAN);
	char source[16384];
	{
		ShaderWriter writer(source, language, ShaderStage::Vertex);
		GenerateDraw2DVS(writer);
		RET(CheckBuiltin(VK_SHADER_STAGE_VERTEX_BIT, source));
	}
	for (auto generate : {GenerateDraw2DCopyColorFs, GenerateDraw2DCopyColorRect2LinFs,
		GenerateDraw2DCopyDepthFs, GenerateDraw2DEncodeDepthFs,
		GenerateDraw2D565ToDepthFs, GenerateDraw2D565ToDepthDeswizzleFs}) {
		ShaderWriter writer(source, language, ShaderStage::Fragment);
		generate(writer);
		RET(CheckBuiltin(VK_SHADER_STAGE_FRAGMENT_BIT, source));
	}
	return true;
}

// Keep this tied to the same manifest and upload wrapper as the core, so a
// newly shipped scaler cannot silently regress to runtime compilation.
extern const char *uploadShader;
bool TestBuiltinTextureShaders() {
	IniFile manifest;
	EXPECT_TRUE(manifest.Load(Path("assets/shaders/defaultshaders.ini")));
	size_t checked = 0;
	for (const auto &section : manifest.Sections()) {
		std::string type;
		section->Get("Type", &type);
		if (!equalsNoCase(type, "texture")) {
			continue;
		}
		std::vector<std::string> paths;
		for (const char *key : {"Compute", "Compute2", "Compute3", "Compute4"}) {
			std::string filename;
			if (section->Get(key, &filename)) {
				paths.push_back(filename);
			}
		}
		EXPECT_FALSE(paths.empty());
		for (const auto &path : paths) {
			size_t size = 0;
			std::unique_ptr<uint8_t[]> data(File::ReadLocalFile(Path("assets/shaders") / path, &size));
			EXPECT_TRUE(data != nullptr);
			std::string shader((const char *)data.get(), size);
			std::string source = paths.size() == 1 ? StringFromFormat(uploadShader, shader.c_str()) : shader;
			RET(CheckBuiltin(VK_SHADER_STAGE_COMPUTE_BIT, source.c_str()));
			// The exact same assets can arrive as Windows files or inside a pack.
			std::string crlf;
			for (size_t i = 0; i < source.size(); ++i) {
				if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n') {
					continue;
				}
				if (source[i] == '\n') crlf.push_back('\r');
				crlf.push_back(source[i]);
			}
			std::vector<uint32_t> original, normalized;
			EXPECT_TRUE(LookupBuiltinVulkanShader(VK_SHADER_STAGE_COMPUTE_BIT, source.c_str(), original));
			EXPECT_TRUE(LookupBuiltinVulkanShader(VK_SHADER_STAGE_COMPUTE_BIT, crlf.c_str(), normalized));
			EXPECT_TRUE(original == normalized);
			++checked;
		}
	}
	EXPECT_TRUE(checked >= 14);
	return true;
}

static bool CheckDepalDefaults(const std::vector<uint32_t> &words, const DepalConfig &config) {
	std::map<uint32_t, uint32_t> ids, literals;
	for (size_t pos = 5; pos < words.size();) {
		uint32_t count = words[pos] >> 16, opcode = words[pos] & 0xffff;
		EXPECT_TRUE(count != 0 && pos + count <= words.size());
		if (opcode == 71 && count == 4 && words[pos + 2] == 1) ids[words[pos + 1]] = words[pos + 3];
		if (opcode == 50 && count == 4) literals[words[pos + 2]] = words[pos + 3];
		pos += count;
	}
	float offset = (float)(std::round(((double)config.startPos + 0.5) / 512.0 * 1000000.0) / 1000000.0);
	uint32_t offsetBits;
	memcpy(&offsetBits, &offset, sizeof(offsetBits));
	uint32_t expected[] = {(uint32_t)config.bufferFormat, (uint32_t)config.textureFormat,
		(uint32_t)config.smoothedDepal, config.shift, config.mask, config.startPos, config.depthUpperBits, offsetBits};
	EXPECT_TRUE(ids.size() == 8);
	for (const auto &entry : ids) {
		EXPECT_TRUE(entry.second >= 100 && entry.second < 108);
		EXPECT_TRUE(literals.count(entry.first) == 1 && literals.at(entry.first) == expected[entry.second - 100]);
	}
	return true;
}

bool TestBuiltinDepalShaders() {
	ShaderLanguageDesc language(GLSL_VULKAN);
	char source[16384];
	std::string goodSource;
	for (int stereo = 0; stereo < 2; ++stereo) {
		for (int format = GE_FORMAT_565; format <= GE_FORMAT_CLUT8; ++format) {
			for (uint32_t shift : {0u, 1u, 5u, 8u, 15u, 16u, 24u, 31u}) {
				DepalConfig config{};
				config.bufferFormat = (GEBufferFormat)format;
				config.textureFormat = format == GE_FORMAT_5551 ? GE_TFMT_CLUT8 : GE_TFMT_CLUT32;
				config.mask = shift & 1 ? 0x1f : 0xff;
				config.shift = shift;
				config.startPos = (shift % 32) * 16;
				config.depthUpperBits = shift & 1 ? 2 : 6;
				ShaderWriter writer(source, language, ShaderStage::Fragment, Slice<const char *>::empty(),
					stereo ? ShaderWriterFlags::FS_AUTO_STEREO : ShaderWriterFlags::NONE);
				GenerateDepalFs(writer, config);
				RET(CheckBuiltin(VK_SHADER_STAGE_FRAGMENT_BIT, source));
				std::vector<uint32_t> words;
				EXPECT_TRUE(LookupBuiltinVulkanShader(VK_SHADER_STAGE_FRAGMENT_BIT, source, words));
				RET(CheckDepalDefaults(words, config));
				goodSource = source;
			}
		}
	}
	std::vector<uint32_t> sentinel{0x12345678};
	const std::string needle = "psp_spec_100 = ";
	size_t value = goodSource.find(needle) + needle.size();
	size_t end = goodSource.find('u', value);
	EXPECT_TRUE(end != std::string::npos);
	goodSource.replace(value, end - value, "4294967296");
	EXPECT_FALSE(LookupBuiltinVulkanShader(VK_SHADER_STAGE_FRAGMENT_BIT, goodSource.c_str(), sentinel));
	EXPECT_TRUE(sentinel.size() == 1 && sentinel[0] == 0x12345678);
	return true;
}


static bool CheckFullPrecision(const char *source, uint32_t stage) {
	std::vector<uint32_t> words;
	EXPECT_TRUE(LookupBuiltinVulkanShader(stage, source, words));
	for (size_t pos = 5; pos < words.size();) {
		uint32_t count = words[pos] >> 16;
		EXPECT_TRUE(count && count <= words.size() - pos);
		// OpDecorate RelaxedPrecision would allow unrelated host FP16 rounding.
		EXPECT_FALSE((words[pos] & 65535) == 71 && count >= 3 && words[pos + 2] == 0);
		pos += count;
	}
	return true;
}

bool TestBuiltinGameVertexShaders() {
	struct RestoreUseFlags {
		uint32_t saved = gstate_c.UseFlags();
		~RestoreUseFlags() { gstate_c.SetUseFlags(saved); }
	} restore;
	ShaderLanguageDesc language(GLSL_VULKAN);
	char reference[32768], source[32768];
	uint64_t random = 0x123456789abcdefULL;
	for (int n = 0; n < 1024; ++n) {
		random ^= random << 13;
		random ^= random >> 7;
		random ^= random << 17;
		VShaderID id;
		id.FromUint64(random & ((1ULL << 52) - 1));
		id.SetBit(VS_BIT_USE_HW_TRANSFORM, n & 1);
		id.SetBit(VS_BIT_IS_THROUGH, !(n & 1) && (n % 3 == 0));
		uint32_t useFlags = 0;
		if (n & 2) useFlags |= GPU_USE_CLIP_DISTANCE;
		if (n & 4) useFlags |= GPU_USE_CULL_DISTANCE;
		if (n & 8) useFlags |= GPU_ROUND_DEPTH_TO_16BIT;
		if (n & 16) useFlags |= GPU_USE_PRE_ROTATION;
		if (n & 32) useFlags |= GPU_USE_NONBUFFERED_FLIP;
		gstate_c.SetUseFlags(useFlags);
		Draw::Bugs bugs;
		if (n % 7 == 0) bugs.Infest(Draw::Bugs::BROKEN_NAN_IN_CONDITIONAL);
		if (n % 11 == 0) bugs.Infest(Draw::Bugs::MALI_CONSTANT_LOAD_BUG);
		if (n % 13 == 0) bugs.Infest(Draw::Bugs::BROKEN_FLAT_IN_SHADER);
		uint32_t attributes, referenceAttributes;
		uint64_t uniforms, referenceUniforms;
		VertexShaderFlags flags, referenceFlags;
		std::string error;
		EXPECT_TRUE(GenerateVertexShader(id, reference, language, bugs, &referenceAttributes, &referenceUniforms, &referenceFlags, &error));
		EXPECT_TRUE(GenerateVertexShader(id, source, language, bugs, &attributes, &uniforms, &flags, &error, true));
		EXPECT_TRUE(attributes == referenceAttributes && uniforms == referenceUniforms && flags == referenceFlags);
		EXPECT_TRUE(bool(flags & VertexShaderFlags::MULTI_VIEW) == id.Bit(VS_BIT_SIMPLE_STEREO));
		RET(CheckBuiltin(VK_SHADER_STAGE_VERTEX_BIT, source));
		RET(CheckFullPrecision(source, VK_SHADER_STAGE_VERTEX_BIT));
	}
	return true;
}


bool TestBuiltinGameFragmentShaders() {
	struct RestoreUseFlags {
		uint32_t saved = gstate_c.UseFlags();
		~RestoreUseFlags() { gstate_c.SetUseFlags(saved); }
	} restore;
	ShaderLanguageDesc language(GLSL_VULKAN);
	char reference[32768], source[32768];
	uint64_t random = 0xabcdef123456789ULL;
	int accepted = 0;
	for (int n = 0; n < 2048; ++n) {
		random ^= random << 13; random ^= random >> 7; random ^= random << 17;
		FShaderID id; id.FromUint64(random & ((1ULL << 62) - 1));
		id.SetBit(FS_BIT_USE_FRAMEBUFFER_FETCH, false);
		gstate_c.SetUseFlags(n & 1 ? GPU_ROUND_FRAGMENT_DEPTH_TO_16BIT : 0);
		Draw::Bugs bugs;
		if (n % 13 == 0) bugs.Infest(Draw::Bugs::BROKEN_FLAT_IN_SHADER);
		uint64_t uniforms, referenceUniforms;
		FragmentShaderFlags flags, referenceFlags;
		std::string error;
		if (!GenerateFragmentShader(id, reference, language, bugs, &referenceUniforms, &referenceFlags, &error)) continue;
		EXPECT_TRUE(GenerateFragmentShader(id, source, language, bugs, &uniforms, &flags, &error, true));
		EXPECT_TRUE(uniforms == referenceUniforms && flags == referenceFlags);
		RET(CheckBuiltin(VK_SHADER_STAGE_FRAGMENT_BIT, source));
		RET(CheckFullPrecision(source, VK_SHADER_STAGE_FRAGMENT_BIT));
		++accepted;
	}
	EXPECT_TRUE(accepted > 128);
	for (int variant = 0; variant < 4; ++variant) {
		Draw::Bugs bugs;
		if (variant & 1) bugs.Infest(Draw::Bugs::NO_DEPTH_CANNOT_DISCARD_STENCIL_MALI);
		GenerateStencilFs(source, language, bugs, variant & 2);
		RET(CheckBuiltin(VK_SHADER_STAGE_FRAGMENT_BIT, source));
	}
	GenerateStencilVs(source, language);
	RET(CheckBuiltin(VK_SHADER_STAGE_VERTEX_BIT, source));
	for (int from = 0; from < 4; ++from) {
		for (int to = 0; to < 4; ++to) {
			if (from == to) continue;
			for (int stereo = 0; stereo < 2; ++stereo) {
				ShaderWriter writer(source, language, ShaderStage::Fragment, Slice<const char *>::empty(),
					stereo ? ShaderWriterFlags::FS_AUTO_STEREO : ShaderWriterFlags::NONE);
				GenerateReinterpretFragmentShader(writer, (GEBufferFormat)from, (GEBufferFormat)to);
				RET(CheckBuiltin(VK_SHADER_STAGE_FRAGMENT_BIT, source));
			}
		}
	}
	return true;
}
