// Copyright (c) 2014- PPSSPP Project.

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

#include <cstdio>
#include <cmath>
#include <cstring>

#include "Common/GPU/Shader.h"
#include "Common/GPU/ShaderWriter.h"

#include "Common/StringUtils.h"
#include "Common/Log.h"
#include "Common/LogOnce.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/DepalettizeShaderCommon.h"
#include "GPU/Common/Draw2D.h"

static const InputDef vsInputs[2] = {
	{ "vec2", "a_position", Draw::SEM_POSITION, },
	{ "vec2", "a_texcoord0", Draw::SEM_TEXCOORD0, },
};

// TODO: Deduplicate with TextureShaderCommon.cpp
static const SamplerDef samplers[2] = {
	{ 0, "tex", SamplerFlags::ARRAY_ON_VULKAN },
	{ 1, "pal" },
};

static const VaryingDef varyings[1] = {
	{ "vec2", "v_texcoord", Draw::SEM_TEXCOORD0, 0, "highp" },
};

// A single Vulkan program covers every palette mode. The bank specializes its
// scalar defaults before module creation; GLSL compilation remains a valid
// fallback if the shader source changes during development.
void GenerateVulkanDepalFs(ShaderWriter &writer, const DepalConfig &config) {
	// Preserve the six-decimal coordinate constant used by the legacy float
	// shader (notably the SOCOM 5551/CLUT8 path).
	float offset = (float)(std::round(((double)config.startPos + 0.5) / 512.0 * 1000000.0) / 1000000.0);
	uint32_t offsetBits;
	memcpy(&offsetBits, &offset, sizeof(offsetBits));
	const uint32_t defaults[] = {
		(uint32_t)config.bufferFormat, (uint32_t)config.textureFormat,
		(uint32_t)config.smoothedDepal, config.shift, config.mask,
		config.startPos, config.depthUpperBits, offsetBits,
	};
	for (uint32_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
		writer.F("layout(constant_id = %u) const uint psp_spec_%u = %uu;\n", 100 + i, 100 + i, defaults[i]);
	}
	writer.DeclareSamplers(samplers);
	writer.HighPrecisionFloat();
	writer.BeginFSMain(g_draw2Duniforms, varyings);
	writer.C(R"(
  vec2 texcoord = v_texcoord;
  if (psp_spec_100 == 4u && psp_spec_106 == 2u) {
    int x = int((texcoord.x / scaleFactor) * texSize.x);
    int xclear = x & 0x01F0;
    int temp = (x - xclear) | ((x >> 1) & 0xF0) | ((x << 4) & 0x100);
    texcoord.x = (float(temp) / texSize.x) * scaleFactor;
  }
  vec4 color = )").SampleTexture2D("tex", "texcoord").C(R"(;
  vec4 outColor;
  float coord;
  if (psp_spec_102 != 0u) {
    float multiplier = psp_spec_100 == 0u && psp_spec_103 == 5u ? 63.0 : 31.0;
    uint channel = psp_spec_103 == 0u ? 0u : (psp_spec_103 == 5u ? 1u : 2u);
    coord = (color[channel] * multiplier + 0.5) * 0.001953;
    outColor = )").SampleTexture2D("pal", "vec2(coord, 0.0)").C(R"(;
  } else if (psp_spec_100 == 1u && psp_spec_101 == 5u) {
    // Preserve the legacy floating-point SOCOM path rather than substituting
    // integer reconstruction, which rounds the blue channel differently.
    float lookup = color.r;
    float multiplier = 0.5;
    if (psp_spec_104 == 255u && psp_spec_103 == 0u) {
      lookup = floor(floor(color.a) * 128.0 + color.b * 64.0);
      multiplier = 0.001953;
    } else if ((psp_spec_104 & (psp_spec_104 + 1u)) == 0u && psp_spec_103 < 16u) {
      uint channel = psp_spec_103 / 5u;
      uint shift = psp_spec_103 % 5u;
      lookup = color[channel];
      if (shift == 0u && psp_spec_104 == 31u) {
        multiplier = 0.060547;
      } else if (psp_spec_103 == 15u && psp_spec_104 == 1u) {
        multiplier = 0.001953;
      } else {
        const float factors[5] = float[5](31.990000, 15.995000, 7.997500, 3.998750, 1.999375);
        lookup = mod(lookup * factors[shift], float(psp_spec_104 + 1u));
        multiplier = 0.001953;
      }
    }
    coord = lookup * multiplier + uintBitsToFloat(psp_spec_107);
    outColor = )").SampleTexture2D("pal", "vec2(coord, 0.0)").C(R"(;
  } else if (psp_spec_100 == 4u && psp_spec_101 == 0u) {
    uint depth = uint(clamp(color.x * 65535.0, 0.0, 65535.0));
    outColor = vec4(float(depth & 31u) / 31.0, float((depth >> 5u) & 63u) / 63.0,
                    float((depth >> 11u) & 31u) / 31.0, 1.0);
  } else {
    uint index = 0u;
    if (psp_spec_100 == 5u) {
      index = uint(color.r * 255.99);
    } else if (psp_spec_100 == 3u) {
      uvec4 rgba = uvec4(color * 255.99);
      index = rgba.r | (rgba.g << 8u) | (rgba.b << 16u) | (rgba.a << 24u);
    } else if (psp_spec_100 == 2u) {
      uvec4 rgba = uvec4(color * 15.99);
      index = rgba.r | (rgba.g << 4u) | (rgba.b << 8u) | (rgba.a << 12u);
    } else if (psp_spec_100 == 0u) {
      uvec3 rgb = uvec3(color.rgb * vec3(31.99, 63.99, 31.99));
      index = rgb.r | (rgb.g << 5u) | (rgb.b << 11u);
    } else if (psp_spec_100 == 1u) {
      uvec3 rgb = uvec3(color.rgb * 31.99);
      index = rgb.r | (rgb.g << 5u) | (rgb.b << 10u) | (uint(color.a) << 15u);
    } else if (psp_spec_100 == 4u) {
      index = uint(clamp(color.x * 65535.0, 0.0, 65535.0));
    }
    index = ((index >> psp_spec_103) & psp_spec_104) | psp_spec_105;
    coord = (float(index) + 0.5) * 0.001953;
    outColor = )").SampleTexture2D("pal", "vec2(coord, 0.0)").C(R"(;
  }
)");
	writer.EndFSMain("outColor");
}

// Uses integer instructions available since OpenGL 3.0, ES 3.0 (and 2.0 with extensions), and of course Vulkan and D3D11.
void GenerateDepalShader300(ShaderWriter &writer, const DepalConfig &config) {
	const int shift = config.shift;
	const int mask = config.mask;

	writer.C("  vec2 texcoord = v_texcoord;\n");

	// Implement the swizzle we need to simulate, if a game uses 8888 framebuffers and any other mode than "6" to access depth textures.
	// This implements the "2" mode swizzle (it fixes up the Y direction but not X. See comments on issue #15898, Tantalus games)
	// NOTE: This swizzle can be made to work with any power-of-2 resolution scaleFactor by shifting
	// the bits around, but not sure how to handle 3x scaling. For now this is 1x-only (rough edges at higher resolutions).
	if (config.bufferFormat == GE_FORMAT_DEPTH16) {
		if (config.depthUpperBits == 0x2) {
			writer.C(R"(
  int x = int((texcoord.x / scaleFactor) * texSize.x);
  int xclear = x & 0x01F0;
  int temp = (x - xclear) | ((x >> 1) & 0xF0) | ((x << 4) & 0x100);
  texcoord.x = (float(temp) / texSize.x) * scaleFactor;
)");
		}
	}

	// Sampling turns our texture into floating point. To avoid this, might be able
	// to declare them as isampler2D objects, but these require integer textures, which needs more work.
	// Anyhow, we simply work around this by converting back to integer, which is fine.
	// Use the mask to skip reading some components.

	// TODO: Since we actually have higher precision color data here, we might want to apply a dithering pattern here
	// in the 5551, 565 and 4444 modes. This would benefit Test Drive which renders at 16-bit on the real hardware
	// and dithers immediately, while we render at higher color depth and thus don't dither resulting in banding
	// when we sample it at low color depth like this.

	// An alternative would be to have a special mode where we keep some extra precision here and sample the CLUT linearly - works for ramps such
	// as those that Test Drive uses for its color remapping. But would need game specific flagging.

	writer.C("  vec4 color = ").SampleTexture2D("tex", "texcoord").C(";\n");

	int shiftedMask = mask << shift;
	switch (config.bufferFormat) {
	case GE_FORMAT_CLUT8:
		writer.C("  int index = int(color.r * 255.99);\n");
		break;
	case GE_FORMAT_8888:
		if (shiftedMask & 0xFF) writer.C("  int r = int(color.r * 255.99);\n"); else writer.C("  int r = 0;\n");
		if (shiftedMask & 0xFF00) writer.C("  int g = int(color.g * 255.99);\n"); else writer.C("  int g = 0;\n");
		if (shiftedMask & 0xFF0000) writer.C("  int b = int(color.b * 255.99);\n"); else writer.C("  int b = 0;\n");
		if (shiftedMask & 0xFF000000) writer.C("  int a = int(color.a * 255.99);\n"); else writer.C("  int a = 0;\n");
		writer.C("  int index = (a << 24) | (b << 16) | (g << 8) | (r);\n");
		break;
	case GE_FORMAT_4444:
		if (shiftedMask & 0xF) writer.C("  int r = int(color.r * 15.99);\n"); else writer.C("  int r = 0;\n");
		if (shiftedMask & 0xF0) writer.C("  int g = int(color.g * 15.99);\n"); else writer.C("  int g = 0;\n");
		if (shiftedMask & 0xF00) writer.C("  int b = int(color.b * 15.99);\n"); else writer.C("  int b = 0;\n");
		if (shiftedMask & 0xF000) writer.C("  int a = int(color.a * 15.99);\n"); else writer.C("  int a = 0;\n");
		writer.C("  int index = (a << 12) | (b << 8) | (g << 4) | (r);\n");
		break;
	case GE_FORMAT_565:
		if (shiftedMask & 0x1F) writer.C("  int r = int(color.r * 31.99);\n"); else writer.C("  int r = 0;\n");
		if (shiftedMask & 0x7E0) writer.C("  int g = int(color.g * 63.99);\n"); else writer.C("  int g = 0;\n");
		if (shiftedMask & 0xF800) writer.C("  int b = int(color.b * 31.99);\n"); else writer.C("  int b = 0;\n");
		writer.C("  int index = (b << 11) | (g << 5) | (r);\n");
		break;
	case GE_FORMAT_5551:
		if (config.textureFormat == GE_TFMT_CLUT8) {
			// SOCOM case. We need to make sure the next few lines load the right bits, see below.
			shiftedMask <<= 8;
		}
		if (shiftedMask & 0x1F) writer.C("  int r = int(color.r * 31.99);\n"); else writer.C("  int r = 0;\n");
		if (shiftedMask & 0x3E0) writer.C("  int g = int(color.g * 31.99);\n"); else writer.C("  int g = 0;\n");
		if (shiftedMask & 0x7C00) writer.C("  int b = int(color.b * 31.99);\n"); else writer.C("  int b = 0;\n");
		if (shiftedMask & 0x8000) writer.C("  int a = int(color.a);\n"); else writer.C("  int a = 0;\n");
		writer.C("  int index = (a << 15) | (b << 10) | (g << 5) | (r);\n");

		if (config.textureFormat == GE_TFMT_CLUT8) {
			// SOCOM case. #16210
			// To debug the issue, remove this shift to see the texture (check for clamping etc).
			writer.C("  index >>= 8;\n");
		}

		break;
	case GE_FORMAT_DEPTH16:
		// Decode depth buffer.
		writer.C("  float depth = clamp(color.x * 65535.0, 0.0, 65535.0);\n");  // Probably don't need to clamp here.
		if (config.bufferFormat == GE_FORMAT_DEPTH16 && config.textureFormat == GE_TFMT_5650) {
			// Convert depth to 565, without going through a CLUT.
			// TODO: Make "depal without a CLUT" a separate concept, to avoid redundantly creating a CLUT texture.
			writer.C("  int idepth = int(depth);\n");
			writer.C("  float r = float(idepth & 31) / 31.0;\n");
			writer.C("  float g = float((idepth >> 5) & 63) / 63.0;\n");
			writer.C("  float b = float((idepth >> 11) & 31) / 31.0;\n");
			writer.C("  vec4 outColor = vec4(r, g, b, 1.0);\n");
			return;
		}
		writer.C("  int index = int(depth);\n");
		break;
	default:
		break;
	}

	float texturePixels = 512.0f;

	if (shift) {
		writer.F("  index = (int(uint(index) >> uint(%d)) & 0x%02x)", shift, mask);
	} else {
		writer.F("  index = (index & 0x%02x)", mask);
	}
	if (config.startPos) {
		writer.F(" | %d;\n", config.startPos);  // '|' matches what we have in gstate.h
	} else {
		writer.F(";\n");
	}

	writer.F("  vec2 uv = vec2((float(index) + 0.5) * %f, 0.0);\n", 1.0f / texturePixels);
	writer.C("  vec4 outColor = ").SampleTexture2D("pal", "uv").C(";\n");
}

// FP only, to suit GL(ES) 2.0 and DX9
void GenerateDepalShaderFloat(ShaderWriter &writer, const DepalConfig &config) {
	char lookupMethod[128] = "index.r";

	const int shift = config.shift;
	const int mask = config.mask;

	writer.C("  vec4 index = ").SampleTexture2D("tex", "v_texcoord").C(";\n");

	float index_multiplier = 1.0f;
	// pixelformat is the format of the texture we are sampling.
	bool formatOK = true;
	switch (config.bufferFormat) {
	case GE_FORMAT_CLUT8:
		if (shift == 0 && mask == 0xFF) {
			// Easy peasy.
			if (writer.Lang().shaderLanguage == HLSL_D3D9)
				snprintf(lookupMethod, sizeof(lookupMethod), "index.a");
			else
				snprintf(lookupMethod, sizeof(lookupMethod), "index.r");
			formatOK = true;
		} else {
			// Deal with this if we find it.
			formatOK = false;
		}
		break;
	case GE_FORMAT_8888:
		if ((mask & (mask + 1)) == 0) {
			// If the value has all bits contiguous (bitmask check above), we can mod by it + 1.
			const char *rgba = "rrrrrrrrggggggggbbbbbbbbaaaaaaaa";
			const u8 rgba_shift = shift & 7;
			if (rgba_shift == 0 && mask == 0xFF) {
				snprintf(lookupMethod, sizeof(lookupMethod), "index.%c", rgba[shift]);
			} else {
				snprintf(lookupMethod, sizeof(lookupMethod), "mod(index.%c * %f, %d.0)", rgba[shift], 255.99f / (1 << rgba_shift), mask + 1);
				index_multiplier = 1.0f / 256.0f;
				// Format was OK if there weren't bits from another component.
				formatOK = mask <= 255 - (1 << rgba_shift);
			}
		} else {
			formatOK = false;
		}
		break;
	case GE_FORMAT_4444:
		if ((mask & (mask + 1)) == 0 && shift < 16) {
			const char *rgba = "rrrrggggbbbbaaaa";
			const u8 rgba_shift = shift & 3;
			if (rgba_shift == 0 && mask == 0xF) {
				snprintf(lookupMethod, sizeof(lookupMethod), "index.%c", rgba[shift]);
				index_multiplier = 15.0f / 256.0f;
			} else {
				// Let's divide and mod to get the right bits.  A common case is shift=0, mask=01.
				snprintf(lookupMethod, sizeof(lookupMethod), "mod(index.%c * %f, %d.0)", rgba[shift], 15.99f / (1 << rgba_shift), mask + 1);
				index_multiplier = 1.0f / 256.0f;
				formatOK = mask <= 15 - (1 << rgba_shift);
			}
		} else {
			formatOK = false;
		}
		break;
	case GE_FORMAT_565:
		if ((mask & (mask + 1)) == 0 && shift < 16) {
			const u8 shifts[16] = { 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4 };
			const int multipliers[16] = { 31, 31, 31, 31, 31, 63, 63, 63, 63, 63, 63, 31, 31, 31, 31, 31 };
			const char *rgba = "rrrrrggggggbbbbb";
			const u8 rgba_shift = shifts[shift];
			if (rgba_shift == 0 && mask == multipliers[shift]) {
				snprintf(lookupMethod, sizeof(lookupMethod), "index.%c", rgba[shift]);
				index_multiplier = multipliers[shift] / 256.0f;
			} else {
				// We just need to divide the right component by the right value, and then mod against the mask.
				// A common case is shift=1, mask=0f.
				snprintf(lookupMethod, sizeof(lookupMethod), "mod(index.%c * %f, %d.0)", rgba[shift], ((float)multipliers[shift] + 0.99f) / (1 << rgba_shift), mask + 1);
				index_multiplier = 1.0f / 256.0f;
				formatOK = mask <= multipliers[shift] - (1 << rgba_shift);
			}
		} else {
			formatOK = false;
		}
		break;
	case GE_FORMAT_5551:
		if (config.textureFormat == GE_TFMT_CLUT8 && mask == 0xFF && shift == 0) {
			// Follow the intent here, and ignore g (and let's not round unnecessarily).
			snprintf(lookupMethod, sizeof(lookupMethod), "floor(floor(index.a) * 128.0 + index.b * 64.0)");
			index_multiplier = 1.0f / 256.0f;
			// SOCOM case. #16210
		} else if ((mask & (mask + 1)) == 0 && shift < 16) {
			const char *rgba = "rrrrrgggggbbbbba";
			const u8 rgba_shift = shift % 5;
			if (rgba_shift == 0 && mask == 0x1F) {
				snprintf(lookupMethod, sizeof(lookupMethod), "index.%c", rgba[shift]);
				index_multiplier = 31.0f / 256.0f;
			} else if (shift == 15 && mask == 1) {
				snprintf(lookupMethod, sizeof(lookupMethod), "index.%c", rgba[shift]);
				index_multiplier = 1.0f / 256.0f;
			} else {
				// A isn't possible here.
				snprintf(lookupMethod, sizeof(lookupMethod), "mod(index.%c * %f, %d.0)", rgba[shift], 31.99f / (1 << rgba_shift), mask + 1);
				index_multiplier = 1.0f / 256.0f;
				formatOK = mask <= 31 - (1 << rgba_shift);
			}
		} else {
			formatOK = false;
		}
		break;
	case GE_FORMAT_DEPTH16:
	{
		// TODO: I think we can handle most scenarios here, but texturing from depth buffers requires an extension on ES 2.0 anyway.
		if (config.bufferFormat == GE_FORMAT_DEPTH16 && config.textureFormat == GE_TFMT_5650) {
			// Convert depth to 565, without going through a CLUT.
			writer.C("  float depth = index.x * 65535.0;\n");
			writer.C("  float idepth = floor(clamp(depth, 0.0, 65535.0));\n");  // clamping should no longer be necessary.
			writer.C("  float r = mod(idepth, 32.0) / 31.0;\n");
			writer.C("  float g = mod(floor(idepth / 32.0), 64.0) / 63.0;\n");
			writer.C("  float b = mod(floor(idepth / 2048.0), 32.0) / 31.0;\n");
			writer.C("  vec4 outColor = vec4(r, g, b, 1.0);\n");
			return;
		}

		if (shift < 16) {
			index_multiplier = 1.0f / (float)(1 << shift);
			truncate_cpy(lookupMethod, "(index.x * 65535.0)");

			if ((mask & (mask + 1)) != 0) {
				// But we'll try with the above anyway.
				formatOK = false;
			}
		} else {
			formatOK = false;
		}
		break;
	}
	default:
		break;
	}

	// We always use 512-sized textures now.
	float texturePixels = 512.f;
	index_multiplier *= 0.5f;

	// Adjust index_multiplier, similar to the use of 15.99 instead of 16 in the ES 3 path.
	// index_multiplier -= 0.01f / texturePixels;

	if (!formatOK) {
		ERROR_LOG_ONCE(depal, Log::G3D, "%s depal unsupported: shift=%d mask=%02x offset=%d", GeBufferFormatToString(config.bufferFormat), shift, mask, config.startPos);
	}

	// Offset by half a texel (plus clutBase) to turn NEAREST filtering into FLOOR.
	// Technically, the clutBase should be |'d, not added, but that's hard with floats.
	float texel_offset = ((float)config.startPos + 0.5f) / texturePixels;

	writer.F("  float coord = (%s * %f) + %f;\n", lookupMethod, index_multiplier, texel_offset);
	writer.C("  vec4 outColor = ").SampleTexture2D("pal", "vec2(coord, 0.0)").C(";\n");
}

void GenerateDepalSmoothed(ShaderWriter &writer, const DepalConfig &config) {
	const char *sourceChannel = "error";
	float indexMultiplier = 31.0f;

	if (config.bufferFormat == GE_FORMAT_5551) {
		_dbg_assert_(config.mask == 0x1F);
		switch (config.shift) {
		case 0: sourceChannel = "r"; break;
		case 5: sourceChannel = "g"; break;
		case 10: sourceChannel = "b"; break;
		default: _dbg_assert_(false);
		}
	} else if (config.bufferFormat == GE_FORMAT_565) {
		_dbg_assert_(config.mask == 0x1F || config.mask == 0x3F);
		switch (config.shift) {
		case 0: sourceChannel = "r"; break;
		case 5: sourceChannel = "g"; indexMultiplier = 63.0f; break;
		case 11: sourceChannel = "b"; break;
		default: _dbg_assert_(false);
		}
	} else {
		_dbg_assert_(false);
	}

	writer.C("  float index = ").SampleTexture2D("tex", "v_texcoord").F(".%s * %0.1f;\n", sourceChannel, indexMultiplier);
	float texturePixels = 512.f;
	writer.F("  float coord = (index + 0.5) * %f;\n", 1.0 / texturePixels);
	writer.C("  vec4 outColor = ").SampleTexture2D("pal", "vec2(coord, 0.0)").C(";\n");
}

void GenerateDepalFs(ShaderWriter &writer, const DepalConfig &config) {
	if (writer.Lang().shaderLanguage == GLSL_VULKAN) {
		GenerateVulkanDepalFs(writer, config);
		return;
	}
	writer.DeclareSamplers(samplers);
	writer.HighPrecisionFloat();
	writer.BeginFSMain(config.bufferFormat == GE_FORMAT_DEPTH16 ? g_draw2Duniforms : Slice<UniformDef>::empty(), varyings);
	if (config.smoothedDepal) {
		// Handles a limited set of cases, but doesn't need any integer math so we don't
		// need two variants.
		GenerateDepalSmoothed(writer, config);
	} else {
		switch (writer.Lang().shaderLanguage) {
		case HLSL_D3D9:
		case GLSL_1xx:
			GenerateDepalShaderFloat(writer, config);
			break;
		case GLSL_VULKAN:
		case GLSL_3xx:
		case HLSL_D3D11:
			// Use the float shader for the SOCOM special.
			if (config.bufferFormat == GE_FORMAT_5551 && config.textureFormat == GE_TFMT_CLUT8) {
				GenerateDepalShaderFloat(writer, config);
			} else {
				GenerateDepalShader300(writer, config);
			}
			break;
		default:
			_assert_msg_(false, "Shader language not supported for depal: %d", (int)writer.Lang().shaderLanguage);
		}
	}
	writer.EndFSMain("outColor");
}
