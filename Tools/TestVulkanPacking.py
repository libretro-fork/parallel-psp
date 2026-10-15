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
"""Check the generated packUnorm4x8 fallback against Vulkan's builtin.

Samples every byte at several fractional offsets, avoiding unspecified half-way
tie breaking. Uses the same offscreen executor as the fragment comparisons.
"""
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("fragments", ROOT / "Tools/TestVulkanFragments.py")
fragments = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fragments)


def main():
    source = (ROOT / "GPU/Common/FragmentShaderGenerator.cpp").read_text()
    source = source.split('WRITE(p, "uint packUnorm4x8%s', 1)[1]
    source = 'WRITE(p, "uint packUnorm4x8%s' + source.split('WRITE(p, "vec4 unpackUnorm4x8', 1)[0]
    strings = re.findall(r'WRITE\(p, ("(?:[^"\\]|\\.)*")', source)
    function = "".join(json.loads(s) for s in strings)
    function = function.replace("%s", "R", 1).replace("%s", "highp ", 1)
    if "uint packUnorm4x8R" not in function or function.count("{") != 1 or function.count("}") != 1:
        raise RuntimeError("Packing generator changed; update the extraction")
    body = r'''
layout(local_size_x=8,local_size_y=8) in;
layout(std430,set=0,binding=3) buffer Output { vec4 pixels[]; } result;
void main() {
 uvec2 xy=gl_GlobalInvocationID.xy;uint n=xy.y*32u+xy.x;
 uint group=n>>8u;
 float fraction=group==0u ? 0.0 : group==1u ? 0.49 : group==2u ? 0.51 : 0.99;
 float r=(float(n&255u)+fraction)/255.0;
 uint packed=PACK(vec4(r,1.0-r,0.25,0.75));
 result.pixels[n]=vec4(uvec4(packed&255u,(packed>>8u)&255u,(packed>>16u)&255u,packed>>24u));
}
'''
    run = fragments.bank.run
    with tempfile.TemporaryDirectory(prefix="psp-packing-compare-") as directory:
        tmp = Path(directory)
        for mode in ("ref", "new"):
            program = "#version 450\n" + (function if mode == "new" else "")
            program += body.replace("PACK", "packUnorm4x8R" if mode == "new" else "packUnorm4x8")
            shader = tmp / ("case_0_" + mode + ".comp")
            shader.write_text(program)
            run("glslc", "--target-env=vulkan1.0", "-O0", shader, "-o", shader.with_suffix(".spv"))
            run("spirv-val", "--target-env", "vulkan1.0", shader.with_suffix(".spv"))
        (tmp / "compare.cpp").write_text((ROOT / "libretro/test/vkdepaltest.cpp").read_text())
        run("c++", "-std=c++17", "-O2", tmp / "compare.cpp", "-lvulkan", "-o", tmp / "compare")
        run(tmp / "compare", tmp, 1, 0)


if __name__ == "__main__":
    main()
