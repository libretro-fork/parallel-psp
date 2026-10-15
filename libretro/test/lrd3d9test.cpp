/* Native D3D9 contract and reset fixture for the libretro core.
 * Build from the repo root with MinGW:
 * g++ -std=c++17 -O2 -Ilibretro/libretro-common/include libretro/test/lrd3d9test.cpp -o lrd3d9test.exe -ld3d9 -ld3dcompiler
 * Usage: lrd3d9test core.dll content frames [ex]
 * LRTEST_SYSTEM and LRTEST_SAVE select isolated frontend directories.
 * LRTEST_REJECT_COMPILER tests failed startup without a drawing context.
 * LRTEST_BENCH skips readbacks and lifecycle changes, timing after 30 warmup frames.
 * The window stays hidden. Tests three sync slots, state contamination,
 * savestate round trips, a resolution change and a device reset.
 */
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <vector>
#include "libretro.h"
#include "libretro_d3d9.h"

static IDirect3D9 *d3d;
static IDirect3DDevice9 *device;
static IDirect3DSurface9 *backbuffer;
static IDirect3DTexture9 *handed;
static IDirect3DTexture9 *retained[3];
static uint64_t retainedHash[3];
static retro_hw_render_callback hw{};
static retro_hw_render_interface_d3d9 iface{};
static D3DPRESENT_PARAMETERS pp{};
static unsigned index_;
static int held, errors, videos, lit, waits, resets;
static bool changed = true, update, shutdownRequested, rejectCompiler, benchmark;
static const char *resolution = "480x272";
static const char *systemDir;
static const char *saveDir;
static uint64_t lastHash;
static std::vector<uint8_t> image;
static unsigned imageWidth, imageHeight;

static void fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); errors++; }
static void log_(enum retro_log_level level, const char *fmt, ...) {
	if (level < RETRO_LOG_WARN) return;
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static uint64_t hash(IDirect3DTexture9 *texture) {
	IDirect3DSurface9 *surface = nullptr, *staging = nullptr;
	D3DSURFACE_DESC desc{};
	D3DLOCKED_RECT locked{};
	texture->GetLevelDesc(0, &desc);
	if (FAILED(texture->GetSurfaceLevel(0, &surface))
		|| FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr))
		|| FAILED(device->GetRenderTargetData(surface, staging))
		|| FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
		fail("frame readback");
		if (surface) surface->Release();
		if (staging) staging->Release();
		return 0;
	}
	image.resize(desc.Width * desc.Height * 4);
	imageWidth = desc.Width; imageHeight = desc.Height;
	uint64_t value = 1469598103934665603ULL;
	bool nonzero = false;
	for (unsigned y = 0; y < desc.Height; y++) {
		const uint32_t *row = (const uint32_t *)((const uint8_t *)locked.pBits + y * locked.Pitch);
		for (unsigned x = 0; x < desc.Width; x++) {
			const uint32_t rgba = (row[x] & 0xFF00FF00) | ((row[x] >> 16) & 0xFF) | ((row[x] & 0xFF) << 16);
			memcpy(image.data() + (y * desc.Width + x) * 4, &rgba, 4);
			for (int b = 0; b < 4; b++) value = (value ^ ((rgba >> (b * 8)) & 0xFF)) * 1099511628211ULL;
			nonzero |= (row[x] & 0xFFFFFF) != 0;
		}
	}
	if (nonzero) lit++;
	staging->UnlockRect(); staging->Release(); surface->Release();
	return value;
}
static HRESULT WINAPI rejectCompile(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, void **code, void **messages) {
	if (code) *code = nullptr;
	if (messages) *messages = nullptr;
	return E_FAIL;
}
static bool lock_(void *) { bool result = changed; changed = false; held++; return result; }
static void unlock_(void *) { if (--held < 0) fail("unbalanced unlock"); }
static unsigned getIndex(void *) { return index_; }
static unsigned getMask(void *) { return 7; }
static void wait_(void *) {
	if (held) fail("wait while holding device");
	waits++;
	if (retained[index_]) {
		if (!benchmark && hash(retained[index_]) != retainedHash[index_]) fail("texture reused before wait");
		retained[index_]->Release(); retained[index_] = nullptr;
	}
}
static void setTexture(void *, IDirect3DTexture9 *texture) {
	if (!held) fail("set_texture without device");
	handed = texture;
}
static void releaseFrames() {
	for (auto &texture : retained) { if (texture) texture->Release(); texture = nullptr; }
	handed = nullptr;
}
static uintptr_t framebuffer() { return 0; }
static void video(const void *data, unsigned, unsigned, size_t) {
	if (held) fail("video_refresh while holding device");
	if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		if (!handed) { fail("frame without texture"); return; }
		if (retained[index_]) fail("slot was not waited");
		retained[index_] = handed; handed->AddRef();
		if (!benchmark) lastHash = retainedHash[index_] = hash(handed);
		videos++;
		handed = nullptr; index_ = (index_ + 1) % 3;
	}
	// A frontend changes both bindings and fixed-function state between core frames.
	device->SetRenderTarget(0, backbuffer);
	device->SetDepthStencilSurface(nullptr);
	device->SetVertexShader(nullptr); device->SetPixelShader(nullptr);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
	for (unsigned i = 0; i < 16; i++) device->SetTexture(i, nullptr);
	changed = true;
}
static bool environment(unsigned command, void *data) {
	switch (command) {
	case RETRO_ENVIRONMENT_SHUTDOWN: shutdownRequested = true; return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((retro_log_callback *)data)->log = log_; return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = systemDir; return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = saveDir; return true;
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: *(const char **)data = systemDir; return true;
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: *(retro_hw_context_type *)data = RETRO_HW_CONTEXT_D3D9; return true;
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
		if (((retro_hw_render_callback *)data)->context_type != RETRO_HW_CONTEXT_D3D9) return false;
		((retro_hw_render_callback *)data)->get_current_framebuffer = framebuffer;
		hw = *(retro_hw_render_callback *)data; return true;
	case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: *(const retro_hw_render_interface **)data = (const retro_hw_render_interface *)&iface; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		auto *v = (retro_variable *)data;
		if (!strcmp(v->key, "ppsspp_backend")) v->value = "d3d9";
		else if (!strcmp(v->key, "ppsspp_internal_resolution")) v->value = resolution;
		else if (!strcmp(v->key, "ppsspp_fast_memory")) v->value = "disabled";
		else if (!strcmp(v->key, "ppsspp_frame_duplication")) v->value = "disabled";
		else return false;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = update; update = false; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_VARIABLES: return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
	default: return false;
	}
}
static size_t audioBatch(const int16_t *, size_t frames) { return frames; }
static void audio(int16_t, int16_t) {}
static void poll() {}
static int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }
#define LOAD(name) auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(module, #name))
int main(int argc, char **argv) {
	if (argc < 4) return 2;
	benchmark = getenv("LRTEST_BENCH") != nullptr;
	rejectCompiler = getenv("LRTEST_REJECT_COMPILER") != nullptr;
	systemDir = getenv("LRTEST_SYSTEM"); saveDir = getenv("LRTEST_SAVE");
	if (!systemDir || !saveDir) return 2;
	HWND window = CreateWindowA("STATIC", "PSP D3D9 test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
	pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	const DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
	if (argc > 4) {
		IDirect3D9Ex *ex = nullptr; IDirect3DDevice9Ex *devEx = nullptr;
		if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &ex)) || FAILED(ex->CreateDeviceEx(0, D3DDEVTYPE_HAL, window, flags, &pp, nullptr, &devEx))) return 4;
		d3d = ex; device = devEx;
	} else {
		d3d = Direct3DCreate9(D3D_SDK_VERSION);
		if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, flags, &pp, &device))) return 4;
	}
	device->GetRenderTarget(0, &backbuffer);
	iface.interface_type = RETRO_HW_RENDER_INTERFACE_D3D9; iface.interface_version = RETRO_HW_RENDER_INTERFACE_D3D9_VERSION;
	iface.handle = &iface; iface.device = device; iface.D3DCompile = reinterpret_cast<retro_d3d9_compile_t>(D3DCompile);
	if (rejectCompiler) iface.D3DCompile = rejectCompile;
	iface.lock_context = lock_; iface.unlock_context = unlock_; iface.set_texture = setTexture;
	iface.get_sync_index = getIndex; iface.get_sync_index_mask = getMask; iface.wait_sync_index = wait_;
	HMODULE module = LoadLibraryA(argv[1]); if (!module) { fprintf(stderr, "LoadLibrary: %lu\n", GetLastError()); return 3; }
	LOAD(retro_init); LOAD(retro_deinit); LOAD(retro_set_environment); LOAD(retro_set_video_refresh);
	LOAD(retro_set_audio_sample); LOAD(retro_set_audio_sample_batch); LOAD(retro_set_input_poll); LOAD(retro_set_input_state);
	LOAD(retro_load_game); LOAD(retro_unload_game); LOAD(retro_run); LOAD(retro_serialize_size); LOAD(retro_serialize); LOAD(retro_unserialize);
	retro_set_environment(environment); retro_set_video_refresh(video); retro_set_audio_sample(audio); retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(poll); retro_set_input_state(input); retro_init();
	retro_game_info content{}; content.path = argv[2];
	if (!retro_load_game(&content) || !hw.context_reset) return 5;
	hw.context_reset();
	const int frames = atoi(argv[3]);
	LARGE_INTEGER frequency, start, end; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
	const ULONGLONG bootStart = GetTickCount64();
	for (int i = 0; i < frames && !shutdownRequested; i++) {
		if (benchmark && i == 30) QueryPerformanceCounter(&start);
		retro_run();
		if (shutdownRequested) break;
		if (!videos) {
			if (GetTickCount64() - bootStart > 30000) { fail("boot timeout"); break; }
			Sleep(1);
			i--;
			continue;
		}
		if (!benchmark && i == frames / 3) {
			size_t size = retro_serialize_size(); void *state = malloc(size);
			if (!state || !retro_serialize(state, size) || !retro_unserialize(state, size)) fail("savestate round trip");
			free(state); resolution = "960x544"; update = true;
		}
		if (!benchmark && i == 2 * frames / 3) {
			hw.context_destroy(); releaseFrames();
			device->SetRenderTarget(0, backbuffer); device->SetDepthStencilSurface(nullptr);
			device->SetStreamSource(0, nullptr, 0, 0); device->SetIndices(nullptr);
			for (unsigned s = 0; s < 16; s++) device->SetTexture(s, nullptr);
			backbuffer->Release(); backbuffer = nullptr;
			if (FAILED(device->Reset(&pp))) { fail("device reset with released core resources"); break; }
			device->GetRenderTarget(0, &backbuffer); changed = true; resets++; hw.context_reset();
		}
	}
	QueryPerformanceCounter(&end);
	if (const char *path = getenv("LRTEST_IMAGE")) {
		if (FILE *out = fopen(path, "wb")) {
			fprintf(out, "P6\n%u %u\n255\n", imageWidth, imageHeight);
			for (size_t i = 0; i < image.size(); i += 4) fwrite(image.data() + i, 1, 3, out);
			fclose(out);
		}
	}
	hw.context_destroy(); releaseFrames(); retro_unload_game(); retro_deinit();
	if (held) fail("device ownership leaked");
	if (rejectCompiler) {
		if (!shutdownRequested || videos) fail("compiler rejection did not shut down cleanly");
	} else if (!videos || (!benchmark && (!lit || !resets))) {
		fail("fixture did not reach rendering and reset");
	}
	printf("@@ frames=%d video=%d lit=%d waits=%d resets=%d errors=%d last=%016llx seconds=%.3f\n", frames, videos, lit, waits, resets, errors, (unsigned long long)lastHash, (double)(end.QuadPart-start.QuadPart)/frequency.QuadPart);
	// Unload while the calling thread and its C runtime TLS are still alive.
	// Process-detach order is not a substitute for the frontend's normal core unload.
	if (!FreeLibrary(module)) fail("core library unload");
	backbuffer->Release(); device->Release(); d3d->Release(); DestroyWindow(window);
	return errors ? 1 : 0;
}
