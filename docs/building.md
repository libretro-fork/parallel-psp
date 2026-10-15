# Building the libretro core

The sole runtime target is `ppsspp_libretro`. Configure with CMake and build that target,
or use `make -C libretro` with the appropriate `platform` setting.

For development tests, configure with `-DUNITTEST=ON`, build `PPSSPPUnitTest`,
and run `PPSSPPUnitTest all`. Game fixtures must run through a libretro frontend.
The in-game PSP keyboard and PSP timing remain part of the core.

## Precompiled Vulkan shaders

Normal core builds use the checked-in `Common/GPU/Vulkan/BuiltinShaders.inl`.
No GLSL compiler is built into or loaded by the core. CMake and the libretro
Makefile do not need the former glslang submodule.

Regenerate the bank after changing its generators, uniforms, ShaderWriter or
shipped texture scalers with `python3 Tools/BuildVulkanShaderBank.py`.
Regeneration needs a host C++ compiler, `glslc` and `spirv-val`; `--check` checks
reproducibility with the same toolchain. Utility, presentation, stencil,
framebuffer reinterpretation, palette, shipped compute and game shaders are
covered. Game state uses uint32 specialization defaults patched into the bank.
Interface variants cover texture targets, dual-source blending, depth outputs,
clip/cull distance, flat interpolation and stereo. Hardware vertex inputs absent
from a decoded format receive bounded placeholder fetches; specialized programs
never consume them. Lighting uses constant UBO indices.

Custom texture scalers need a `.spv` sidecar beside each compute source. Run
`python3 Tools/CompileTextureShader.py path/to/scaler.glsl` for a single-pass
scaler, or add `--multipass` for each complete compute stage of a multipass
scaler. Recompile the sidecar when changing its source. Shipped scalers use the
bank directly. Missing sidecars produce a diagnostic and leave that scaler
unavailable, rather than invoking a runtime compiler.

Shader syntax unit tests invoke the external `glslangValidator` executable on
PATH; it is only a development dependency. Core builds do not need it.
`PPSSPPUnitTest all` additionally checks bank lookup, specialization, interface
variants and the absence of RelaxedPrecision in game shader modules.

`Tools/TestVulkanVertices.py`, `Tools/TestVulkanFragments.py` and
`Tools/TestVulkanDepal.py` compare the legacy and bank equations through offscreen
Vulkan compute wrappers. They require a host C++ compiler, glslc, spirv-val and
Vulkan headers/loader. Set `VK_ICD_FILENAMES` to a lavapipe ICD and
`LP_NUM_THREADS=4` for windowless software execution. The fragment test also
checks byte unpacking and bilinear palette filtering against independent oracles. These tests
check math; graphics pipelines, derivatives, multiview and game presentation
still need frontend tests.

Vulkan shader calculations use full precision to avoid extra host FP16 rounding.
Explicit GE depth truncation, lighting-power approximation, color-test rounding
and palette-channel reconstruction remain. Stereo eye offsets preserve their
float32 bits. CLUT8 extraction from RGBA recovers normalized bytes with 255.99;
the former 254.99 scale incorrectly decremented nonzero bytes.
`Tools/TestVulkanPacking.py` checks the shared color-packing fallback against
the Vulkan builtin for every byte at several fractional offsets. The fallback
rounds to nearest rather than truncating; exact half-way ties are excluded
because the GLSL packing specification leaves tie breaking unspecified.

## Delayed framebuffer readback

Only callers using `ReadbackMode::OLD_DATA_OK` can use cached pixels. OpenGL 3.2+
and GLES 3 use two pixel-pack buffers with zero-timeout fence checks; completed
copies reach the emulation thread through a bounded atomic mailbox. D3D11 uses two staging
textures and `D3D11_MAP_FLAG_DO_NOT_WAIT`. Each framebuffer keeps its own cache.
The first read of a rectangle fetches current pixels synchronously. Later reads
reuse the latest completed copy and skip a new submission if both slots are busy.
Current-data, depth/stencil and unsupported GL paths keep synchronous semantics.
GL allocation, fence or mapping failures fall back to synchronous reads.

Vulkan delayed staging keys include the framebuffer, rectangle and aspect.
Before submission, repeated delayed copies to the same staging buffer coalesce
to the last copy. Synchronous consumers remain ordering boundaries. A delayed
read with no framebuffer falls back to a synchronous backbuffer copy.

D3D9 reuses its system-memory readback surface across compatible requests.
For non-MSAA rectangles smaller than half the target, it first tries a GPU crop
into a reusable render target. Only the crop is downloaded. Unsupported crop or
download operations retry the original full-target path. The download still
blocks; this reduces transfer size, not the number of waits.

Set `PPSSPP_READBACK_STATS=1` before launching a frontend for per-context,
per-tag stdout summaries on shutdown for Vulkan, GL, D3D11 and D3D9. Normal operation
does not sample a timer or allocate statistics rows. `requested_bytes` counts
logical destination bytes; it does not imply a completed GPU transfer.
`blocking_requests` counts explicit BLOCK requests, excluding implicit bootstrap
or fallback stalls. `elapsed_ms` includes host waiting, conversion and copying,
not isolated GPU execution time. `coalesced` counts removed Vulkan copy steps.
`download_attempt_bytes` is tracked for D3D9 color downloads only, including
failed attempts; zero on other backends means untracked. Caller tags distinguish
first-frame, compatibility, full framebuffer and block-transfer reads where the
shared path forwards them. `PPSSPPUnitTest VulkanReadbackPlan` checks queue
coalescing boundaries and independent rectangle/aspect cache keys.

On Windows, `PPSSPPUnitTest D3D11Readback D3D9Readback GLReadback` exercises the
native backends without showing a game window. The D3D11 check uses WARP; the D3D9
and desktop GL checks require a driver. Their device-unavailable cases report a
skip. The tests cover delayed/current results, rectangle changes, padded rows,
format conversion, depth/stencil staging, PBO publication and resource cleanup.
No savestate layout or shader cache key changes are involved.
