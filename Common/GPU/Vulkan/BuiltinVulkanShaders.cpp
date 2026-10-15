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

// Asset files may use CRLF on disk or LF in a shader pack. GLSL treats these
// identically. Normalize only CRLF pairs, preserving every other source byte.
bool SameSource(const char *expected, const char *source) {
	while (*expected && *source) {
		if (source[0] == '\r' && source[1] == '\n') {
			++source;
		}
		if (*expected++ != *source++) {
			return false;
		}
	}
	return *expected == *source;
}
}

bool LookupBuiltinVulkanShader(uint32_t stage, const char *source, std::vector<uint32_t> &spirv) {
	if (!source) {
		return false;
	}
	for (const auto &shader : builtinShaders) {
		if (shader.stage == stage && SameSource(shader.source, source)) {
			spirv.assign(shader.words, shader.words + shader.count);
			return true;
		}
	}
	return false;
}
