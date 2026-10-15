#!/usr/bin/env python3
"""Regenerate Vulkan's fixed shader bank using the existing C++ generators.

Requires a host g++/clang++, glslc and spirv-val. These tools are only needed
for regeneration, never to build or run a core using the checked-in bank.
Game shaders and custom texture shaders are not covered by this bank.
Shipped texture-scaling shaders are compiled from defaultshaders.ini.
"""
import argparse
import configparser
import contextlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib

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
    stencil = (ROOT / "GPU/Common/StencilCommon.cpp").read_text(encoding="utf-8")
    stencil = between(stencil, "// TODO: Merge this with UniformBufferDesc", "bool FramebufferManagerCommon::PerformWriteStencilFromMemory")
    reinterpret = (ROOT / "GPU/Common/ReinterpretFramebuffer.cpp").read_text(encoding="utf-8")
    reinterpret = reinterpret[reinterpret.index("static const VaryingDef varyings[1]"):]
    uniforms = between(draw2d, "const UniformDef g_draw2Duniforms[2]", "struct Draw2DUB")
    base = (ROOT / "GPU/Common/ShaderUniforms.h").read_text(encoding="utf-8")
    base = between(base, "static const char * const ub_baseStr", "// 512 bytes.")
    light = (ROOT / "GPU/Common/ShaderUniforms.h").read_text(encoding="utf-8")
    light = between(light, "static const char * const ub_vs_lightsStr", "// useBufferedRendering")
    return r'''#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include "Common/GPU/ShaderWriter.h"
#include "GPU/Common/Draw2D.h"
#include "GPU/Common/DepalettizeShaderCommon.h"
#include "GPU/Vulkan/SoftwareVertexShader.h"
#include "GPU/Vulkan/HardwareVertexShader.h"
#include "GPU/Vulkan/GameFragmentShader.h"
using namespace Draw;
static std::string directory;
static void Emit(const std::string &name, const char *code) {
 FILE *f = fopen((directory + "/" + name).c_str(), "wb");
 if (!f) std::abort();
 if (fputs(code, f) < 0 || fclose(f) != 0) std::abort();
}
bool HandleAssert(bool, const char *, const char *, int, const char *, const char *, ...) { std::abort(); }
''' + base + light + r'''namespace Presets {
''' + presets + "}\nnamespace Draw2DBuiltins {\n" + draw2d + "}\n" + uniforms + "namespace DepalBuiltins {\n" + depal + "}\nnamespace StencilBuiltins {\n" + stencil + "}\nnamespace ReinterpretBuiltins {\n" + reinterpret + r'''}
int main(int argc, char **argv) {
 if (argc != 2) return 2;
 directory = argv[1];
 for (int i = 0; i < VS_MAX_PRESET; ++i)
  for (bool tint : {false, true}) Presets::GenerateVShader((VertexShaderPreset)i, tint);
 for (int i = 0; i < FS_MAX_PRESET; ++i) Presets::GenerateFShader((FragmentShaderPreset)i);
 ShaderLanguageDesc language(GLSL_VULKAN);
 char code[32768];
 StencilBuiltins::GenerateStencilVs(code, language);
 Emit("stencil.vert", code);
 for (int variant = 0; variant < 4; ++variant) {
  Bugs bugs;
  if (variant & 1) bugs.Infest(Bugs::NO_DEPTH_CANNOT_DISCARD_STENCIL_MALI);
  StencilBuiltins::GenerateStencilFs(code, language, bugs, variant & 2);
  Emit("stencil_" + std::to_string(variant) + ".frag", code);
 }
 for (int from = 0; from < 4; ++from)
 for (int to = 0; to < 4; ++to)
 for (int stereo = 0; stereo < 2; ++stereo) {
  if (from == to) continue;
  ShaderWriter writer(code, language, ShaderStage::Fragment, Slice<const char *>::empty(),
    stereo ? ShaderWriterFlags::FS_AUTO_STEREO : ShaderWriterFlags::NONE);
  ReinterpretBuiltins::GenerateReinterpretFragmentShader(writer, (GEBufferFormat)from, (GEBufferFormat)to);
  Emit("reinterpret_" + std::to_string(from) + "_" + std::to_string(to) + "_" + std::to_string(stereo) + ".frag", code);
 }
 for (int textureKind = 0; textureKind < 4; ++textureKind)
 for (int palette = 0; palette < 2; ++palette) {
  if (palette && (textureKind == 0 || textureKind == 2)) continue;
  for (int variant = 0; variant < 16; ++variant)
  for (int depth = 0; depth < 3; ++depth) {
   bool stereo = (variant & 4) != 0;
   ShaderWriter writer(code, language, ShaderStage::Fragment, Slice<const char *>::empty(),
     stereo ? ShaderWriterFlags::FS_AUTO_STEREO : ShaderWriterFlags::NONE);
   WriteGameVulkanFragmentShader(writer, ub_baseStr, textureKind, palette, variant & 1,
     variant & 2, stereo, variant & 8, depth, 0, 0, 0);
   Emit("game_" + std::to_string(textureKind) + "_" + std::to_string(palette) + "_" + std::to_string(variant) + "_" + std::to_string(depth) + ".frag", code);
  }
 }
 for (int variant = 0; variant < 16; ++variant) {
  std::vector<const char *> extensions;
  if (variant & 8) extensions.push_back("#extension GL_EXT_multiview : enable");
  ShaderWriter writer(code, language, ShaderStage::Vertex, extensions);
  WriteHardwareVulkanVertexShader(writer, ub_baseStr, ub_vs_lightsStr, variant & 1, variant & 2, variant & 4, variant & 8, 0, 0, 0, 0);
  Emit("hardware_" + std::to_string(variant) + ".vert", code);
 }
 for (int variant = 0; variant < 16; ++variant) {
  ShaderWriter writer(code, language, ShaderStage::Vertex);
  WriteSoftwareVulkanVertexShader(writer, ub_baseStr, variant & 1, variant & 2, variant & 4, variant & 8, 0);
  Emit("software_" + std::to_string(variant) + ".vert", code);
 }
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
    if args.work_dir:
        Path(args.work_dir).mkdir(parents=True, exist_ok=True)
    with (contextlib.nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="psp-vulkan-shaders-")) as folder:
        tmp = Path(folder)
        (tmp / "dump.cpp").write_text(dump_source(), encoding="utf-8")
        # Use the runtime's shader language descriptions in the host generator.
        desc = (ROOT / "Common/GPU/Shader.cpp").read_text(encoding="utf-8")
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
            # Keep game arithmetic aligned with the runtime glslang path. An
            # additional offline optimization pass changes depth rounding near
            # quantization boundaries even when the GLSL equations are equal.
            optimization = "-O0" if source.name.startswith(("software_", "hardware_", "game_")) else "-O"
            run(args.glslc, "--target-env=vulkan1.0", optimization, source, "-o", binary)
            try:
                run(args.spirv_val, "--target-env", "vulkan1.0", binary)
            except subprocess.CalledProcessError as error:
                raise RuntimeError("Invalid SPIR-V generated for " + source.name) from error
            data = binary.read_bytes()
            words = struct.unpack("<%dI" % (len(data) // 4), data)
            entries[key] = (words, specialization_words(words))
        lines = ["// Generated by Tools/BuildVulkanShaderBank.py. Do not edit.",
                 "// Sources retain their original license notices embedded below.", ""]
        for i, ((stage, source), (words, specs)) in enumerate(entries.items()):
            raw = struct.pack("<%dI" % len(words), *words)
            compressed = zlib.compress(raw, level=9, wbits=-15)
            lines.append("static const uint8_t builtin_%d[] = {" % i)
            for j in range(0, len(compressed), 16):
                lines.append("\t" + ", ".join("0x%02x" % b for b in compressed[j:j+16]) + ",")
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
            lines.append("{ %d,\n%s,\n builtin_%d, sizeof(builtin_%d), %d, %s, %d }," % (stage, literal, i, i, len(words), spec_array, len(specs)))
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
    parser.add_argument("--work-dir", help="Keep generated sources for diagnosis")
    generate(parser.parse_args())
