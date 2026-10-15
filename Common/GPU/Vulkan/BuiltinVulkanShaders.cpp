// Copyright (c) 2026- PPSSPP Project. SPDX-License-Identifier: GPL-2.0-or-later
#include <cstddef>
#include <cstring>
#include "Common/GPU/Vulkan/BuiltinVulkanShaders.h"

namespace {
struct BuiltinShader {
	uint32_t stage;
	const char *source;
	const uint32_t *words;
	size_t count;
};
#include "Common/GPU/Vulkan/BuiltinShaders.inl"
}

bool LookupBuiltinVulkanShader(uint32_t stage, const char *source, std::vector<uint32_t> &spirv) {
	if (!source) {
		return false;
	}
	for (const auto &shader : builtinShaders) {
		if (shader.stage == stage && strcmp(source, shader.source) == 0) {
			spirv.assign(shader.words, shader.words + shader.count);
			return true;
		}
	}
	return false;
}
