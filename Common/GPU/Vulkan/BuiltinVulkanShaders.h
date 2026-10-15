// Copyright (c) 2026- PPSSPP Project. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

// Uses Vulkan shader stage bits without depending on Vulkan headers. Returns
// false without changing spirv when the source is not in the fixed bank.
bool LookupBuiltinVulkanShader(uint32_t stage, const char *source, std::vector<uint32_t> &spirv);
