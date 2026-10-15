#!/usr/bin/env python3
"""Regenerate Vulkan's fixed shader bank using the existing C++ generators.

Requires a host g++/clang++, glslc and spirv-val. These tools are only needed
for regeneration, never to build or run a core using the checked-in bank.
Game shaders and custom texture shaders are not covered by this bank.
Shipped texture-scaling shaders are compiled from defaultshaders.ini.
"""
import argparse
import configparser
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "Common/GPU/Vulkan/BuiltinShaders.inl"


def between(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError("Shader generator layout changed; update this tool")
    return text[text.index(start):text.index(end)]


def dump_source():
    thin3d = (ROOT / "Common/GPU/thin3d.cpp").read_text(encoding="utf-8")
    presets = between(thin3d, "static const InputDef g_inputs[]", "bool DrawContext::CreatePresets")
    presets = presets.replace("static ShaderModule *GenerateVShader(DrawContext *draw,", "static void GenerateVShader(")
    presets = presets.replace("static ShaderModule *GenerateFShader(DrawContext *draw,", "static void GenerateFShader(")
    presets = presets.replace("const ShaderLanguageDesc &shaderLanguageDesc = draw->GetShaderLanguageDesc();", "ShaderLanguageDesc shaderLanguageDesc(GLSL_VULKAN);")
    presets, vs_count = re.subn(r"return draw->CreateShaderModule\(ShaderStage::Vertex,[^\n]*", 'Emit("preset_" + std::to_string(preset) + "_" + std::to_string(tintSupported) + ".vert", code);', presets)
    presets, fs_count = re.subn(r"return draw->CreateShaderModule\(ShaderStage::Fragment,[^\n]*", 'Emit("preset_" + std::to_string(preset) + ".frag", code);', presets)
    if vs_count != 1 or fs_count != 1 or "draw->" in presets:
        raise RuntimeError("Preset generator interface changed; update this tool")
    draw2d = (ROOT / "GPU/Common/Draw2D.cpp").read_text(encoding="utf-8")
    draw2d = between(draw2d, "static const InputDef inputs[2]", "template <typename T>")
    depal = (ROOT / "GPU/Common/DepalettizeShaderCommon.cpp").read_text(encoding="utf-8")
    depal = between(depal, "static const InputDef vsInputs[2]", "// Uses integer instructions available")
    uniforms = between(draw2d, "const UniformDef g_draw2Duniforms[2]", "struct Draw2DUB")
    return r'''#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include "Common/GPU/ShaderWriter.h"
#include "GPU/Common/Draw2D.h"
#include "GPU/Common/DepalettizeShaderCommon.h"
using namespace Draw;
static std::string directory;
static void Emit(const std::string &name, const char *code) {
 FILE *f = fopen((directory + "/" + name).c_str(), "wb");
 if (!f) std::abort();
 if (fputs(code, f) < 0 || fclose(f) != 0) std::abort();
}
bool HandleAssert(bool, const char *, const char *, int, const char *, const char *, ...) { std::abort(); }
namespace Presets {
''' + presets + "}\nnamespace Draw2DBuiltins {\n" + draw2d + "}\n" + uniforms + "namespace DepalBuiltins {\n" + depal + r'''}
int main(int argc, char **argv) {
 if (argc != 2) return 2;
 directory = argv[1];
 for (int i = 0; i < VS_MAX_PRESET; ++i)
  for (bool tint : {false, true}) Presets::GenerateVShader((VertexShaderPreset)i, tint);
 for (int i = 0; i < FS_MAX_PRESET; ++i) Presets::GenerateFShader((FragmentShaderPreset)i);
 ShaderLanguageDesc language(GLSL_VULKAN);
 char code[16384];
 {
  ShaderWriter writer(code, language, ShaderStage::Vertex);
  Draw2DBuiltins::GenerateDraw2DVS(writer);
  Emit("draw2d.vert", code);
 }
 int index = 0;
 for (auto generate : {Draw2DBuiltins::GenerateDraw2DCopyColorFs,
    Draw2DBuiltins::GenerateDraw2DCopyColorRect2LinFs,
    Draw2DBuiltins::GenerateDraw2DCopyDepthFs,
    Draw2DBuiltins::GenerateDraw2DEncodeDepthFs,
    Draw2DBuiltins::GenerateDraw2D565ToDepthFs,
    Draw2DBuiltins::GenerateDraw2D565ToDepthDeswizzleFs}) {
  for (int stereo = 0; stereo < 2; ++stereo) {
   ShaderWriter writer(code, language, ShaderStage::Fragment, Slice<const char *>::empty(),
      stereo ? ShaderWriterFlags::FS_AUTO_STEREO : ShaderWriterFlags::NONE);
   generate(writer);
   Emit("draw2d_" + std::to_string(index) + "_" + std::to_string(stereo) + ".frag", code);
  }
  ++index;
 }
 for (int stereo = 0; stereo < 2; ++stereo) {
  DepalConfig config{};
  ShaderWriter writer(code, language, ShaderStage::Fragment, Slice<const char *>::empty(),
      stereo ? ShaderWriterFlags::FS_AUTO_STEREO : ShaderWriterFlags::NONE);
  DepalBuiltins::GenerateVulkanDepalFs(writer, config);
  Emit("depal_" + std::to_string(stereo) + ".frag", code);
 }
}
'''


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def texture_sources():
    directory = ROOT / "assets/shaders"
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read(directory / "defaultshaders.ini", encoding="utf-8")
    source = (ROOT / "GPU/Vulkan/TextureCacheVulkan.cpp").read_text(encoding="utf-8")
    wrapper = between(source, 'const char *uploadShader = R"(', ')";\n\nstatic int VkFormatBytesPerPixel')
    wrapper = wrapper[len('const char *uploadShader = R"('):]
    if wrapper.count("%s") != 1:
        raise RuntimeError("Texture upload shader wrapper changed")
    for section in config.values():
        if section.get("Type", "").lower() != "texture":
            continue
        files = [section[key] for key in ("Compute", "Compute2", "Compute3", "Compute4") if key in section]
        for filename in files:
            path = directory / filename
            if path.resolve().parent != directory.resolve():
                raise RuntimeError("Texture shader must be in assets/shaders")
            shader = path.read_text(encoding="utf-8")
            yield wrapper.replace("%s", shader) if len(files) == 1 else shader


SPEC_DEFAULT = re.compile(r"layout\(constant_id = (\d+)\) const uint psp_spec_(\d+) = (\d+)u;\n")


def canonical_source(source):
    seen = set()
    def zero(match):
        spec_id, name_id, value = map(int, match.groups())
        if spec_id != name_id or spec_id in seen or value > 0xffffffff:
            raise RuntimeError("Invalid specialization declaration")
        seen.add(spec_id)
        return "layout(constant_id = %d) const uint psp_spec_%d = 0u;\n" % (spec_id, spec_id)
    return SPEC_DEFAULT.sub(zero, source)


def specialization_words(words):
    # OpDecorate SpecId connects the API constant ID to the SPIR-V result ID.
    ids, constants, types = {}, {}, {}
    pos = 5
    while pos < len(words):
        count, opcode = words[pos] >> 16, words[pos] & 0xffff
        if not count or pos + count > len(words):
            raise RuntimeError("Malformed generated SPIR-V")
        if opcode == 71 and count == 4 and words[pos+2] == 1:
            ids[words[pos+1]] = words[pos+3]
        elif opcode == 21 and count == 4:
            types[words[pos+1]] = (words[pos+2], words[pos+3])
        elif opcode == 50 and count == 4:
            constants[words[pos+2]] = (words[pos+1], pos+3)
        pos += count
    result = []
    for result_id, spec_id in ids.items():
        type_id, offset = constants[result_id]
        if types.get(type_id) != (32, 0):
            raise RuntimeError("Only uint32 specialization defaults are supported")
        result.append((spec_id, offset))
    return sorted(result)


def generate(args):
    with tempfile.TemporaryDirectory(prefix="psp-vulkan-shaders-") as folder:
        tmp = Path(folder)
        (tmp / "dump.cpp").write_text(dump_source(), encoding="utf-8")
        # Shader language descriptions are shared with the runtime, but the
        # compiler resource-limit function is not needed by this host tool.
        desc = (ROOT / "Common/GPU/Shader.cpp").read_text(encoding="utf-8")
        desc = desc[:desc.index("void InitShaderResources")]
        desc = desc.replace('#include "ext/glslang/SPIRV/GlslangToSpv.h"', "")
        (tmp / "ShaderLanguage.cpp").write_text("#include <cstring>\n" + desc, encoding="utf-8")
        exe = tmp / ("dump.exe" if os.name == "nt" else "dump")
        run(args.cxx, "-std=c++17", "-O2", "-ffunction-sections", "-fdata-sections",
            "-DNDEBUG", "-I" + str(ROOT), "-I" + str(ROOT / "Common"),
            "-I" + str(ROOT / "GPU"), "-I" + str(ROOT / "libretro/libretro-common/include"),
            tmp / "dump.cpp", tmp / "ShaderLanguage.cpp",
            ROOT / "Common/GPU/ShaderWriter.cpp", "-Wl,--gc-sections", "-o", exe)
        run(exe, tmp)
        for i, shader in enumerate(texture_sources()):
            (tmp / ("texture_%02d.comp" % i)).write_text(shader, encoding="utf-8")
        entries = {}
        for source in sorted(tmp.glob("*.vert")) + sorted(tmp.glob("*.frag")) + sorted(tmp.glob("*.comp")):
            stage = {".vert": 1, ".frag": 16, ".comp": 32}[source.suffix]  # Vulkan stage bits.
            text = canonical_source(source.read_text(encoding="utf-8"))
            source.write_text(text, encoding="utf-8")
            key = stage, text
            if key in entries:
                continue
            binary = tmp / "shader.spv"
            run(args.glslc, "--target-env=vulkan1.0", "-O", source, "-o", binary)
            run(args.spirv_val, "--target-env", "vulkan1.0", binary)
            data = binary.read_bytes()
            words = struct.unpack("<%dI" % (len(data) // 4), data)
            entries[key] = (words, specialization_words(words))
        lines = ["// Generated by Tools/BuildVulkanShaderBank.py. Do not edit.",
                 "// Sources retain their original license notices embedded below.", ""]
        for i, ((stage, source), (words, specs)) in enumerate(entries.items()):
            lines.append("static const uint32_t builtin_%d[] = {" % i)
            for j in range(0, len(words), 8):
                lines.append("\t" + ", ".join("0x%08x" % w for w in words[j:j+8]) + ",")
            lines.append("};")
            if specs:
                lines.append("static const BuiltinSpecialization builtin_specs_%d[] = {" % i)
                lines.extend("\t{ %d, %d }," % pair for pair in specs)
                lines.append("};")
        lines.append("static const BuiltinShader builtinShaders[] = {")
        for i, ((stage, source), (words, specs)) in enumerate(entries.items()):
            # Keep individual literals small enough for MSVC as well as GCC.
            literal = "\n".join("\t" + json.dumps(source[k:k+2048], ensure_ascii=True)
                                for k in range(0, len(source), 2048))
            spec_array = "builtin_specs_%d" % i if specs else "nullptr"
            lines.append("{ %d,\n%s,\n builtin_%d, %d, %s, %d }," % (stage, literal, i, len(words), spec_array, len(specs)))
        lines.append("};\n")
        result = "\n".join(lines).encode()
        if args.check:
            if not OUTPUT.exists() or OUTPUT.read_bytes().replace(b"\r\n", b"\n") != result:
                raise SystemExit("Shader bank differs; regenerate it with the same toolchain")
        else:
            OUTPUT.write_bytes(result)
        print("Validated %d distinct precompiled Vulkan shaders" % len(entries))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--glslc", default="glslc")
    parser.add_argument("--spirv-val", default="spirv-val")
    parser.add_argument("--check", action="store_true")
    generate(parser.parse_args())
