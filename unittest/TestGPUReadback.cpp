// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/GPU/thin3d.h"
#include "ppsspp_config.h"
#include "unittest/UnitTest.h"
#include "Common/GPU/Vulkan/VulkanQueueRunner.h"

bool TestVulkanReadbackPlan() {
	CachedReadback first{}, second{};
	std::vector<VKRStep *> steps;
	auto add = [&](CachedReadback *dst, bool delayed) {
		auto *step = new VKRStep(VKRStepType::READBACK);
		step->readback.delayed = delayed;
		step->readback.dst = dst;
		steps.push_back(step);
	};
	add(&first, true);
	steps.push_back(new VKRStep(VKRStepType::RENDER));
	add(&second, true);
	add(&first, true);
	EXPECT_EQ_INT(CoalescePendingReadbacks(steps, &first), 2);
	EXPECT_EQ_INT(steps.size(), 2);
	EXPECT_TRUE(steps[0]->stepType == VKRStepType::RENDER);
	EXPECT_TRUE(steps[1]->readback.dst == &second);
	add(&first, false);
	add(&first, true);
	EXPECT_EQ_INT(CoalescePendingReadbacks(steps, &first), 1);
	EXPECT_TRUE(!steps.back()->readback.delayed);
	for (auto *step : steps) delete step;
	steps.clear();
	add(&first, true);
	steps.push_back(new VKRStep(VKRStepType::READBACK_IMAGE));
	EXPECT_EQ_INT(CoalescePendingReadbacks(steps, &first), 0);
	for (auto *step : steps) delete step;
	// Equal-size rectangles at distinct origins/aspects must not share staging.
	DenseHashMap<ReadbackKey, int> cache(8);
	ReadbackKey key{nullptr, 7, 3, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT, 0};
	cache.Insert(key, 1);
	key.x = 3; cache.Insert(key, 2);
	key.y = 4; cache.Insert(key, 3);
	key.aspect = VK_IMAGE_ASPECT_DEPTH_BIT; cache.Insert(key, 4);
	int found = 0;
	EXPECT_TRUE(cache.Get(key, &found)); EXPECT_EQ_INT(found, 4);
	key.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	EXPECT_TRUE(cache.Get(key, &found)); EXPECT_EQ_INT(found, 3);
	key.y = 1; EXPECT_TRUE(cache.Get(key, &found)); EXPECT_EQ_INT(found, 2);
	key.x = 2; EXPECT_TRUE(cache.Get(key, &found)); EXPECT_EQ_INT(found, 1);
	return true;
}

#ifdef _WIN32
#include "Common/GPU/D3D9/D3D9StateCache.h"
#include "Common/GPU/thin3d_create.h"
#include <d3d11_1.h>
#include <d3d9.h>
#ifndef USING_GLES2
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/GPU/OpenGL/GLRenderManager.h"
#endif
#include <memory>
#include <vector>

bool TestD3D11Readback() {
	using namespace Draw;
	using Microsoft::WRL::ComPtr;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL level;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
								 &level, &context))) {
		printf("SKIP: D3D11 WARP device unavailable.\n");
		return true;
	}
	std::unique_ptr<DrawContext> draw(T3DCreateD3D11Context(device, context, {}, {}, {}, level, nullptr, {}));
	draw->BeginFrame(DebugFlags::NONE);
	FramebufferDesc desc{640, 32, 1, 1, 0, true, "ReadbackTest"};
	AutoRef<Framebuffer> fb(draw->CreateFramebuffer(desc));
	EXPECT_TRUE(fb);
	auto clear = [&](uint32_t color) {
		RenderPassInfo rp{RPAction::CLEAR, RPAction::CLEAR, RPAction::CLEAR, color, 1.0f, 0, "ReadbackTest"};
		draw->BindFramebufferAsRenderTarget(fb, rp, "ReadbackTest");
	};
	constexpr uint32_t a = 0xff332211, b = 0xff665544, c = 0xff998877;
	constexpr int stride = 11, width = 7, height = 3;
	std::vector<uint32_t> pixels(stride * height, 0xdeadbeef);
	auto read = [&](ReadbackMode mode, int x = 2, int y = 1) {
		return draw->CopyFramebufferToMemory(fb, Aspect::COLOR_BIT, x, y, width, height, DataFormat::R8G8B8A8_UNORM,
											 pixels.data(), stride, mode, "ReadbackTest");
	};
	auto check = [&](uint32_t color) {
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < stride; ++x) {
				if (pixels[y * stride + x] != (x < width ? color : 0xdeadbeef)) {
					return false;
				}
			}
		}
		return true;
	};
	clear(a);
	EXPECT_TRUE(read(ReadbackMode::OLD_DATA_OK));
	EXPECT_TRUE(check(a));
	clear(b);
	EXPECT_TRUE(read(ReadbackMode::OLD_DATA_OK));
	EXPECT_TRUE(check(a)); // A fresh submission must not overwrite the cached result.
	EXPECT_TRUE(read(ReadbackMode::BLOCK));
	EXPECT_TRUE(check(b)); // Blocking reads must return the current image.
	clear(c);
	EXPECT_TRUE(read(ReadbackMode::OLD_DATA_OK));
	EXPECT_TRUE(check(b)); // The blocking read also completed the previous staging copy.
	EXPECT_TRUE(read(ReadbackMode::BLOCK));
	EXPECT_TRUE(check(c));
	EXPECT_TRUE(read(ReadbackMode::OLD_DATA_OK, 600, 2)); // Shape/key change bootstraps.
	EXPECT_TRUE(check(c));
	clear(a);
	EXPECT_TRUE(read(ReadbackMode::BLOCK, 600, 2)); // Beyond the 512x512 shared staging area.
	EXPECT_TRUE(check(a));
	uint16_t packed[width * height]{};
	EXPECT_TRUE(draw->CopyFramebufferToMemory(fb, Aspect::COLOR_BIT, 600, 2, width, height,
											  DataFormat::R5G6B5_UNORM_PACK16, packed, width, ReadbackMode::OLD_DATA_OK,
											  "ReadbackTest"));
	const uint16_t expected565 = ((a & 0xff) >> 3) | (((a >> 8) & 0xff) >> 2 << 5) | (((a >> 16) & 0xff) >> 3 << 11);
	// The delayed cache still contains c at this rectangle; its earlier submission is allowed.
	const uint16_t cached565 = ((c & 0xff) >> 3) | (((c >> 8) & 0xff) >> 2 << 5) | (((c >> 16) & 0xff) >> 3 << 11);
	for (uint16_t pixel : packed) {
		EXPECT_EQ_INT(pixel, cached565);
	}
	EXPECT_TRUE(draw->CopyFramebufferToMemory(fb, Aspect::COLOR_BIT, 600, 2, width, height,
		DataFormat::R5G6B5_UNORM_PACK16, packed, width, ReadbackMode::BLOCK, "ReadbackTest"));
	for (uint16_t pixel : packed) {
		EXPECT_EQ_INT(pixel, expected565);
	}
	RenderPassInfo depthPass{RPAction::KEEP, RPAction::CLEAR, RPAction::CLEAR, 0, 0.25f, 0x5a, "DepthReadbackTest"};
	draw->BindFramebufferAsRenderTarget(fb, depthPass, "DepthReadbackTest");
	float depth[stride * height];
	std::fill(std::begin(depth), std::end(depth), -1.0f);
	EXPECT_TRUE(draw->CopyFramebufferToMemory(fb, Aspect::DEPTH_BIT, 600, 2, width, height, DataFormat::D32F, depth,
											  stride, ReadbackMode::BLOCK, "DepthReadbackTest"));
	uint8_t stencil[stride * height];
	memset(stencil, 0xcc, sizeof(stencil));
	EXPECT_TRUE(draw->CopyFramebufferToMemory(fb, Aspect::STENCIL_BIT, 600, 2, width, height, DataFormat::S8, stencil,
											  stride, ReadbackMode::BLOCK, "StencilReadbackTest"));
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < stride; ++x) {
			EXPECT_TRUE(std::abs(depth[y * stride + x] - (x < width ? 0.25f : -1.0f)) < 0.000001f);
			EXPECT_EQ_INT(stencil[y * stride + x], x < width ? 0x5a : 0xcc);
		}
	}
	draw->EndFrame();
	draw->BindFramebufferAsRenderTarget(nullptr, {RPAction::KEEP, RPAction::KEEP, RPAction::KEEP}, "Unbind");
	fb = nullptr;
	return true;
}
bool TestD3D9Readback() {
	using namespace Draw;
	using Microsoft::WRL::ComPtr;
	HWND window = CreateWindowExW(0, L"STATIC", L"ReadbackTest", WS_POPUP, 0, 0, 32, 32, nullptr, nullptr,
								  GetModuleHandleW(nullptr), nullptr);
	EXPECT_TRUE(window);
	ComPtr<IDirect3D9> d3d;
	d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
	EXPECT_TRUE(d3d);
	D3DPRESENT_PARAMETERS pp{};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = window;
	pp.BackBufferWidth = 32;
	pp.BackBufferHeight = 32;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	ComPtr<IDirect3DDevice9> device;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
								 D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device))) {
		printf("SKIP: D3D9 device unavailable.\n");
		DestroyWindow(window);
		return true;
	}
	pD3Ddevice9 = device;
	{
		std::unique_ptr<DrawContext> draw(T3DCreateDX9Context(d3d.Get(), nullptr, 0, device.Get(), nullptr));
		EXPECT_TRUE(draw);
		for (int width : {16, 32, 16}) {
			FramebufferDesc desc{width, 16, 1, 1, 0, false, "ReadbackTest"};
			AutoRef<Framebuffer> fb(draw->CreateFramebuffer(desc));
			EXPECT_TRUE(fb);
			for (uint32_t color : {0xff332211u, 0xff665544u}) {
				RenderPassInfo rp{RPAction::CLEAR, RPAction::KEEP, RPAction::KEEP, color, 1.0f, 0, "ReadbackTest"};
				draw->BindFramebufferAsRenderTarget(fb, rp, "ReadbackTest");
				// A distinct rectangle catches wrong crop origins, not just pitch.
				D3DVIEWPORT9 viewport{0, 0, (DWORD)width, 16, 0.0f, 1.0f};
				EXPECT_TRUE(SUCCEEDED(device->SetViewport(&viewport)));
				EXPECT_TRUE(SUCCEEDED(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE)));
				D3DRECT patch{2, 3, 9, 6};
				EXPECT_TRUE(SUCCEEDED(device->Clear(1, &patch, D3DCLEAR_TARGET, 0xff112233, 1.0f, 0)));
				EXPECT_TRUE(SUCCEEDED(device->BeginScene()));
				std::vector<uint32_t> pixels(11 * 3, 0xdeadbeef);
				for (ReadbackMode mode : {ReadbackMode::BLOCK, ReadbackMode::OLD_DATA_OK}) {
					EXPECT_TRUE(draw->CopyFramebufferToMemory(fb, Aspect::COLOR_BIT, 2, 3, 7, 3,
															  DataFormat::R8G8B8A8_UNORM, pixels.data(), 11, mode,
															  "ReadbackTest"));
					for (int y = 0; y < 3; ++y) {
						for (int x = 0; x < 11; ++x) {
							EXPECT_EQ_HEX(pixels[y * 11 + x], x < 7 ? 0xff332211u : 0xdeadbeefu);
						}
					}
				}
				EXPECT_TRUE(SUCCEEDED(device->EndScene()));
				// A full-target read exercises the uncropped path after cached crops.
				std::vector<uint32_t> full(width * 16);
				EXPECT_TRUE(draw->CopyFramebufferToMemory(fb, Aspect::COLOR_BIT, 0, 0, width, 16,
					DataFormat::R8G8B8A8_UNORM, full.data(), width, ReadbackMode::BLOCK, "ReadbackFullTest"));
				EXPECT_EQ_HEX(full[0], color);
				EXPECT_EQ_HEX(full[3 * width + 2], 0xff332211u);
			}
			draw->HandleEvent(Event::LOST_BACKBUFFER, 0, 0);
		}
	}
	pD3Ddevice9 = nullptr;
	device = nullptr;
	d3d = nullptr;
	DestroyWindow(window);
	return true;
}

#ifndef USING_GLES2
bool TestGLReadback() {
	HWND window = CreateWindowExW(0, L"STATIC", L"ReadbackTest", WS_POPUP, 0, 0, 32, 32, nullptr, nullptr,
								  GetModuleHandleW(nullptr), nullptr);
	EXPECT_TRUE(window);
	HDC dc = GetDC(window);
	PIXELFORMATDESCRIPTOR format{};
	format.nSize = sizeof(format);
	format.nVersion = 1;
	format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	format.iPixelType = PFD_TYPE_RGBA;
	format.cColorBits = 32;
	int index = ChoosePixelFormat(dc, &format);
	EXPECT_TRUE(index && SetPixelFormat(dc, index, &format));
	HGLRC context = wglCreateContext(dc);
	EXPECT_TRUE(context && wglMakeCurrent(dc, context));
	EXPECT_EQ_INT(glewInit(), GLEW_OK);
	while (glGetError() != GL_NO_ERROR) {
	}
	ResetGLExtensions();
	CheckGLExtensions();
	if (!gl_extensions.VersionGEThan(3, 2)) {
		printf("SKIP: OpenGL 3.2 required for PBO/fence readback checks.\n");
		wglMakeCurrent(nullptr, nullptr);
		wglDeleteContext(context);
		ReleaseDC(window, dc);
		DestroyWindow(window);
		ResetGLExtensions();
		return true;
	}
	{
		GLQueueRunner runner;
		runner.CreateDeviceObjects();
		Draw::DeviceCaps caps{};
		GLRFramebuffer fb(caps, 16, 16, false, "ReadbackTest");
		fb.delayedReadback = std::make_unique<GLDelayedReadback>();
		glGenTextures(1, &fb.color_texture.texture);
		glBindTexture(GL_TEXTURE_2D, fb.color_texture.texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glGenFramebuffers(1, &fb.handle);
		glBindFramebuffer(GL_FRAMEBUFFER, fb.handle);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.color_texture.texture, 0);
		EXPECT_EQ_INT(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
		GLRStep step(GLRStepType::READBACK);
		step.tag = "ReadbackTest";
		step.readback.src = &fb;
		step.readback.srcRect = {2, 1, 7, 3};
		step.readback.aspectMask = GL_COLOR_BUFFER_BIT;
		step.readback.dstFormat = Draw::DataFormat::R8G8B8A8_UNORM;
		step.readback.delayed = true;
		GLFrameData frame{};
		auto run = [&]() {
			runner.RunSteps({&step}, frame, false, true, false);
			GLint binding = -1;
			glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &binding);
			return binding == 0 && glGetError() == GL_NO_ERROR;
		};
		auto clear = [&](uint32_t color) {
			glBindFramebuffer(GL_FRAMEBUFFER, fb.handle);
			glClearColor((color & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, ((color >> 16) & 255) / 255.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		};
		auto check = [&](uint32_t expected, int width, int height) {
			auto result = fb.delayedReadback->Take();
			if (!result || result->rect.w != width || result->rect.h != height ||
				result->pixels.size() != (size_t)width * height * 4) {
				return false;
			}
			for (size_t i = 0; i < result->pixels.size(); i += 4) {
				uint32_t pixel;
				memcpy(&pixel, result->pixels.data() + i, 4);
				if (pixel != expected) {
					return false;
				}
			}
			return true;
		};
		clear(0xff332211);
		EXPECT_TRUE(run());
		EXPECT_TRUE(!fb.delayedReadback->Take());
		glFinish(); // Test-only completion, never used by the delayed runtime path.
		clear(0xff665544);
		EXPECT_TRUE(run());
		EXPECT_TRUE(check(0xff332211, 7, 3));
		glFinish();
		clear(0xff998877);
		EXPECT_TRUE(run());
		EXPECT_TRUE(check(0xff665544, 7, 3));
		step.readback.srcRect = {1, 2, 3, 2};
		EXPECT_TRUE(run()); // Discard pending copies of a different rectangle.
		EXPECT_TRUE(!fb.delayedReadback->Take());
		glFinish();
		clear(0xff332211);
		EXPECT_TRUE(run());
		EXPECT_TRUE(check(0xff998877, 3, 2));
		glFinish();
		clear(0xff665544);
		EXPECT_TRUE(run());  // Leave A in the mailbox, as if the CPU were busy.
		glFinish();
		clear(0xff998877);
		EXPECT_TRUE(run());  // B replaces A rather than queuing another full image.
		EXPECT_TRUE(check(0xff665544, 3, 2));
		EXPECT_FALSE(fb.delayedReadback->Take());
		fb.delayedReadback->Destroy(false);
		EXPECT_EQ_INT(glGetError(), GL_NO_ERROR);
		runner.DestroyDeviceObjects();
	}
	wglMakeCurrent(nullptr, nullptr);
	wglDeleteContext(context);
	ReleaseDC(window, dc);
	DestroyWindow(window);
	ResetGLExtensions();
	return true;
}

#else
bool TestGLReadback() {
	printf("SKIP: Desktop OpenGL test requires WGL.\n");
	return true;
}
#endif
#else
bool TestD3D9Readback() { return true; }
bool TestGLReadback() { return true; }
bool TestD3D11Readback() {
	printf("D3D11 readback test requires Windows.\n");
	return true;
}
#endif
