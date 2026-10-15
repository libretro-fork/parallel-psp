// Copyright (c) 2026- PPSSPP Project. SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/GPU/ShaderWriter.h"
#include "Common/GPU/Vulkan/BuiltinVulkanShaders.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "GPU/Common/Draw2D.h"
#include "unittest/UnitTest.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include <memory>

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
