# Building the libretro core

The sole runtime target is `ppsspp_libretro`. Configure with CMake and build that target,
or use `make -C libretro` with the appropriate `platform` setting.

For development tests, configure with `-DUNITTEST=ON`, build `PPSSPPUnitTest`,
and run `PPSSPPUnitTest all`. Game fixtures must run through a libretro frontend.
The in-game PSP keyboard and PSP timing remain part of the core.

## Precompiled Vulkan utility shaders

Normal core builds use the checked-in `Common/GPU/Vulkan/BuiltinShaders.inl`.
Regenerate it after changing thin3d presets, Draw2D shaders or ShaderWriter with
`python3 Tools/BuildVulkanShaderBank.py`. Regeneration needs a host C++ compiler,
`glslc` and `spirv-val`; `--check` checks that the bank matches the generators
with the same toolchain. No shader compiler executable is needed by a normal build.

This bank covers fixed presentation, framebuffer-copy, palette conversion and shipped
texture-scaling shaders. Palette parameters use SPIR-V specialization defaults; the
bank lookup patches the defaults without invoking a shader compiler. Game shaders
and custom texture-scaling shaders still use runtime glslang. Removing that
dependency requires replacing those remaining generators as well.

`python3 Tools/TestVulkanDepal.py` compares the legacy and bank palette equations
using an offscreen Vulkan compute wrapper with nearest and linear sampling. It
requires a host C++ compiler, glslc, spirv-val and Vulkan headers/loader. Set
`VK_ICD_FILENAMES` to a lavapipe ICD to run without a GPU or window. This checks
conversion math; it does not replace a graphics pipeline or game-image test.
