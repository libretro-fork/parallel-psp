#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the fixed video conversion shader; no runtime compiler is needed."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="glslangValidator")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source = ROOT / "Common/GPU/Vulkan/VideoSurface.comp"
    output = source.with_name("VulkanVideoSurfaceSpv.h")
    with tempfile.TemporaryDirectory(prefix="video-surface-shader-") as directory:
        binary = Path(directory) / "video.spv"
        subprocess.run([args.compiler, "-V", str(source), "-o", str(binary)], check=True)
        data = binary.read_bytes()
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    header = ("// Copyright (c) 2012- PPSSPP Project.\n"
              "// SPDX-License-Identifier: GPL-2.0-or-later\n"
              "// Generated from VideoSurface.comp with glslangValidator -V.\n"
              "#pragma once\n#include <cstdint>\nstatic const uint32_t videoSurfaceSpv[] = {\n")
    header += "".join("\t" + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",\n"
                      for i in range(0, len(words), 8)) + "};\n"
    if args.check:
        assert output.read_bytes().replace(b"\r\n", b"\n") == header.encode(), "Shader header needs regeneration"
    else:
        newline = b"\r\n" if output.exists() and b"\r\n" in output.read_bytes() else b"\n"
        output.write_bytes(header.encode().replace(b"\n", newline))


if __name__ == "__main__":
    main()
