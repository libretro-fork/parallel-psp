// Copyright (c) 2026- PPSSPP Project. SPDX-License-Identifier: GPL-2.0-or-later
#include <cstddef>
#include <cstring>
#include <string>
#include <limits>
#include <utility>
#include <encodings/deflate.h>
#include "Common/GPU/Vulkan/BuiltinVulkanShaders.h"

namespace {
struct BuiltinSpecialization {
	uint32_t id;
	uint32_t word;
};
struct BuiltinShader {
	uint32_t stage;
	const char *source;
	const uint8_t *compressed;
	size_t compressedSize;
	size_t count;
	const BuiltinSpecialization *specializations;
	size_t specializationCount;
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

struct SpecializationValue { uint32_t id; uint32_t value; };

bool ReadUint(const std::string &s, size_t &pos, uint32_t &value) {
	value = 0;
	size_t start = pos;
	while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
		uint32_t digit = s[pos++] - '0';
		if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
		value = value * 10 + digit;
	}
	return pos != start;
}

bool Consume(const std::string &s, size_t &pos, const std::string &token) {
	if (s.compare(pos, token.size(), token) != 0) return false;
	pos += token.size();
	return true;
}

// Normalize only our generated uint32 specialization declarations. Matching the
// entire remaining source prevents unrelated or edited programs from aliasing
// a bank entry. Parameter defaults stay in the GLSL for the compiler fallback.
bool CanonicalSpecializations(const char *source, std::string &canonical, std::vector<SpecializationValue> &values) {
	for (const char *p = source; *p; ++p) {
		if (p[0] != '\r' || p[1] != '\n') canonical.push_back(*p);
	}
	const std::string prefix = "layout(constant_id = ";
	size_t search = 0;
	while ((search = canonical.find(prefix, search)) != std::string::npos) {
		size_t pos = search + prefix.size();
		uint32_t id, value;
		if (!ReadUint(canonical, pos, id) ||
			!Consume(canonical, pos, ") const uint psp_spec_" + std::to_string(id) + " = ")) return false;
		size_t valueStart = pos;
		if (!ReadUint(canonical, pos, value)) return false;
		size_t valueEnd = pos;
		if (!Consume(canonical, pos, "u;\n") || values.size() >= 32) return false;
		for (const auto &existing : values) if (existing.id == id) return false;
		values.push_back({id, value});
		canonical.replace(valueStart, valueEnd - valueStart, "0");
		search = valueStart + 4;
	}
	return !values.empty();
}
}

bool LookupBuiltinVulkanShader(uint32_t stage, const char *source, std::vector<uint32_t> &spirv) {
	if (!source) {
		return false;
	}
	std::string canonical;
	std::vector<SpecializationValue> values;
	if (strstr(source, "psp_spec_") != nullptr) {
		if (!CanonicalSpecializations(source, canonical, values)) return false;
		source = canonical.c_str();
	}
	for (const auto &shader : builtinShaders) {
		if (shader.stage != stage || !SameSource(shader.source, source)) continue;
		// One spare word lets the decoder consume the final block marker even
		// when its last output byte exactly fills the expected shader size.
		std::vector<uint32_t> specialized(shader.count + 1);
		void *decoder = rinflate_new(-15);
		if (!decoder) {
			return false;
		}
		rinflate_set_in(decoder, shader.compressed, shader.compressedSize);
		rinflate_set_out(decoder, (uint8_t *)specialized.data(), specialized.size() * sizeof(uint32_t));
		size_t read = 0, wrote = 0;
		int result = rinflate_process(decoder, &read, &wrote);
		rinflate_free(decoder);
		if (result != RDEFLATE_PROCESS_END || read != shader.compressedSize || wrote != shader.count * sizeof(uint32_t)) {
			return false;
		}
		specialized.resize(shader.count);
		if (!specialized.empty() && specialized[0] == 0x03022307) {
			for (uint32_t &word : specialized) {
				word = (word >> 24) | ((word >> 8) & 0xff00) | ((word << 8) & 0xff0000) | (word << 24);
			}
		}
		for (size_t i = 0; i < shader.specializationCount; ++i) {
			const auto &spec = shader.specializations[i];
			bool found = false;
			for (const auto &param : values) {
				if (param.id == spec.id && spec.word < specialized.size()) {
					specialized[spec.word] = param.value;
					found = true;
					break;
				}
			}
			if (!found) return false;
		}
		spirv = std::move(specialized);
		return true;
	}
	return false;
}
