// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
#pragma once

#include "Common/GPU/ShaderWriter.h"

// Only interface changes need separate SPIR-V modules. The remaining state is
// encoded in uint specialization defaults, resolved when creating the pipeline.
enum SoftwareVertexShaderFlag : uint32_t {
	SWVS_THROUGH = 1 << 0,
	SWVS_RANGE_CULL = 1 << 1,
	SWVS_ROUND_DEPTH = 1 << 2,
	SWVS_ROTATION = 1 << 3,
	SWVS_FLIP_Y = 1 << 4,
	SWVS_DEPTH_CLAMP = 1 << 5,
};

inline void WriteSoftwareVulkanVertexShader(ShaderWriter &p, const char *baseUniforms,
	bool texture3, bool secondaryColor, bool flat, bool clipMinMax, uint32_t flags) {
	p.F("layout(constant_id = 200) const uint psp_spec_200 = %uu;\n", flags);
	p.F("layout(std140, set = 0, binding = 3) uniform baseVars {\n%s};\n", baseUniforms);
	p.C("layout(location = 0) in vec4 position;\n");
	p.C("layout(location = 2) in float fog;\n");
	p.F("layout(location = 1) in %s texcoord;\n", texture3 ? "vec3" : "vec2");
	p.C("layout(location = 5) in vec4 color0;\n");
	if (secondaryColor) {
		p.C("layout(location = 6) in vec3 color1;\n");
	}
	p.F("layout(location = 2) %sout highp vec3 v_color1;\n", flat ? "flat " : "");
	p.F("layout(location = 1) %sout highp vec4 v_color0;\n", flat ? "flat " : "");
	p.C("layout(location = 0) out highp vec3 v_texcoord;\n");
	p.C("layout(location = 3) out highp float v_fogdepth;\n");
	p.C("layout(location = 4) out highp vec2 v_zw;\n");
	p.C("invariant gl_Position;\nvoid main() {\n");
	p.F("  v_texcoord = %s;\n", texture3 ? "texcoord" : "vec3(texcoord, 1.0)");
	p.C("  v_color0 = color0;\n");
	if (secondaryColor) {
		p.C("  v_color1 = color1;\n");
	} else {
		p.C("  v_color1 = vec3(0.0);\n");
	}
	p.C(R"(  v_fogdepth = fog;
  vec4 outPos = position;
  v_zw = vec2(outPos.z * outPos.w, outPos.w);
  if ((psp_spec_200 & 1u) == 0u) {
    if ((psp_spec_200 & 2u) != 0u &&
        (outPos.x < 0.0 || outPos.y < 0.0 || outPos.x >= 4096.0 || outPos.y >= 4096.0 || outPos.w < -1.0)) {
      outPos = vec4(u_NaN);
    }
    outPos.xy -= u_rasterOffset.xy;
  }
)");
	if (clipMinMax) {
		p.C(R"(  float clipZNear = floor(outPos.z * 0.5 + 0.5) * 2.0;
  float clipZFar = floor(outPos.z * 0.5) * 2.0;
  gl_ClipDistance[0] = u_minZmaxZ.x > 0.0 ? (clipZNear - u_minZmaxZ.x) * outPos.w : 1.0;
  gl_ClipDistance[1] = u_minZmaxZ.y < 65535.0 ? (u_minZmaxZ.y - clipZFar) * outPos.w : 1.0;
)");
	}
	p.C(R"(  outPos.xy = (((outPos.xy + u_xywh.xy) * u_xywh.zw) - vec2(1.0)) * outPos.w;
  if ((psp_spec_200 & 4u) != 0u) {
    outPos.z = floor(outPos.z * 0.5) * 2.0;
  }
  outPos.z *= outPos.w * (1.0 / 65536.0);
  if ((psp_spec_200 & 8u) != 0u) {
    mat2 displayRotation = mat2(
      u_rotation == 0.0 ? 1.0 : (u_rotation == 2.0 ? -1.0 : 0.0), u_rotation == 1.0 ? 1.0 : (u_rotation == 3.0 ? -1.0 : 0.0),
      u_rotation == 3.0 ? 1.0 : (u_rotation == 1.0 ? -1.0 : 0.0), u_rotation == 0.0 ? 1.0 : (u_rotation == 2.0 ? -1.0 : 0.0));
    outPos.xy = displayRotation * outPos.xy;
  }
  if ((psp_spec_200 & 16u) != 0u) {
    outPos.y *= -1.0;
  }
  gl_Position = outPos;
  if ((psp_spec_200 & 32u) != 0u) {
    gl_Position.z = (u_minZmaxZ.x + u_minZmaxZ.y) * (0.5 / 65536.0) * outPos.w;
  }
}
)");
}
