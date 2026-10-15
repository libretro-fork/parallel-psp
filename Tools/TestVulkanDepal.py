#!/usr/bin/env python3
"""Compare legacy and precompiled Vulkan palette equations on an offscreen device.

Requires a host g++/clang++, glslc, spirv-val and Vulkan headers/loader.
Set VK_ICD_FILENAMES to the lavapipe ICD for a CPU-only, windowless check.
The exact fragment shader bodies are wrapped in compute dispatches using only
mip level zero; no derivatives or other fragment-only operations are involved.
"""
import argparse
import concurrent.futures
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("vulkan_shader_bank", ROOT / "Tools/BuildVulkanShaderBank.py")
bank = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bank)

DUMP_MAIN = r'''
int main(int argc, char **argv) {
	if (argc != 2)
		return 2;
	directory = argv[1];
	FILE *manifest = fopen((directory + "/cases.tsv").c_str(), "wb");
	int number = 0;
	auto dump = [&](DepalConfig config) {
		char code[32768];
		ShaderLanguageDesc language(GLSL_VULKAN);
		{
			ShaderWriter writer(code, language, ShaderStage::Fragment);
			Reference::GenerateDepalFs(writer, config);
			Emit("case_" + std::to_string(number) + "_ref.frag", code);
		}
		{
			ShaderWriter writer(code, language, ShaderStage::Fragment);
			DepalBuiltins::GenerateVulkanDepalFs(writer, config);
			Emit("case_" + std::to_string(number) + "_new.frag", code);
		}
		fprintf(manifest, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", number, (int)config.bufferFormat,
				(int)config.textureFormat, config.shift, config.mask, config.startPos, config.smoothedDepal,
				config.depthUpperBits);
		++number;
	};
	const int shifts[] = {0, 1, 4, 5, 8, 10, 11, 15, 16, 24, 31};
	const int masks[] = {0, 1, 15, 31, 63, 255};
	for (int fmt = 0; fmt < 6; ++fmt)
		for (int i = 0; i < 22; ++i) {
			DepalConfig c{};
			c.bufferFormat = (GEBufferFormat)fmt;
			c.textureFormat = GE_TFMT_CLUT32;
			c.shift = shifts[i % 11];
			c.mask = masks[(i + fmt) % 6];
			c.startPos = (i % 3) * 240;
			c.depthUpperBits = i % 2 ? 2 : 6;
			dump(c);
		}
	for (int i = 0; i < 22; ++i) {
		DepalConfig c{};
		c.bufferFormat = GE_FORMAT_5551;
		c.textureFormat = GE_TFMT_CLUT8;
		c.shift = shifts[i % 11];
		c.mask = masks[(i + 1) % 6];
		c.startPos = (i % 3) * 240;
		dump(c);
	}
	for (int fmt = 0; fmt < 2; ++fmt)
		for (int i = 0; i < 3; ++i) {
			DepalConfig c{};
			c.bufferFormat = (GEBufferFormat)fmt;
			c.smoothedDepal = true;
			c.shift = i == 0 ? 0 : (i == 1 ? 5 : (fmt == 0 ? 11 : 10));
			c.mask = fmt == 0 && i == 1 ? 63 : 31;
			dump(c);
		}
	for (int i = 0; i < 2; ++i) {
		DepalConfig c{};
		c.bufferFormat = GE_FORMAT_DEPTH16;
		c.textureFormat = GE_TFMT_5650;
		c.depthUpperBits = i == 0 ? 2 : 6;
		dump(c);
	}
	fclose(manifest);
	printf("%d palette cases\n", number);
}
'''


def reference_dump():
    source = bank.dump_source()
    source = source[:source.index("int main(int argc")]
    legacy = (ROOT / "GPU/Common/DepalettizeShaderCommon.cpp").read_text(encoding="utf-8")
    legacy = legacy[legacy.index("static const InputDef vsInputs[2]"):]
    start = legacy.index("// A single Vulkan program")
    end = legacy.index("// Uses integer instructions available", start)
    legacy = legacy[:start] + legacy[end:]
    route = """\tif (writer.Lang().shaderLanguage == GLSL_VULKAN) {
\t\tGenerateVulkanDepalFs(writer, config);
\t\treturn;
\t}
"""
    if legacy.count(route) != 1:
        raise RuntimeError("Palette generator interface changed; update the reference wrapper")
    legacy = legacy.replace(route, "")
    return source + '#include "Common/StringUtils.h"\n#undef ERROR_LOG_ONCE\n#define ERROR_LOG_ONCE(...) ((void)0)\nnamespace Reference {\n' + legacy + "\n}\n" + DUMP_MAIN


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def main(args):
    with tempfile.TemporaryDirectory(prefix="psp-vulkan-depal-test-") as name:
        tmp = Path(name)
        (tmp / "dump.cpp").write_text(reference_dump(), encoding="utf-8")
        desc = (ROOT / "Common/GPU/Shader.cpp").read_text(encoding="utf-8")
        desc = desc[:desc.index("void InitShaderResources")].replace('#include "ext/glslang/SPIRV/GlslangToSpv.h"', "")
        (tmp / "ShaderLanguage.cpp").write_text("#include <cstring>\n" + desc, encoding="utf-8")
        run(args.cxx, "-std=c++17", "-O2", "-ffunction-sections", "-fdata-sections", "-DNDEBUG",
            "-I" + str(ROOT), "-I" + str(ROOT / "Common"), "-I" + str(ROOT / "GPU"),
            "-I" + str(ROOT / "libretro/libretro-common/include"), tmp / "dump.cpp",
            tmp / "ShaderLanguage.cpp", ROOT / "Common/GPU/ShaderWriter.cpp",
            ROOT / "Common/StringUtils.cpp", "-Wl,--gc-sections", "-o", tmp / "dump")
        run(tmp / "dump", tmp)

        def compile_kernel(source):
            code = source.read_text(encoding="utf-8")
            code, inputs = re.subn(r"layout\(location = 0\) highp in vec2 v_texcoord;[^\n]*", "vec2 v_texcoord;", code)
            code, outputs = re.subn(r"layout\(location = 0, index = 0\) out vec4 fragColor0;", "vec4 fragColor0;", code)
            if inputs != 1 or outputs != 1:
                raise RuntimeError("Fragment interface changed; update the compute wrapper")
            code = code.replace("void main() {", "void fragmentMain() {", 1)
            code += """
layout(local_size_x=8,local_size_y=8) in;
layout(std430,set=0,binding=3) buffer Output { vec4 pixels[]; } result;
void main() {
 uvec2 xy=gl_GlobalInvocationID.xy;
 v_texcoord=(vec2(xy)+vec2(0.5))/32.0;
 fragmentMain();
 result.pixels[xy.y*32u+xy.x]=fragColor0;
}
"""
            kernel = source.with_suffix(".comp")
            kernel.write_text(code, encoding="utf-8")
            run(args.glslc, "--target-env=vulkan1.0", "-O", kernel, "-o", source.with_suffix(".spv"))
            run(args.spirv_val, "--target-env", "vulkan1.0", source.with_suffix(".spv"))

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(compile_kernel, sorted(tmp.glob("case_*.frag"))))
        run(args.cxx, "-std=c++17", "-O2", ROOT / "libretro/test/vkdepaltest.cpp", "-lvulkan", "-o", tmp / "compare")
        count = len((tmp / "cases.tsv").read_text().splitlines())
        for linear in (0, 1):
            run(tmp / "compare", tmp, count, linear)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--glslc", default="glslc")
    parser.add_argument("--spirv-val", default="spirv-val")
    main(parser.parse_args())
