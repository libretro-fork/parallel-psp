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
"""Compile a custom Vulkan texture scaler to a .spv sidecar.

Single-pass shaders need the core's upload wrapper (the default). Use
--multipass for each complete compute program in a multipass scaler.
Requires glslc and spirv-val on the development machine, not at runtime.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--multipass", action="store_true")
    parser.add_argument("--glslc", default="glslc")
    parser.add_argument("--spirv-val", default="spirv-val")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    if not args.multipass:
        wrapper = (ROOT / "GPU/Vulkan/TextureCacheVulkan.cpp").read_text(encoding="utf-8")
        wrapper = wrapper.split('const char *uploadShader = R"(', 1)[1].split(')";', 1)[0]
        if wrapper.count("%s") != 1:
            raise RuntimeError("Texture upload wrapper changed")
        source = wrapper.replace("%s", source)
    with tempfile.TemporaryDirectory(prefix="psp-texture-shader-") as directory:
        kernel = Path(directory) / "texture.comp"
        binary = Path(directory) / "texture.spv"
        kernel.write_text(source, encoding="utf-8")
        subprocess.run([args.glslc, "--target-env=vulkan1.0", "-O", str(kernel), "-o", str(binary)], check=True)
        subprocess.run([args.spirv_val, "--target-env", "vulkan1.0", str(binary)], check=True)
        output = Path(str(args.source) + ".spv")
        output.write_bytes(binary.read_bytes())
        print(output)


if __name__ == "__main__":
    main()
