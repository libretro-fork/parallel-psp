#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
"""Compare legacy and bank vertex equations on an offscreen Vulkan device.

Uses the production generators, wrapped in compute dispatches. Requires a host
C++ compiler, glslc, spirv-val and Vulkan. No game or desktop window is opened.
This checks vertex math separately from graphics pipeline interface validation.
"""
import argparse
import concurrent.futures
import contextlib
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("bank", ROOT / "Tools/BuildVulkanShaderBank.py")
bank = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bank)


def dumper():
    prefix = bank.dump_source().split("int main(int argc", 1)[0]
    legacy = (ROOT / "GPU/Common/VertexShaderGenerator.cpp").read_text()
    legacy = legacy[legacy.index("bool GenerateVertexShader("):]
    legacy = legacy.replace("gstate_c.Use(", "Use(")
    legacy = legacy.replace("g_Config.bVendorBugChecksEnabled", "true")
    legacy = legacy.replace("PSP_CoreParameter().compat.vrCompat().UnitsPerMeter", "1.0f")
    legacy = legacy.replace("id.Description()", 'std::string("reference")')
    legacy = re.sub(r"gl_extensions\.[A-Za-z_0-9]+", "0", legacy)
    return prefix + r'''
#include "Common/StringUtils.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "GPU/Common/ShaderId.h"
#include "GPU/Common/VertexShaderGenerator.h"
#define WRITE(p, ...) p.F(__VA_ARGS__)
static uint32_t useFlags;
static bool Use(uint32_t f) { return (useFlags & f) != 0; }
enum { DRAW_BINDING_DYNUBO_BASE = 3, DRAW_BINDING_DYNUBO_LIGHT = 4 };
namespace Reference {
''' + legacy + r'''}
int main(int argc, char **argv) {
 if (argc != 2) return 2;
 directory = argv[1];
 uint64_t random = 0x123456789abcdefULL;
 auto next = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
 int count = 0;
 for (int hw = 0; hw < 2; ++hw) {
  for (int n = 0; n < 128; ++n) {
   VShaderID id;
   id.FromUint64(next() & ((1ULL << 52) - 1));
   id.SetBit(VS_BIT_USE_HW_TRANSFORM, hw);
   id.SetBit(VS_BIT_IS_THROUGH, !hw && n % 3 == 0);
   // Exercise each UV source with and without supplied vertex attributes.
   id.SetBits(VS_BIT_UVGEN_MODE, 2, n % 4);
   id.SetBits(VS_BIT_UVPROJ_MODE, 2, (n / 4) % 4);
   useFlags = 0;
   if (n & 1) useFlags |= GPU_USE_CLIP_DISTANCE;
   if (n & 2) useFlags |= GPU_USE_CULL_DISTANCE;
   if (n & 4) useFlags |= GPU_ROUND_DEPTH_TO_16BIT;
   if (n & 8) useFlags |= GPU_USE_PRE_ROTATION;
   if (n & 16) useFlags |= GPU_USE_NONBUFFERED_FLIP;
   Draw::Bugs bugs;
   if (n % 7 == 0) bugs.Infest(Draw::Bugs::BROKEN_NAN_IN_CONDITIONAL);
   if (n % 11 == 0) bugs.Infest(Draw::Bugs::MALI_CONSTANT_LOAD_BUG);
   if (n % 13 == 0) bugs.Infest(Draw::Bugs::BROKEN_FLAT_IN_SHADER);
   for (int compiled = 0; compiled < 2; ++compiled) {
    ShaderLanguageDesc language(GLSL_VULKAN);
    char code[32768]; uint32_t attributes; uint64_t uniforms; VertexShaderFlags flags; std::string error;
    if (!Reference::GenerateVertexShader(id, code, language, bugs, &attributes, &uniforms, &flags, &error, compiled)) std::abort();
    Emit("case_" + std::to_string(count) + (compiled ? "_new.vert" : "_ref.vert"), code);
   }
   ++count;
  }
 }
 printf("%d vertex cases\n", count);
}
'''


def kernel(code):
    code = code.replace("#extension GL_EXT_multiview : enable", "")
    uniform_source = (ROOT / "GPU/Common/ShaderUniforms.h").read_text()
    blocks = re.findall(r'R"\((.*?)\)"', uniform_source, re.S)
    uniforms = "layout(std140, set=0, binding=0) uniform Vars {\n" + "\n".join(blocks[:2]) + "};"
    code = re.sub(r"layout\s*\(std140, set = 0, binding = 3\) uniform \w+\s*\{(.*?)\};", lambda m: uniforms, code, flags=re.S)
    code = re.sub(r"layout\s*\(std140, set = 0, binding = 4\) uniform \w+\s*\{(.*?)\};", "", code, flags=re.S)
    inputs = []

    def interface(match):
        direction, typename, name = match.groups()
        if direction == "in":
            inputs.append((typename, name))
        return typename + " " + name + ";"

    code = re.sub(r"layout\s*\(\s*location = \d+\)\s*(?:flat\s+)?(in|out)\s+(?:(?:highp|lowp|mediump)\s+)?(vec[234]|float) (\w+);", interface, code)
    code = code.replace("invariant gl_Position;", "")
    code = code.replace("gl_Position", "test_Position").replace("gl_ClipDistance", "test_Clip").replace("gl_CullDistance", "test_Cull")
    code = code.replace("gl_ViewIndex", "int(gl_GlobalInvocationID.x & 1u)")
    declarations = "vec4 test_Position; float test_Clip[3]; float test_Cull[2];\n"
    code = code.replace("void main() {", declarations + "void vertexMain() {", 1)
    original_zw = "v_zw" in code
    original_secondary = "v_color1" in code
    if not original_zw:
        code += "vec2 v_zw;\n"
    if not original_secondary:
        code += "vec3 v_color1;\n"
    # Don't compare varyings unused by the original interface.
    zw_read = "v_zw" if original_zw else "vec2(0.0)"
    secondary_read = "v_color1" if original_secondary else "vec3(0.0)"
    code += r'''
layout(local_size_x=8,local_size_y=8) in;
layout(std430,set=0,binding=3) buffer Output { vec4 pixels[]; } result;
void main() {
 uint x=gl_GlobalInvocationID.x, y=gl_GlobalInvocationID.y;
 float a=float(x)/31.0, b=float(y)/31.0;
 test_Clip[0]=1.0; test_Clip[1]=1.0; test_Clip[2]=1.0;
 test_Cull[0]=1.0; test_Cull[1]=1.0;
 v_color1=vec3(0.0); v_zw=vec2(0.0);
'''
    values = {
        "position": {"vec3": "vec3((a-0.5)*4.0,(b-0.5)*4.0,a+b-1.0)", "vec4": "vec4(a*5000.0,b*5000.0,a*65535.0, b<0.1 ? -2.0 : 1.0+a)"},
        "normal": {"vec3": "x%7u==0u ? vec3(0.0) : vec3(a-0.5,b-0.5,0.75)"},
        "texcoord": {"vec2": "vec2(a,b)", "vec3": "vec3(a,b,0.5+a)"},
        "fog": {"float": "a-b"}, "color0": {"vec4": "vec4(a,b,0.5,0.75)"},
        "color1": {"vec3": "vec3(b*0.1,a*0.2,0.01)"}}
    for typename, name in inputs:
        code += " " + name + "=" + values[name][typename] + ";\n"
    code += r'''
 vertexMain();
 vec4 value;
 switch (x%8u) {
 case 0u: value=test_Position; break;
 case 1u: value=v_color0; break;
 case 2u: value=vec4(SECONDARY,0.0); break;
 case 3u: value=vec4(v_texcoord,0.0); break;
 case 4u: value=vec4(v_fogdepth,ZW,0.0); break;
 case 5u: value=vec4(test_Clip[0],test_Clip[1],test_Clip[2],0.0); break;
 default: value=vec4(test_Cull[0],test_Cull[1],0.0,0.0); break;
 }
 result.pixels[y*32u+x]=value;
}
'''.replace("SECONDARY", secondary_read).replace("ZW", zw_read)
    return code


def main(args):
    run = bank.run
    if args.work_dir:
        Path(args.work_dir).mkdir(parents=True, exist_ok=True)
    with (contextlib.nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="psp-vertex-compare-")) as folder:
        tmp = Path(folder)
        (tmp / "dump.cpp").write_text(dumper())
        desc = (ROOT / "Common/GPU/Shader.cpp").read_text()
        (tmp / "ShaderLanguage.cpp").write_text("#include <cstring>\n" + desc)
        run(args.cxx, "-std=c++17", "-O2", "-ffunction-sections", "-fdata-sections", "-DNDEBUG",
            "-I" + str(ROOT), "-I" + str(ROOT / "Common"), "-I" + str(ROOT / "GPU"),
            "-I" + str(ROOT / "libretro/libretro-common/include"), tmp / "dump.cpp", tmp / "ShaderLanguage.cpp",
            ROOT / "Common/GPU/ShaderWriter.cpp", ROOT / "Common/StringUtils.cpp", "-Wl,--gc-sections", "-o", tmp / "dump")
        run(tmp / "dump", tmp)
        sources = sorted(tmp.glob("case_*.vert"))
        # Match the new superset outputs against only those used by the reference.
        for ref in tmp.glob("case_*_ref.vert"):
            new = ref.with_name(ref.name.replace("_ref", "_new"))
            reference = ref.read_text()
            candidate = new.read_text()
            if "v_zw" not in reference:
                candidate = candidate.replace("layout(location = 4) out highp vec2 v_zw;", "vec2 unused_zw;").replace("v_zw", "unused_zw")
            if "v_color1" not in reference:
                candidate = re.sub(r"layout\(location = 2\).*?out lowp vec3 v_color1;", "vec3 unused_color1;", candidate).replace("v_color1", "unused_color1")
            new.write_text(candidate)

        def compile_case(source):
            compiled = source.with_suffix(".comp")
            compiled.write_text(kernel(source.read_text()))
            run(args.glslc, "--target-env=vulkan1.0", "-O0", compiled, "-o", source.with_suffix(".spv"))
            run(args.spirv_val, "--target-env", "vulkan1.0", source.with_suffix(".spv"))

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(compile_case, sources))
        # Reuse the bounded offscreen executor; unused image bindings are valid.
        executor = (ROOT / "libretro/test/vkdepaltest.cpp").read_text()
        executor = executor.replace("MakeBuffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)", "MakeBuffer(944, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)")
        executor = executor.replace("bi0{ubo.handle, 0, 64}", "bi0{ubo.handle, 0, 944}")
        executor = executor.replace("memcpy(ubo.mapped, uniforms, sizeof(uniforms));", r'''
        float *u = (float *)ubo.mapped;
        memset(u, 0, 944);
        auto setUniform = [&](int offset, std::initializer_list<float> values) { std::copy(values.begin(), values.end(), u + offset); };
        setUniform(0, {1.1f,0.1f,0.0f,0.01f, 0.0f,0.9f,0.1f,0.0f, 0.1f,0.0f,0.8f,0.02f, 0.0f,0.1f,0.3f,1.0f});
        setUniform(16, {0.8f,0.1f,0.0f,0.2f, 0.0f,1.1f,0.2f,0.1f, 0.1f,0.0f,0.9f,0.4f});
        setUniform(28, {1.1f,0.2f,0.0f,0.1f, 0.0f,1.0f,0.1f,0.2f, 0.1f,0.0f,1.2f,0.3f});
        std::copy(u+28,u+40,u+40);
        setUniform(52, {0.5f,0.25f,1.0f/240.0f,1.0f/136.0f});
        setUniform(56, {120.0f,-68.0f,32767.0f,NAN});
        setUniform(60, {240.0f,136.0f,32767.0f,0.0f});
        setUniform(64, {0.25f,0.5f,2000.0f,61000.0f});
        setUniform(68, {0.75f,0.5f,0.125f,0.25f});
        setUniform(72, {0.2f,0.3f,0.4f,0.75f});
        setUniform(84, {0.1f,0.8f}); u[107]=1.0f;
        setUniform(108, {0.4f,0.3f,0.2f,0.9f}); setUniform(112, {0.8f,0.7f,0.6f});
        setUniform(116, {0.6f,0.5f,0.4f,4.0f}); setUniform(120, {0.1f,0.02f,0.03f});
        uint32_t control=0x007a5a5fu; memcpy(u+123,&control,4);
        for(int i=0;i<4;++i) {
            float f=float(i)+1.0f;
            setUniform(124+4*i, {0.2f*f,0.3f,0.8f/f}); setUniform(140+4*i, {0.0f,0.0f,1.0f});
            setUniform(156+4*i, {0.1f,0.2f,0.3f}); setUniform(172+4*i, {-0.25f,0.0f,0.0f,0.0f});
            setUniform(188+4*i, {0.02f*f,0.01f,0.02f}); setUniform(204+4*i, {0.05f,0.02f*f,0.03f});
            setUniform(220+4*i, {0.03f,0.04f,0.02f*f});
        }
''')
        executor = executor.replace('if (!std::isfinite(ref[j]) || !std::isfinite(test[j])) {',
            'if (std::isnan(ref[j]) && std::isnan(test[j])) continue;\n'
            'if (ref[j] == test[j]) continue;\n'
            'if (!std::isfinite(ref[j]) || !std::isfinite(test[j])) {')
        executor = executor.replace("error > 2e-6f", "error > 2e-6f")
        executor = executor.replace('printf("FAIL case %d max error %.9f\\n", i, error);', '''
                size_t worst = 0;
                for (size_t j = 0; j < ref.size(); ++j)
                    if (std::fabs(ref[j]-test[j]) > std::fabs(ref[worst]-test[worst])) worst=j;
                printf("FAIL case %d max error %.9f at %zu (x=%zu y=%zu c=%zu): %.9g != %.9g\\n", i, error,
                    worst, (worst/4)%32, worst/128, worst%4, ref[worst], test[worst]);''')
        (tmp / "compare.cpp").write_text(executor)
        run(args.cxx, "-std=c++17", "-O2", tmp / "compare.cpp", "-lvulkan", "-o", tmp / "compare")
        run(tmp / "compare", tmp, len(sources) // 2, 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--glslc", default="glslc")
    parser.add_argument("--spirv-val", default="spirv-val")
    parser.add_argument("--work-dir", help="Keep generated cases for diagnosis")
    main(parser.parse_args())
