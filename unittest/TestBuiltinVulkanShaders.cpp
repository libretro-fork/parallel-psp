// Copyright (c) 2026- PPSSPP Project. SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/GPU/ShaderWriter.h"
#include "Common/GPU/Vulkan/BuiltinVulkanShaders.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "GPU/Common/Draw2D.h"
#include "unittest/UnitTest.h"

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
