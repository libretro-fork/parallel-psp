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

This bank covers fixed presentation and framebuffer-copy shaders. Game shaders,
depalettization and texture-scaling shaders still use runtime glslang. Removing
that dependency requires replacing those remaining generators as well.
