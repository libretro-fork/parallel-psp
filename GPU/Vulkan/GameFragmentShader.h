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

// Texture kinds: 0 absent, 1 2D, 2 3D, 3 array. Depth: 0 unwritten,
// 1 replacement depth, 2 depth_unchanged driver workaround.
inline void WriteGameVulkanFragmentShader(ShaderWriter &p, const char *baseUniforms,
	int textureKind, bool palette, bool framebuffer, bool flat, bool stereo, bool dualSource, int depth,
	uint32_t idLow, uint32_t idHigh, uint32_t features) {
	p.F("layout(constant_id = 220) const uint psp_spec_220 = %uu;\n", idLow);
	p.F("layout(constant_id = 221) const uint psp_spec_221 = %uu;\n", idHigh);
	p.F("layout(constant_id = 222) const uint psp_spec_222 = %uu;\n", features);
	p.F("layout(std140, set = 0, binding = 3) uniform baseVars {\n%s};\n", baseUniforms);
	if (textureKind) {
		p.F("layout(set = 0, binding = 0) uniform %s tex;\n", textureKind == 2 ? "sampler3D" : textureKind == 3 ? "sampler2DArray" : "sampler2D");
	}
	if (palette) {
		p.C("layout(set = 0, binding = 2) uniform sampler2D pal;\n");
	}
	if (framebuffer) {
		p.C("layout(set = 0, binding = 1) uniform sampler2DArray fbotex;\n");
	}
	p.F("layout(location = 1) %sin highp vec4 v_color0;\n", flat ? "flat " : "");
	p.F("layout(location = 2) %sin highp vec3 v_color1;\n", flat ? "flat " : "");
	p.C("layout(location = 0) in highp vec3 v_texcoord;\n");
	p.C("layout(location = 3) in highp float v_fogdepth;\n");
	p.C("layout(location = 4) in highp vec2 v_zw;\n");
	p.C("layout(location = 0, index = 0) out vec4 fragColor0;\n");
	if (dualSource) {
		p.C("layout(location = 0, index = 1) out vec4 fragColor1;\n");
	}
	if (depth == 2) {
		p.C("layout(depth_unchanged) out float gl_FragDepth;\n");
	}
	p.C(R"(
#define CLEAR ((psp_spec_220 & 1u) != 0u)
#define PROJECTION ((psp_spec_220 & 8u) != 0u)
#define TEXFUNC ((psp_spec_220 >> 4u) & 7u)
#define LMODE ((psp_spec_220 & 128u) != 0u)
#define FOG ((psp_spec_220 & 256u) != 0u)
#define DEPTH_CLAMP ((psp_spec_220 & 1024u) != 0u)
#define MINMAX ((psp_spec_220 & 2048u) != 0u)
#define ALPHA_TEST ((psp_spec_220 & 4096u) != 0u)
#define ALPHA_FUNC ((psp_spec_220 >> 13u) & 7u)
#define ALPHA_ZERO ((psp_spec_220 & 65536u) != 0u)
#define COLOR_TEST ((psp_spec_220 & 131072u) != 0u)
#define COLOR_FUNC ((psp_spec_220 >> 18u) & 3u)
#define COLOR_ZERO ((psp_spec_220 & 1048576u) != 0u)
#define CLAMP_S ((psp_spec_220 & 4194304u) != 0u)
#define CLAMP_T ((psp_spec_220 & 8388608u) != 0u)
#define STENCIL ((psp_spec_220 >> 24u) & 3u)
#define STENCIL_VALUE ((psp_spec_220 >> 26u) & 15u)
#define SIMULATE_LOGIC (psp_spec_220 >> 30u)
#define BLEND (psp_spec_221 & 7u)
#define BLEND_EQ ((psp_spec_221 >> 3u) & 7u)
#define SRC_FACTOR ((psp_spec_221 >> 6u) & 15u)
#define DST_FACTOR ((psp_spec_221 >> 10u) & 15u)
#define TEST_ZERO ((psp_spec_221 & 16384u) != 0u)
#define COLOR_MASK ((psp_spec_221 & 65536u) != 0u)
#define LOGIC (CLEAR ? 3u : ((psp_spec_221 >> 17u) & 15u))
#define DEPAL ((psp_spec_221 >> 21u) & 3u)
#define DEPAL_FORMAT ((psp_spec_221 >> 23u) & 7u)
vec3 blendFactor(uint factor, vec4 v, vec4 d, bool source) {
  switch (factor) {
  case 0u: return source ? d.rgb : v.rgb;
  case 1u: return vec3(1.0) - (source ? d.rgb : v.rgb);
  case 2u: return v.aaa;
  case 3u: return vec3(1.0 - v.a);
  case 4u: return d.aaa;
  case 5u: return vec3(1.0) - d.aaa;
  case 6u: return v.aaa * 2.0;
  case 7u: return vec3(1.0) - v.aaa * 2.0;
  case 8u: return d.aaa * 2.0;
  case 9u: return vec3(1.0) - d.aaa * 2.0;
  default: return source ? u_blendFixA : u_blendFixB;
  }
}
bool alphaFail(float a) {
  if (ALPHA_ZERO) {
    if (ALPHA_FUNC == 3u || ALPHA_FUNC == 6u) return a < 0.002;
    if (ALPHA_FUNC != 0u) return a > 0.002;
    return true;
  }
  int value = int(floor(a * 255.0 + 0.5)) & int(u_alphacolormask >> 24u);
  int ref = int(u_alphacolorref >> 24u);
  switch (ALPHA_FUNC) {
  case 2u: return value != ref;
  case 3u: return value == ref;
  case 4u: return value >= ref;
  case 5u: return value > ref;
  case 6u: return value <= ref;
  case 7u: return value < ref;
  default: return true;
  }
}
bool colorFail(vec3 color) {
  if (COLOR_ZERO) {
    if (COLOR_FUNC == 3u) return color.r + color.g + color.b < 0.002;
    if (COLOR_FUNC != 0u) return color.r + color.g + color.b > 0.002;
    return true;
  }
  uvec3 c = uvec3(floor(color * 255.0 + 0.5));
  uint v = (c.r | (c.g << 8u) | (c.b << 16u)) & u_alphacolormask;
  uint ref = (u_alphacolorref & u_alphacolormask) & 0xFFFFFFu;
  if (COLOR_FUNC == 2u) return v != ref;
  if (COLOR_FUNC == 3u) return v == ref;
  return true;
}
)");
	if (palette) {
		p.C(R"(uint depalIndex(vec4 t) {
  uvec4 c;
  switch (DEPAL_FORMAT) {
  case 0u: c=uvec4(t.rgb*vec3(31.99,63.99,31.99),0); return (c.b<<11u)|(c.g<<5u)|c.r;
  case 1u: c=uvec4(t*vec4(31.99,31.99,31.99,1.0)); return (c.a<<15u)|(c.b<<10u)|(c.g<<5u)|c.r;
  case 2u: c=uvec4(t*15.99); return (c.a<<12u)|(c.b<<8u)|(c.g<<4u)|c.r;
  case 3u: c=uvec4(t*255.99); return (c.a<<24u)|(c.b<<16u)|(c.g<<8u)|c.r;
  case 4u: return uint(t.r*65535.99);
  case 5u: return uint(t.r*255.99);
  default: return 0u;
  }
}
)");
	}
	p.C("void main() {\n  vec4 destColor = vec4(0.0);\n");
	if (framebuffer) {
		p.F("  destColor = texelFetch(fbotex, ivec3(gl_FragCoord.xy, %s), 0);\n", stereo ? "gl_ViewIndex" : "0");
	}
	p.C("  vec4 v = v_color0;\n  if (!CLEAR) {\n    vec4 secondary = LMODE ? vec4(v_color1, 0.0) : vec4(0.0);\n");
	if (textureKind) {
		p.C(R"(    highp vec3 tc = v_texcoord;
    bool project = PROJECTION;
    if ((psp_spec_222 & 1u) != 0u) {
      vec2 uv = project ? tc.xy / tc.z : tc.xy;
      float u = CLAMP_S ? clamp(uv.x, u_texclamp.z, u_texclamp.x-u_texclamp.z) : uv.x-u_texclamp.x*floor(uv.x/u_texclamp.x);
      float w = CLAMP_T ? clamp(uv.y, u_texclamp.w, u_texclamp.y-u_texclamp.w) : uv.y-u_texclamp.y*floor(uv.y/u_texclamp.y);
      if ((psp_spec_222 & 2u) == 0u) {
        if (!CLAMP_S) u=mod(uv.x,u_texclamp.x);
        if (!CLAMP_T) w=mod(uv.y,u_texclamp.y);
      }
      tc = vec3(vec2(u,w)+u_texclampoff,1.0);
      project=false;
    }
    vec4 t;
)");
		if (textureKind == 2) {
			p.C("    if (project) t=textureProj(tex,vec4(tc.xy,u_mipBias,tc.z));\n    else t=texture(tex,vec3(tc.xy,u_mipBias));\n");
		} else {
			const char *layer = stereo ? "float(gl_ViewIndex)" : "0.0";
			if (textureKind == 3) {
				p.C("    vec2 uv=project ? tc.xy/tc.z : tc.xy;\n");
				if (!palette) {
					p.F("    t=texture(tex,vec3(uv,%s));\n", layer);
				}
			} else {
				if (!palette) {
					p.C("    if (project) t=textureProj(tex,tc);\n    else t=texture(tex,tc.xy);\n");
				}
				p.C("    vec2 uv=project ? tc.xy/tc.z : tc.xy;\n");
			}
			if (palette) {
				p.C("    if (DEPAL == 2u) {\n");
				if (textureKind == 3) {
					p.F("      t=texture(tex,vec3(uv,%s));\n", layer);
				} else {
					p.C("      t=texture(tex,uv);\n");
				}
				p.C(R"(
      uint shift=(u_depal_mask_shift_off_fmt>>8u)&255u;
      uint offset=((u_depal_mask_shift_off_fmt>>16u)&255u)<<4u;
      uint fmt=(u_depal_mask_shift_off_fmt>>24u)&3u;
      float index=t.r, factor=31.0/256.0;
      if (fmt==0u) {
        if (shift==5u) { index=t.g; factor=63.0/256.0; }
        else if (shift==11u) index=t.b;
      } else {
        if (shift==5u) index=t.g;
        else if (shift==10u) index=t.b;
      }
      t=texture(pal,vec2((index*factor+float(offset)/256.0)*0.5+0.5/512.0,0.0));
    } else if (DEPAL == 1u) {
      vec2 size=vec2(textureSize(tex,0).xy);
      bool linear=(u_depal_mask_shift_off_fmt>>31u)!=0u;
      vec2 fraction=vec2(0.0);
      vec2 point=uv;
      if (linear) {
        point=uv*size-vec2(0.5);
        fraction=fract(point);
        point=(point-fraction+vec2(0.5))/size;
      }
)");
				if (textureKind == 3) {
					p.F("      vec3 samplePoint=vec3(point,%s);\n", layer);
				} else {
					p.C("      vec2 samplePoint=point;\n");
				}
				p.C(R"(      t=texture(tex,samplePoint);
      vec4 t1=textureOffset(tex,samplePoint,ivec2(1,0));
      vec4 t2=textureOffset(tex,samplePoint,ivec2(0,1));
      vec4 t3=textureOffset(tex,samplePoint,ivec2(1,1));
      uint i0=depalIndex(t),i1=0u,i2=0u,i3=0u;
      if (linear) { i1=depalIndex(t1); i2=depalIndex(t2); i3=depalIndex(t3); }
      uint mask=u_depal_mask_shift_off_fmt&255u;
      uint shift=(u_depal_mask_shift_off_fmt>>8u)&255u;
      uint offset=((u_depal_mask_shift_off_fmt>>16u)&255u)<<4u;
      i0=((i0>>shift)&mask)|offset;
      t=texelFetch(pal,ivec2(i0,0),0);
      if (linear) {
        i1=((i1>>shift)&mask)|offset; i2=((i2>>shift)&mask)|offset; i3=((i3>>shift)&mask)|offset;
      }
      if (linear && !(i0==i1 && i1==i2 && i2==i3)) {
        t1=texelFetch(pal,ivec2(i1,0),0); t2=texelFetch(pal,ivec2(i2,0),0); t3=texelFetch(pal,ivec2(i3,0),0);
        t=mix(t,t1,fraction.x); t2=mix(t2,t3,fraction.x); t=mix(t,t2,fraction.y);
      }
    } else if (DEPAL == 3u) {
      vec2 size=vec2(textureSize(tex,0).xy);
      vec2 point=floor(uv*size);
      int component=int(point.x)&3;
      point.x*=0.25; point/=size;
)");
				if (textureKind == 3) {
					p.F("      t=texture(tex,vec3(point,%s));\n", layer);
				} else {
					p.C("      t=texture(tex,point);\n");
				}
				p.C("      int index=int(t[component]*255.99);\n      t=texelFetch(pal,ivec2(index,0),0);\n    }\n");
			}
		}
		p.C(R"(    vec4 primary=v_color0;
    if (TEXFUNC!=3u) t.a=max(t.a,u_texNoAlphaMul.x);
    switch (TEXFUNC) {
    case 0u: v=primary*t+secondary; break;
    case 1u: v=vec4(mix(primary.rgb,t.rgb,t.a),primary.a)+secondary; break;
    case 2u: v=vec4(mix(primary.rgb,u_texenv.rgb,t.rgb),primary.a*t.a)+secondary; break;
    case 3u: t.a=mix(t.a,primary.a,u_texNoAlphaMul.x); v=t+secondary; break;
    default: v=vec4(primary.rgb+t.rgb,primary.a*t.a)+secondary; break;
    }
    if (any(bvec4(FOG,COLOR_TEST,BLEND!=0u,SIMULATE_LOGIC!=0u)) || COLOR_MASK) {
      v.rgb=clamp(v.rgb*u_texNoAlphaMul.y,0.0,1.0);
    } else v.rgb*=u_texNoAlphaMul.y;
)");
	} else {
		p.C("    v=v_color0+secondary;\n");
	}
	p.C(R"(    if (FOG) v.rgb=mix(u_fogcolor,v.rgb,clamp(v_fogdepth,0.0,1.0));
    if (ALPHA_TEST && alphaFail(v.a)) {
      if (TEST_ZERO) v.a=0.0; else discard;
    }
    if (COLOR_TEST && colorFail(v.rgb)) {
      if (TEST_ZERO) v.a=0.0; else discard;
    }
    if (BLEND==5u) v.rgb*=2.0;
    if (((1u << BLEND) & 140u) != 0u) {
      vec3 factor;
      switch (SRC_FACTOR) {
      case 2u: factor=vec3(v.a); break;
      case 3u: factor=vec3(1.0-v.a); break;
      case 6u: factor=vec3(v.a*2.0); break;
      case 7u: factor=vec3(1.0-v.a*2.0); break;
      case 8u: factor=vec3(2.0); break;
      default: factor=u_blendFixA; break;
      }
      v.rgb*=factor;
    }
    if (BLEND==6u) {
      vec3 a=blendFactor(SRC_FACTOR,v,destColor,true);
      vec3 b=blendFactor(DST_FACTOR,v,destColor,false);
      switch (BLEND_EQ) {
      case 0u: v.rgb=v.rgb*a+destColor.rgb*b; break;
      case 1u: v.rgb=v.rgb*a-destColor.rgb*b; break;
      case 2u: v.rgb=destColor.rgb*b-v.rgb*a; break;
      case 3u: v.rgb=min(v.rgb,destColor.rgb); break;
      case 4u: v.rgb=max(v.rgb,destColor.rgb); break;
      case 5u: v.rgb=abs(v.rgb-destColor.rgb); break;
      }
    }
    if (BLEND==3u || BLEND==4u) v.a*=2.0;
  }
  float replacedAlpha=0.0;
  switch (STENCIL_VALUE) {
  case 0u: replacedAlpha=u_stencilReplaceValue; break;
  case 2u: case 4u: replacedAlpha=1.0; break;
  case 5u: case 7u: replacedAlpha=0.066667; break;
  case 6u: case 8u: replacedAlpha=0.003922; break;
  }
  fragColor0=STENCIL==0u ? v : vec4(v.rgb,replacedAlpha);
)");
	if (dualSource) {
		p.C("  fragColor1=vec4(0.0,0.0,0.0,v.a);\n");
	}
	p.C(R"(  if (SIMULATE_LOGIC==1u) fragColor0.rgb=vec3(1.0);
  if (SIMULATE_LOGIC==2u) fragColor0.rgb=vec3(1.0)-fragColor0.rgb;
  if (COLOR_MASK || LOGIC!=3u) {
    uint s=packUnorm4x8(fragColor0),d=packUnorm4x8(destColor);
    uint rgb;
    switch (LOGIC) {
    case 0u: rgb=0u; break;
    case 1u: rgb=s&d; break;
    case 2u: rgb=s&~d; break;
    case 3u: rgb=s; break;
    case 4u: rgb=~s&d; break;
    case 5u: rgb=d; break;
    case 6u: rgb=s^d; break;
    case 7u: rgb=s|d; break;
    case 8u: rgb=~(s|d); break;
    case 9u: rgb=~(s^d); break;
    case 10u: rgb=~d; break;
    case 11u: rgb=s|~d; break;
    case 12u: rgb=~s; break;
    case 13u: rgb=~s|d; break;
    case 14u: rgb=~(s&d); break;
    default: rgb=0xFFFFFFu; break;
    }
    s=(rgb&0xFFFFFFu)|(s&0xFF000000u);
    if (COLOR_MASK) {
      if (STENCIL!=0u) s=(s&u_colorWriteMask)|(d&~u_colorWriteMask);
      else s=(s&u_colorWriteMask&0xFFFFFFu)|(d&(~u_colorWriteMask|0xFF000000u));
    }
    fragColor0=unpackUnorm4x8(s);
  }
  if (BLEND==7u) fragColor0=vec4(0.0,0.0,0.0,fragColor0.z);
  highp float projZ=v_zw.x/v_zw.y;
  if (MINMAX) {
    float nearZ=floor(projZ*0.5+0.5)*2.0,farZ=floor(projZ*0.5)*2.0;
    if (u_minZmaxZ.x>0.0 && nearZ<u_minZmaxZ.x) discard;
    if (u_minZmaxZ.y<65535.0 && farZ>u_minZmaxZ.y) discard;
  }
)");
	if (depth == 1) {
		p.C(R"(  if ((psp_spec_222 & 4u)!=0u) {
    if (DEPTH_CLAMP) gl_FragDepth=clamp(floor(projZ),0.0,65535.0)/65535.0;
    else gl_FragDepth=floor(gl_FragCoord.z*65535.0)/65535.0;
  } else gl_FragDepth=clamp(projZ,0.0,65535.0)/65535.0;
)");
	} else if (depth == 2) {
		p.C("  gl_FragDepth=gl_FragCoord.z;\n");
	}
	p.C("}\n");
}
