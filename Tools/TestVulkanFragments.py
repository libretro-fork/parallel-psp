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
"""Compare legacy and bank fragment equations through offscreen Vulkan compute.

Requires the same tools as TestVulkanDepal.py. Covers 2D texture sampling,
palette modes, blending, tests, logic, masks and depth calculations. Fragment
derivatives, actual rasterization, 3D textures and multiview need frontend tests.
"""
import argparse
import concurrent.futures
import importlib.util
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("bank", ROOT / "Tools/BuildVulkanShaderBank.py")
bank = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bank)


def dumper():
    prefix = bank.dump_source().split("int main(int argc", 1)[0]
    legacy = (ROOT / "GPU/Common/FragmentShaderGenerator.cpp").read_text()
    legacy = legacy[legacy.index("static const SamplerDef samplersMono"):]
    legacy = legacy.replace("gstate_c.Use(", "Use(").replace("g_Config.bVendorBugChecksEnabled", "true")
    legacy = legacy.replace("id.Description()", 'std::string("reference")')
    legacy = re.sub(r"gl_extensions\.[A-Za-z_0-9]+", "0", legacy)
    return '#include "Common/GPU/thin3d.h"\n#include "GPU/Common/GPUStateUtils.h"\n' + prefix + r'''
#include "Common/StringUtils.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "GPU/Common/ShaderId.h"
#include "GPU/Common/FragmentShaderGenerator.h"
#include "GPU/Directx9/ShaderConstantsDX9.h"
#define WRITE(p, ...) p.F(__VA_ARGS__)
static uint32_t useFlags;
static bool Use(uint32_t f) { return (useFlags & f) != 0; }
enum { DRAW_BINDING_TEXTURE=0, DRAW_BINDING_2ND_TEXTURE=1, DRAW_BINDING_DEPAL_TEXTURE=2, DRAW_BINDING_DYNUBO_BASE=3 };
namespace Reference {
''' + legacy + r'''}
int main(int argc, char **argv) {
 if (argc != 2) return 2;
 directory=argv[1];
 uint64_t random=0xabcdef123456789ULL;
 auto next=[&]() { random^=random<<13; random^=random>>7; random^=random<<17; return random; };
 int count=0;
 for (int n=0;n<512;++n) {
  FShaderID id; id.FromUint64(next()&((1ULL<<62)-1));
  id.SetBit(FS_BIT_CLEARMODE,n%61==0);
  id.SetBit(FS_BIT_3D_TEXTURE,false); id.SetBit(FS_BIT_SAMPLE_ARRAY_TEXTURE,false);
  id.SetBit(FS_BIT_STEREO,false); id.SetBit(FS_BIT_USE_FRAMEBUFFER_FETCH,false);
  id.SetBits(FS_BIT_SIMULATE_LOGIC_OP_TYPE,2,n%3);
  id.SetBits(FS_BIT_STENCIL_TO_ALPHA,2,(n/3)%3);
  id.SetBits(FS_BIT_BLENDEQ,3,n%6);
  uint32_t blend=id.Bits(FS_BIT_REPLACE_BLEND,3);
  if (blend==2 || blend==3 || blend==7) {
   const int factors[]={2,3,6,7,8,10}; id.SetBits(FS_BIT_BLENDFUNC_A,4,factors[n%6]);
  }
  if (!id.Bit(FS_BIT_DO_TEXTURE)) id.SetBits(FS_BIT_SHADER_DEPAL_MODE,2,0);
  id.SetBits(FS_BIT_SHADER_DEPAL_FORMAT,3,n%6);
  if (n<192) {
   id.SetBit(FS_BIT_ALPHA_TEST,false); id.SetBit(FS_BIT_COLOR_TEST,false); id.SetBit(FS_BIT_MINMAX_DISCARD,false);
  }
  useFlags=n&1 ? GPU_ROUND_FRAGMENT_DEPTH_TO_16BIT : 0;
  char ref[32768],code[32768]; uint64_t uniforms; FragmentShaderFlags flags; std::string error;
  ShaderLanguageDesc language(GLSL_VULKAN); Draw::Bugs bugs;
  if (!Reference::GenerateFragmentShader(id,ref,language,bugs,&uniforms,&flags,&error,false)) continue;
  if (!Reference::GenerateFragmentShader(id,code,language,bugs,&uniforms,&flags,&error,true)) std::abort();
  Emit("case_"+std::to_string(count)+"_ref.frag",ref);
  Emit("case_"+std::to_string(count)+"_new.frag",code);
  ++count;
 }
 FShaderID bytes; bytes.FromUint64(2ULL | (3ULL<<4) | (3ULL<<49) | (3ULL<<53));
 for(int compiled=0;compiled<2;++compiled) {
  ShaderLanguageDesc language(GLSL_VULKAN); Draw::Bugs bugs;
  char code[32768];uint64_t uniforms;FragmentShaderFlags flags;std::string error;
  if (!Reference::GenerateFragmentShader(bytes,code,language,bugs,&uniforms,&flags,&error,compiled)) std::abort();
  Emit("case_"+std::to_string(count)+(compiled ? "_new.frag" : "_ref.frag"),code);
 }
 ++count;
 FShaderID bilinear; bilinear.FromUint64(2ULL | (3ULL<<4) | (3ULL<<49) | (1ULL<<53) | (5ULL<<55));
 for(int compiled=0;compiled<2;++compiled) {
  ShaderLanguageDesc language(GLSL_VULKAN); Draw::Bugs bugs;
  char code[32768];uint64_t uniforms;FragmentShaderFlags flags;std::string error;
  if (!Reference::GenerateFragmentShader(bilinear,code,language,bugs,&uniforms,&flags,&error,compiled)) std::abort();
  Emit(compiled ? "bilinear_new.frag" : "bilinear_ref.frag",code);
 }
 printf("%d fragment cases\n",count);
}
'''


def kernel(code):
    code=re.sub(r"layout\s*\(std140, set = 0, binding = 3\)","layout(std140,set=0,binding=0)",code)
    code=re.sub(r"layout\s*\(set = 0, binding = 0\)( uniform \w+ tex;)",r"layout(set=0,binding=1)\1",code)
    code=re.sub(r"layout\s*\(set = 0, binding = 1\)( uniform \w+ fbotex;)",r"layout(set=0,binding=4)\1",code)
    code=re.sub(r"layout\s*\(\s*location = \d+\)\s*(?:flat\s+)?in\s+(?:(?:highp|lowp|mediump)\s+)?(vec[234]|float) (\w+);",r"\1 \2;",code)
    code=re.sub(r"layout\s*\(\s*location = 0, index = [01]\) out vec4 (\w+);",r"vec4 \1;",code)
    code=re.sub(r"layout\s*\(depth_unchanged\) out float gl_FragDepth;","",code)
    code=code.replace("gl_FragCoord","test_Coord").replace("gl_FragDepth","test_Depth")
    declarations="highp vec4 test_Coord; highp float test_Depth; bool test_Discard;\n"
    code=code.replace("void main() {",declarations+"void fragmentMain() {",1)
    code=code.replace("#define DISCARD discard","#define DISCARD { test_Discard=true; return; }")
    code=code.replace("else discard;","else { test_Discard=true; return; }").replace(") discard;",") { test_Discard=true; return; }")
    names={name for name in ["v_color1","v_texcoord","v_fogdepth","v_zw","fragColor1"] if re.search(r"\b"+name+r"\s*;",code)}
    types={"v_color1":"vec3","v_texcoord":"vec3","v_fogdepth":"float","v_zw":"vec2","fragColor1":"vec4"}
    for name in types:
        if name not in names: code+=types[name]+" "+name+";\n"
    return code+r'''
layout(local_size_x=8,local_size_y=8) in;
layout(std430,set=0,binding=3) buffer Output { vec4 pixels[]; } result;
void main() {
 uvec2 xy=gl_GlobalInvocationID.xy;
 float a=float(xy.x)/31.0,b=float(xy.y)/31.0;
 test_Coord=vec4(vec2(xy)+vec2(0.5),a,1.0);
 test_Depth=0.0; test_Discard=false;
 v_color0=vec4(a,b,0.5,1.0-a); v_color1=vec3(b*0.1,a*0.2,0.05);
 v_texcoord=vec3((vec2(xy)+vec2(0.5))/32.0,0.5+a);
 v_fogdepth=a-b; v_zw=vec2(a*65535.0,1.0);
 fragColor0=vec4(0.0); fragColor1=vec4(0.0);
 fragmentMain();
 vec4 value=xy.y%3u==0u ? fragColor0 : xy.y%3u==1u ? fragColor1 : vec4(test_Depth);
 result.pixels[xy.y*32u+xy.x]=test_Discard ? vec4(-123.0) : value;
}
'''


def executor_source():
    code=(ROOT/"libretro/test/vkdepaltest.cpp").read_text()
    code=code.replace("MakeBuffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)","MakeBuffer(432, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)")
    code=code.replace("bi0{ubo.handle, 0, 64}","bi0{ubo.handle, 0, 432}")
    code=code.replace("memcpy(ubo.mapped, uniforms, sizeof(uniforms));",r'''
        float *u=(float *)ubo.mapped;
        memset(u,0,432);
        u[66]=2000.0f;u[67]=61000.0f;
        u[80]=linear ? 1.0f : 0.0f;u[81]=linear ? 2.0f : 1.0f;
        u[88]=0.1f;u[89]=0.2f;u[90]=0.3f;
        u[92]=0.3f;u[93]=0.4f;u[94]=0.5f;
        u[96]=1.0f;u[97]=1.0f;u[98]=0.5f/32.0f;u[99]=0.5f/32.0f;
        u[100]=0.2f;u[101]=0.3f;u[102]=0.4f;u[103]=0.25f;
        u[104]=0.6f;u[105]=0.5f;u[106]=0.4f;
        uint32_t depal=255u|(linear ? 0x80000000u : 0u),mask=0xf0f0f0f0u,ref=0x60504030u,write=0xff0ff0ffu;
        memcpy(u+77,&depal,4);memcpy(u+78,&write,4);memcpy(u+91,&ref,4);memcpy(u+95,&mask,4);
''')
    code=code.replace("Image source = MakeImage(32, 32, true)","Image source = MakeImage(32, 32, false)")
    code=code.replace("begin();\n\t\tImage *images[]",r'''
        VkImageView arrayView;
        VkImageViewCreateInfo extra{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        extra.image=source.handle;extra.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;extra.format=VK_FORMAT_R32G32B32A32_SFLOAT;
        extra.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VK(vkCreateImageView(device,&extra,nullptr,&arrayView));
        begin();
        Image *images[]''')
    code=code.replace("bindings[4]", "bindings[5]").replace("lc.bindingCount = 4", "lc.bindingCount = 5")
    code=code.replace("{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};",
        "{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},\n"
        "{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};")
    code=code.replace("VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2", "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3")
    code=code.replace("writes[4]", "writes[5]").replace("for (int i = 0; i < 4; i++) {\n\t\t\twrites", "for (int i = 0; i < 5; i++) {\n\t\t\twrites")
    code=code.replace("vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);",
        "VkDescriptorImageInfo ii4{sampler,arrayView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};\n"
        "writes[4].pImageInfo=&ii4;\n vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);")
    code=code.replace("vkDestroySampler(device, sampler, nullptr);", "vkDestroyImageView(device,arrayView,nullptr);\n vkDestroySampler(device, sampler, nullptr);")
    return code


def main(args):
    run=bank.run
    with tempfile.TemporaryDirectory(prefix="psp-fragment-compare-") as folder:
        tmp=Path(folder)
        (tmp/"dump.cpp").write_text(dumper())
        desc=(ROOT/"Common/GPU/Shader.cpp").read_text()
        (tmp/"ShaderLanguage.cpp").write_text("#include <cstring>\n"+desc)
        run(args.cxx,"-std=c++17","-O2","-ffunction-sections","-fdata-sections","-DNDEBUG",
            "-I"+str(ROOT),"-I"+str(ROOT/"Common"),"-I"+str(ROOT/"GPU"),"-I"+str(ROOT/"libretro/libretro-common/include"),
            tmp/"dump.cpp",tmp/"ShaderLanguage.cpp",ROOT/"Common/GPU/ShaderWriter.cpp",ROOT/"Common/StringUtils.cpp",
            "-Wl,--gc-sections","-o",tmp/"dump")
        run(tmp/"dump",tmp)
        sources=sorted(tmp.glob("case_*.frag"))
        def compile_case(source):
            compiled=source.with_suffix(".comp");compiled.write_text(kernel(source.read_text()))
            run(args.glslc,"--target-env=vulkan1.0","-O0",compiled,"-o",source.with_suffix(".spv"))
            run(args.spirv_val,"--target-env","vulkan1.0",source.with_suffix(".spv"))
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(compile_case,sources))
        (tmp/"compare.cpp").write_text(executor_source())
        run(args.cxx,"-std=c++17","-O2",tmp/"compare.cpp","-lvulkan","-o",tmp/"compare")
        for linear in (0,1): run(tmp/"compare",tmp,len(sources)//2,linear)
        # An independent byte oracle catches a shared generator mistake that
        # legacy/new comparisons alone cannot detect. Every byte must address
        # its own palette entry, including 1 and 255.
        oracle=tmp/"bytes";oracle.mkdir()
        last=len(sources)//2-1
        candidate=kernel((tmp/("case_%d_new.frag"%last)).read_text())
        candidate=candidate.replace("vec4 value=xy.y%3u==0u ? fragColor0 : xy.y%3u==1u ? fragColor1 : vec4(test_Depth);", "vec4 value=fragColor0;")
        (oracle/"case_0_new.comp").write_text(candidate)
        (oracle/"case_0_ref.comp").write_text(r'''#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(std430,set=0,binding=3) buffer Output { vec4 pixels[]; } result;
void main() {
 uvec2 xy=gl_GlobalInvocationID.xy;uint i=(xy.y*128u+xy.x)&255u;
 result.pixels[xy.y*32u+xy.x]=vec4(float(i&31u)/31.0,float(i>>5u)/15.0,float((i*37u)&255u)/255.0,1.0);
}
''')
        for mode in ("ref","new"):
            run(args.glslc,"--target-env=vulkan1.0","-O0",oracle/("case_0_"+mode+".comp"),"-o",oracle/("case_0_"+mode+".spv"))
        executor=executor_source().replace("pixels[i] = (random >> 16) / 65535.0f;", "pixels[i] = float(i & 255) / 255.0f;")
        executor=re.sub(r"\s*if \(i % 4 == 3 && \(i / 4\) % 5 == 0\)\s*pixels\[i\] = 1.0f;", "",executor)
        (oracle/"compare.cpp").write_text(executor)
        run(args.cxx,"-std=c++17","-O2",oracle/"compare.cpp","-lvulkan","-o",oracle/"compare")
        run(oracle/"compare",oracle,1,0)


        # A mapped first index can equal the three raw neighbor indices even
        # though their final palette entries differ: raw 4,2,2,2 shifted by 1
        # becomes 2,1,1,1. Compare both generators against actual bilinear math.
        oracle=tmp/"bilinear";oracle.mkdir()
        expected=r'''#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(std430,set=0,binding=3) buffer Output { vec4 pixels[]; } result;
void main() {
 uvec2 xy=gl_GlobalInvocationID.xy;vec2 f=vec2(xy)/31.0;
 vec4 first=vec4(2.0/31.0,0.0,74.0/255.0,1.0);
 vec4 neighbor=vec4(1.0/31.0,0.0,37.0/255.0,1.0);
 result.pixels[xy.y*32u+xy.x]=mix(first,neighbor,1.0-(1.0-f.x)*(1.0-f.y));
}
'''
        for index,mode in enumerate(("ref","new")):
            candidate=kernel((tmp/("bilinear_"+mode+".frag")).read_text())
            candidate=candidate.replace("v_texcoord=vec3((vec2(xy)+vec2(0.5))/32.0,0.5+a);",
                "v_texcoord=vec3((vec2(a,b)+vec2(0.5))/32.0,1.0);")
            candidate=candidate.replace("vec4 value=xy.y%3u==0u ? fragColor0 : xy.y%3u==1u ? fragColor1 : vec4(test_Depth);", "vec4 value=fragColor0;")
            for label,source in (("new",candidate),("ref",expected)):
                path=oracle/("case_%d_%s.comp"%(index,label));path.write_text(source)
                run(args.glslc,"--target-env=vulkan1.0","-O0",path,"-o",path.with_suffix(".spv"))
                run(args.spirv_val,"--target-env","vulkan1.0",path.with_suffix(".spv"))
        executor=executor_source().replace("pixels[i] = (random >> 16) / 65535.0f;", "pixels[i] = float(i / 4 == 0 ? 4 : 2) / 255.0f;")
        executor=re.sub(r"\s*if \(i % 4 == 3 && \(i / 4\) % 5 == 0\)\s*pixels\[i\] = 1.0f;", "",executor)
        executor=executor.replace("u[80]=linear ? 1.0f : 0.0f;u[81]=linear ? 2.0f : 1.0f;", "u[80]=0.0f;u[81]=1.0f;")
        executor=executor.replace("255u|(linear ? 0x80000000u : 0u)", "255u|0x100u|0x80000000u")
        (oracle/"compare.cpp").write_text(executor)
        run(args.cxx,"-std=c++17","-O2",oracle/"compare.cpp","-lvulkan","-o",oracle/"compare")
        run(oracle/"compare",oracle,2,0)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx",default="c++")
    parser.add_argument("--glslc",default="glslc")
    parser.add_argument("--spirv-val",default="spirv-val")
    main(parser.parse_args())
