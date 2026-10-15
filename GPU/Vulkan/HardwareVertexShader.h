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

inline void WriteHardwareVulkanVertexShader(ShaderWriter &p, const char *baseUniforms, const char *lightUniforms,
	bool flat, bool clip, bool cull, bool stereo, uint32_t id, uint32_t features, uint32_t lights, uint32_t eyeOffset) {
	p.F("layout(constant_id = 210) const uint psp_spec_210 = %uu;\n", id);
	p.F("layout(constant_id = 211) const uint psp_spec_211 = %uu;\n", features);
	p.F("layout(constant_id = 212) const uint psp_spec_212 = %uu;\n", lights);
	p.F("layout(constant_id = 213) const uint psp_spec_213 = %uu;\n", eyeOffset);
	p.F("layout(std140, set = 0, binding = 3) uniform baseVars {\n%s};\n", baseUniforms);
	p.F("layout(std140, set = 0, binding = 4) uniform lightVars {\n%s};\n", lightUniforms);
	p.C("layout(location = 0) in vec3 position;\n");
	p.C("layout(location = 1) in vec2 texcoord;\n");
	p.C("layout(location = 2) in vec3 normal;\n");
	p.C("layout(location = 5) in vec4 color0;\n");
	p.F("layout(location = 1) %sout highp vec4 v_color0;\n", flat ? "flat " : "");
	p.F("layout(location = 2) %sout highp vec3 v_color1;\n", flat ? "flat " : "");
	p.C("layout(location = 0) out highp vec3 v_texcoord;\n");
	p.C("layout(location = 3) out highp float v_fogdepth;\n");
	p.C("layout(location = 4) out highp vec2 v_zw;\n");
	p.C(R"(invariant gl_Position;
#define THROUGH ((psp_spec_210 & 1u) != 0u)
#define HAS_NORMAL ((psp_spec_210 & 4u) != 0u)
#define HAS_UV ((psp_spec_210 & 8u) != 0u)
#define HAS_COLOR ((psp_spec_210 & 16u) != 0u)
#define LMODE ((psp_spec_210 & 32u) != 0u)
#define REVERSE_NORMAL ((psp_spec_210 & 64u) != 0u)
#define LIGHTING ((psp_spec_210 & 16777216u) != 0u)
#define UBER ((psp_spec_210 & 536870912u) != 0u)
vec3 normalizeOr001(vec3 v) {
  float len2 = dot(v, v);
  return len2 == 0.0 ? vec3(0.0, 0.0, 1.0) : (v * inversesqrt(len2));
}
vec3 normalizeOr000(vec3 v) {
  float len2 = dot(v, v);
  return len2 == 0.0 ? vec3(0.0) : (v * inversesqrt(len2));
}
float pspPow(float x, float e) {
  float t = max(e, 0.0) * float(floatBitsToInt(max(x, 1e-30)) - 1065353216) + 1065353216.0;
  return x > 0.0 || e <= 0.0 ? intBitsToFloat(int(max(t, 0.0))) : 0.0;
}
vec3 shadeLight(uint index, vec3 worldpos, vec3 worldnormal, vec3 viewDir, uint control) {
  uint comp = (control >> (4u + 4u * index)) & 3u;
  uint type = (control >> (6u + 4u * index)) & 3u;
  vec3 light = u_lightpos[index];
  if (type != 0u) light -= worldpos;
  if (UBER || type != 0u) light = normalizeOr000(light);
  if (comp == 1u) light = normalizeOr000(light + viewDir);
  return light;
}
void main() {
  uint uvMode = (psp_spec_210 >> 12u) & 3u;
  uint projection = (psp_spec_210 >> 14u) & 3u;
  uint control = UBER ? u_lightControl : psp_spec_212;
  vec3 modelNormal = HAS_NORMAL ? normal : vec3(0.0, 0.0, 1.0);
  if (REVERSE_NORMAL) modelNormal = -modelNormal;
  vec3 worldpos = (vec4(position, 1.0) * u_world).xyz;
  highp vec3 worldnormal = normalizeOr001((vec4(modelNormal, 0.0) * u_world).xyz);
  highp vec3 viewDir = normalizeOr001(u_view[2].xyz);
  vec4 viewPos = vec4((vec4(worldpos, 1.0) * u_view).xyz, 1.0);
)");
	if (stereo) {
		p.C("  viewPos.x += uintBitsToFloat(psp_spec_213) * float(gl_ViewIndex * 2 - 1);\n");
	}
	p.C("  vec4 outPos = u_proj * viewPos;\n  bool zClipped = outPos.z < -outPos.w;\n");
	if (clip) {
		p.C("  gl_ClipDistance[0] = 1.0;\n  gl_ClipDistance[1] = 1.0;\n  gl_ClipDistance[2] = outPos.z + outPos.w;\n");
	}
	if (cull) {
		p.C(R"(  gl_CullDistance[0] = 1.0;
  gl_CullDistance[1] = 1.0;
  if (!THROUGH) {
    gl_CullDistance[0] = outPos.z + outPos.w + 0.0000304 / outPos.w;
    gl_CullDistance[1] = outPos.w - outPos.z + 0.0000304 / outPos.w;
  }
)");
	}
	p.C(R"(  float recip = 1.0 / outPos.w;
  outPos.xyz = (outPos.xyz * u_vpScale.xyz) * recip + u_vpOffset.xyz;
  v_zw = vec2(outPos.z * outPos.w, outPos.w);
  v_color1 = vec3(0.0);
  if (LIGHTING) {
    vec4 ambientColor = HAS_COLOR && ((control & (1u << 20u)) != 0u) ? color0 : u_matambientalpha;
    vec3 diffuseColor = HAS_COLOR && ((control & (1u << 21u)) != 0u) ? color0.rgb : u_matdiffuse;
    vec3 specularColor = HAS_COLOR && ((control & (1u << 22u)) != 0u) ? color0.rgb : u_matspecular.rgb;
    highp vec4 lightSum0 = u_ambient * ambientColor + vec4(u_matemissive, 0.0);
    highp vec3 lightSum1 = vec3(0.0);
 )");
	// Constant UBO indices avoid relying on driver loop unrolling or dynamic
	// uniform indexing, including devices with UNIFORM_INDEXING_BROKEN.
	for (uint32_t light = 0; light < 4; ++light) {
		p.F("    { const uint i = %uu;\n", light);
		p.C(R"(      if ((control & (1u << i)) != 0u) {
      uint comp = (control >> (4u + 4u * i)) & 3u;
      uint type = (control >> (6u + 4u * i)) & 3u;
      vec3 toLight = u_lightpos[i];
      highp float lightScale = 1.0;
      if (type != 0u) {
        toLight -= worldpos;
        float distSq = dot(toLight, toLight);
        float invDist = inversesqrt(distSq);
        float distance = distSq * invDist;
        toLight *= invDist;
        float attenuation = clamp(1.0 / dot(u_lightatt[i], vec3(1.0, distance, distSq)), 0.0, 1.0);
        if (type == 1u) {
          lightScale = attenuation;
        } else {
          highp float angle = dot(u_lightdir[i], toLight);
          if (angle >= u_lightangle_spotCoef[i].x) {
            lightScale = attenuation * pspPow(angle, u_lightangle_spotCoef[i].y);
          } else {
            lightScale = 0.0;
          }
        }
      }
      highp float ldot = dot(toLight, worldnormal);
      if (comp == 2u) ldot = pspPow(ldot, u_matspecular.a);
      highp vec3 diffuse = (u_lightdiffuse[i] * diffuseColor) * max(ldot, 0.0);
      if (comp == 1u && ldot >= 0.0) {
        vec3 halfVec = toLight + viewDir;
        float halfInvLen = inversesqrt(dot(halfVec, halfVec));
        ldot = pspPow(dot(halfVec, worldnormal) * halfInvLen, u_matspecular.a);
        if (UBER || ldot > 0.0) {
          if (type == 0u && !UBER) {
            lightSum1 += u_lightspecular[i] * specularColor * ldot;
          } else {
            lightSum1 += u_lightspecular[i] * specularColor * ldot * lightScale;
          }
        }
      }
      if (type == 0u && !UBER) {
        lightSum0.rgb += (u_lightambient[i] * ambientColor.rgb + diffuse);
      } else {
        lightSum0.rgb += (u_lightambient[i] * ambientColor.rgb + diffuse) * lightScale;
      }
      }
    }
)");
	}
	p.C(R"(    if (LMODE) {
      v_color0 = clamp(lightSum0, 0.0, 1.0);
      v_color1 = clamp(lightSum1, 0.0, 1.0);
    } else {
      v_color0 = clamp(vec4(lightSum0.rgb + lightSum1, lightSum0.a), 0.0, 1.0);
    }
  } else {
    v_color0 = HAS_COLOR ? color0 : u_matambientalpha;
    if (!HAS_COLOR && (psp_spec_211 & 64u) != 0u) v_color0.r += 0.000001;
  }
  vec2 uv = HAS_UV ? texcoord : vec2(0.0);
  if (uvMode == 0u || uvMode == 3u) {
    v_texcoord = vec3(uv * u_uvscaleoffset.xy, 0.0);
    if (THROUGH) v_texcoord.xy += u_uvscaleoffset.zw;
  } else if (uvMode == 1u) {
    vec4 source;
    if (projection == 0u) source = vec4(position, 1.0);
    else if (projection == 1u) source = vec4(uv, 0.0, 1.0);
    else if (!HAS_NORMAL) source = vec4(0.0, 0.0, 1.0, 1.0);
    else if (projection == 2u) source = length(normal) == 0.0 ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(normalize(modelNormal), 1.0);
    else source = vec4(modelNormal, 1.0);
    v_texcoord = (source * u_texmtx).xyz * vec3(u_uvscaleoffset.xy, 1.0);
  } else {
    vec3 shadeL0 = shadeLight((psp_spec_210 >> 25u) & 3u, worldpos, worldnormal, viewDir, control);
    vec3 shadeL1 = shadeLight((psp_spec_210 >> 27u) & 3u, worldpos, worldnormal, viewDir, control);
    v_texcoord = vec3(u_uvscaleoffset.xy * vec2(1.0 + dot(shadeL0, worldnormal), 1.0 + dot(shadeL1, worldnormal)) * 0.5, 1.0);
  }
  v_fogdepth = (viewPos.z + u_fogcoef.x) * u_fogcoef.y;
  if (!THROUGH) {
    if ((psp_spec_211 & 1u) != 0u && !zClipped &&
        (outPos.x < 0.0 || outPos.y < 0.0 || outPos.x >= 4096.0 || outPos.y >= 4096.0 || outPos.w < -1.0)) {
      outPos = vec4(u_NaN);
    }
    outPos.xy -= u_rasterOffset.xy;
  }
)");
	if (clip) {
		p.C(R"(  if (!THROUGH) {
    float clipZNear = floor(outPos.z * 0.5 + 0.5) * 2.0;
    float clipZFar = floor(outPos.z * 0.5) * 2.0;
    gl_ClipDistance[0] = u_minZmaxZ.x > 0.0 ? (clipZNear - u_minZmaxZ.x) * outPos.w : 1.0;
    gl_ClipDistance[1] = u_minZmaxZ.y < 65535.0 ? (u_minZmaxZ.y - clipZFar) * outPos.w : 1.0;
  }
)");
	}
	p.C(R"(  outPos.xy = (((outPos.xy + u_xywh.xy) * u_xywh.zw) - vec2(1.0)) * outPos.w;
  if ((psp_spec_211 & 2u) != 0u) outPos.z = floor(outPos.z * 0.5) * 2.0;
  outPos.z *= outPos.w * (1.0 / 65536.0);
  if ((psp_spec_211 & 4u) != 0u) {
    mat2 displayRotation = mat2(
      u_rotation == 0.0 ? 1.0 : (u_rotation == 2.0 ? -1.0 : 0.0), u_rotation == 1.0 ? 1.0 : (u_rotation == 3.0 ? -1.0 : 0.0),
      u_rotation == 3.0 ? 1.0 : (u_rotation == 1.0 ? -1.0 : 0.0), u_rotation == 0.0 ? 1.0 : (u_rotation == 2.0 ? -1.0 : 0.0));
    outPos.xy = displayRotation * outPos.xy;
  }
  if ((psp_spec_211 & 8u) != 0u) outPos.y *= -1.0;
  gl_Position = outPos;
  if ((psp_spec_211 & 16u) != 0u) {
    gl_Position.z = (u_minZmaxZ.x + u_minZmaxZ.y) * (0.5 / 65536.0) * outPos.w;
  }
}
)");
}
