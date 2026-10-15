#!/usr/bin/env python3
"""Regenerate Vulkan's fixed shader bank using the existing C++ generators.

Requires a host g++/clang++, glslc and spirv-val. These tools are only needed
for regeneration, never to build or run a core using the checked-in bank.
Game shaders and custom texture shaders are not covered by this bank.
"""
import argparse
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
    return r'''#include <cstdio>
#include <cstdlib>
#include <string>
#include "Common/GPU/ShaderWriter.h"
#include "GPU/Common/Draw2D.h"
using namespace Draw;
static std::string directory;
static void Emit(const std::string &name, const char *code) {
 FILE *f = fopen((directory + "/" + name).c_str(), "wb");
 if (!f) std::abort();
 if (fputs(code, f) < 0 || fclose(f) != 0) std::abort();
}
bool HandleAssert(bool, const char *, const char *, int, const char *, const char *, ...) { std::abort(); }
namespace Presets {
''' + presets + "}\nnamespace Draw2DBuiltins {\n" + draw2d + r'''}
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
  ShaderWriter writer(code, language, ShaderStage::Fragment);
  generate(writer);
  Emit("draw2d_" + std::to_string(index++) + ".frag", code);
 }
}
'''


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


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
        entries = {}
        for source in sorted(tmp.glob("*.vert")) + sorted(tmp.glob("*.frag")):
            stage = 1 if source.suffix == ".vert" else 16  # Vulkan stage bits.
            text = source.read_text(encoding="utf-8")
            key = stage, text
            if key in entries:
                continue
            binary = tmp / "shader.spv"
            run(args.glslc, "--target-env=vulkan1.0", "-O", source, "-o", binary)
            run(args.spirv_val, "--target-env", "vulkan1.0", binary)
            data = binary.read_bytes()
            words = struct.unpack("<%dI" % (len(data) // 4), data)
            entries[key] = words
        lines = ["// Generated by Tools/BuildVulkanShaderBank.py. Do not edit.",
                 "// Shader sources retain the PPSSPP GPL-2.0-or-later license.", ""]
        for i, ((stage, source), words) in enumerate(entries.items()):
            lines.append("static const uint32_t builtin_%d[] = {" % i)
            for j in range(0, len(words), 8):
                lines.append("\t" + ", ".join("0x%08x" % w for w in words[j:j+8]) + ",")
            lines.append("};")
        lines.append("static const BuiltinShader builtinShaders[] = {")
        for i, ((stage, source), words) in enumerate(entries.items()):
            lines.append("{ %d, %s, builtin_%d, %d }," % (stage, json.dumps(source, ensure_ascii=True), i, len(words)))
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
