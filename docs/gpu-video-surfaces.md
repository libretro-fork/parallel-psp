# GPU-backed PSP video buffers

With a version-3 libretro video decoder and Windows Vulkan, 32-bit linear video
output can stay on the GPU. The frontend leases an independent decoded NV12
image; a checked-in compute shader converts it using the PSP integer YUV
conversion, and the core draws it into a framebuffer associated with the PSP
buffer address. Display and compatible texture reads use that framebuffer.
There is no decoded-frame download or subsequent upload on this path.

The conversion waits for the decoder timeline semaphore and signals the lease
completion semaphore on the graphics queue. Descriptor sets are immutable per
lease. Decoder destruction is deferred until graphics work has finished.
`Tools/GenerateVideoSurfaceShader.py` regenerates the embedded shader offline;
the core does not compile this shader at runtime.

## Guest memory coherence

The guest RAM copy is deferred while the GPU framebuffer owns the pixels.
Windows protects every RAM/VRAM alias of its pages. Native memory helpers resolve
pending output before reading or handing out pointers; generated JIT loads and
stores fault into the same resolver. Unrelated faults are passed through.

The resolver downloads once before CPU access, including partial writes and
accesses to padding on a protected page. Subsequent GPU consumers upload any CPU
changes before sampling. A complete replacement by another video frame discards
the previous deferred copy. Shared pages and capacity eviction resolve the older
owner. Memory copies, block transfers, savestates and device teardown resolve
pending output before using or destroying its storage. No serialized fields were
added. CPU access between frames uses an offscreen submission through the
existing synchronous readback path.

The firmware videocodec path also keeps its eight tiled YUV buffers on the GPU.
Full YUV copies share the surface; CPU access restores all eight components once.
A second GPU image preserves the original Y/Cb/Cr bytes, so this restoration is
lossless rather than a reverse conversion from RGB. ME allocations resolve through
ME pointer access; ordinary guest RAM uses the protected-page owner mechanism.
Native-size framebuffers contain visible pixels only, leaving row padding in RAM.

## Fallback and validation

Other operating systems/renderers, 16-bit output, swizzled VRAM, wrapped output
ranges, incompatible framebuffer dimensions/scales, unavailable GPU views or
leases, and older frontends retain the existing CPU path. The PMP access-unit
path also retains CPU conversion. This is a Vulkan implementation, not a D3D11
or OpenGL surface importer.

`VideoMemory` tests range ownership, aliases, wraparound, partial/full overwrites,
shared pages, eviction and failure retention. `VideoMemoryHost` exercises native
Windows protected aliases. Configure CMake with `-DUNITTEST=ON
-DVIDEO_SURFACE_GPU_TEST=ON` and run `PPSSPPUnitTest VulkanVideoSurface` for the
optional device test. It compares conversion to scalar PSP arithmetic, checks
replacement without readback, RAM/VRAM aliases, partial CPU writes, padding and
between-frame materialization. Its conversion input is synthetic GPU planes;
frontend decoder export is tested separately in RetroArch's
`tools/test_video_decode_interface.py` and D3D11 decoder fixtures.

The main regression risks are memory protection/reentrancy, framebuffer alias
selection, GPU queue ordering and device/context lifetime. Game tests must show
that the GPU buffer path was reached; a successful fallback run alone does not
validate this optimization.
