// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include "ppsspp_config.h"
#include "unittest/UnitTest.h"
#include "Common/GPU/GraphicsContext.h"
#include "Common/System/Display.h"
#include "Common/GPU/thin3d_create.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanGraphicsContext.h"
#include "Common/GPU/Vulkan/VulkanPresentation.h"
#include "Common/GPU/Vulkan/VulkanImage.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/CoreParameter.h"
#include "Core/HW/VideoMemory.h"
#include "Core/HW/TiledVideoSurface.h"
#include "Core/HLE/sceVideocodec.h"
#include "Core/HLE/sceMpegbase.h"
#include "Core/HLE/MpegCscRow.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"
#include "GPU/Common/FramebufferManagerCommon.h"

#include "Common/Thread/ThreadManager.h"
#include <formats/image.h>
#include <memory>
#include <vector>

namespace {
VkDevice CreateDevice(VkPhysicalDevice gpu, void *, const VkDeviceCreateInfo *info) {
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	timeline.timelineSemaphore = VK_TRUE;
	timeline.pNext = const_cast<void *>(info->pNext);
	VkDeviceCreateInfo requested = *info;
	requested.pNext = &timeline;
	VkDevice device = VK_NULL_HANDLE;
	return vkCreateDevice(gpu, &requested, nullptr, &device) == VK_SUCCESS ? device : VK_NULL_HANDLE;
}
void Abandon(void *owner, uint64_t handle, uint64_t value) {
	auto *vulkan = (VulkanContext *)owner;
	auto signal = (PFN_vkSignalSemaphoreKHR)vkGetDeviceProcAddr(vulkan->GetDevice(), "vkSignalSemaphoreKHR");
	VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
	memcpy(&info.semaphore, &handle, sizeof(VkSemaphore));
	info.value = value;
	signal(vulkan->GetDevice(), &info);
}
struct Context : GraphicsContext {
	VulkanContext vulkan;
	Draw::DrawContext *draw = nullptr;
	VkSemaphore ready = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
	std::unique_ptr<VulkanTexture> y, uv;
	Draw::Texture *converted = nullptr;
	bool device = false, frame = false, ownsThreads = false;
	Context() {
		if (!g_threadManager.IsInitialized()) { g_threadManager.Init(2, 1); ownsThreads = true; }
	}
	void Resize() override {}
	void *GetAPIContext() override { return &vulkan; }
	Draw::DrawContext *GetDrawContext() override { return draw; }
	~Context() {
		if (frame) {
			ResolveAllVideoMemory();
			if (gpu) gpu->EndHostFrame();
			draw->EndFrame();
			draw->Present();
			auto *manager = (VulkanRenderManager *)draw->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
			manager->StopThreads();
		}
		if (gpu) GPU_Shutdown();
		if (draw) draw->HandleEvent(Draw::Event::LOST_BACKBUFFER, 64, 32);
		if (converted) converted->Release();
		y.reset(); uv.reset();
		delete draw;
		if (device) {
			vulkan.WaitUntilQueueIdle();
			if (ready) vkDestroySemaphore(vulkan.GetDevice(), ready, nullptr);
			if (done) vkDestroySemaphore(vulkan.GetDevice(), done, nullptr);
			if (vulkan.GetPresentation()) { vulkan.GetPresentation()->Destroy(&vulkan); vulkan.SetPresentation(nullptr); }
			vulkan.DestroyDevice();
		}
		vulkan.DestroyInstance();
		if (Memory::IsActive()) Memory::Shutdown();
		if (ownsThreads) g_threadManager.Teardown();
	}
};

bool Upload(Context &context, VulkanTexture *texture, int width, int height, VkFormat format,
	const std::vector<u8> &pixels, int bytesPerPixel) {
	auto *manager = (VulkanRenderManager *)context.draw->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	auto *push = (VulkanPushPool *)context.draw->GetNativeObject(Draw::NativeObject::PUSH_POOL);
	EXPECT_TRUE(texture->CreateDirect(width, height, 1, 1, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &manager->PostInitBarrier()));
	VkBuffer buffer;
	const u32 offset = (u32)push->Push(pixels.data(), (size_t)width * height * bytesPerPixel, 4, &buffer);
	const VkCommandBuffer command = manager->GetInitCmd();
	manager->PostInitBarrier().Flush(command);
	TextureCopyBatch copies;
	texture->CopyBufferToMipLevel(command, &copies, 0, width, height, 0, buffer, offset, width);
	texture->FinishCopyBatch(command, &copies);
	texture->EndCreate(command, false, VK_PIPELINE_STAGE_TRANSFER_BIT);
	return true;
}
}

bool TestVulkanVideoSurface() {
	EXPECT_TRUE(!gpu && !Memory::IsActive());
	std::string error;
	EXPECT_TRUE(VulkanLoad(&error));
	Context context;
	VulkanContext::CreateInfo info{"VideoSurfaceTest", 1, (VulkanInitFlags)0, ""};
	EXPECT_TRUE(context.vulkan.CreateInstance(info) == VK_SUCCESS);
	EXPECT_TRUE(context.vulkan.CreateDevice(context.vulkan.GetBestPhysicalDevice(),
		{VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME}, nullptr, CreateDevice) == VK_SUCCESS);
	context.device = true;
	EXPECT_TRUE(context.vulkan.ChooseGraphicsQueueWithoutSurface());
	SetGPUBackend(GPUBackend::VULKAN);
	g_Config.iInflightFrames = 2;
	PSP_CoreParameter().gpuCore = GPUCORE_VULKAN;
	EXPECT_TRUE(CreateVulkanOffscreenPresentation(&context.vulkan, 64, 32));
	context.draw = Draw::T3DCreateVulkanContext(&context.vulkan, true);
	EXPECT_TRUE(context.draw && context.draw->CreatePresets());
	context.draw->HandleEvent(Draw::Event::GOT_BACKBUFFER, 64, 32);
	context.draw->BeginFrame(Draw::DebugFlags::NONE);
	context.frame = true;
	const int width = 34, height = 18, stride = 40;
	std::vector<u8> y(width * height), uv(width * height / 2);
	for (size_t i = 0; i < y.size(); ++i) y[i] = (u8)(i * 13);
	for (size_t i = 0; i < uv.size(); ++i) uv[i] = (u8)(i * 31);
	context.y.reset(new VulkanTexture(&context.vulkan, "Video luma test"));
	context.uv.reset(new VulkanTexture(&context.vulkan, "Video chroma test"));
	EXPECT_TRUE(Upload(context, context.y.get(), width, height, VK_FORMAT_R8_UNORM, y, 1));
	EXPECT_TRUE(Upload(context, context.uv.get(), width / 2, height / 2, VK_FORMAT_R8G8_UNORM, uv, 2));
	auto *manager = (VulkanRenderManager *)context.draw->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	VkMemoryBarrier inputBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	inputBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	inputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(manager->GetInitCmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &inputBarrier, 0, nullptr, 0, nullptr);
	VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	semaphore.pNext = &type;
	EXPECT_TRUE(vkCreateSemaphore(context.vulkan.GetDevice(), &semaphore, nullptr, &context.ready) == VK_SUCCESS);
	EXPECT_TRUE(vkCreateSemaphore(context.vulkan.GetDevice(), &semaphore, nullptr, &context.done) == VK_SUCCESS);
	video_hw_frame_t frame{};
	const VkImage image = context.y->GetImage();
	const VkImageView yview = context.y->GetImageView(), uvview = context.uv->GetImageView();
	memcpy(&frame.image, &image, sizeof(image));
	memcpy(&frame.plane_views[0], &yview, sizeof(yview));
	memcpy(&frame.plane_views[1], &uvview, sizeof(uvview));
	memcpy(&frame.wait, &context.ready, sizeof(context.ready));
	memcpy(&frame.done, &context.done, sizeof(context.done));
	frame.width = width; frame.height = height;
	frame.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	frame.api = VIDEO_HW_API_VULKAN;
	frame.pixfmt = IMAGE_PIXFMT_NV12;
	frame.done_value = 1; frame.abandon = Abandon; frame.owner = &context.vulkan;
	context.converted = context.draw->ImportVideoFrame(frame);
	EXPECT_TRUE(context.converted);
	std::vector<u8> output(width * height * 4), expected(output.size());
	EXPECT_TRUE(context.draw->ReadVideoTexture(context.converted, output.data(), width));
	for (int row = 0; row < height; ++row) {
		for (int x = 0; x < width; ++x) {
			const int yy = y[row * width + x] - 16;
			const int cb = uv[(row / 2) * width + (x / 2) * 2] - 128;
			const int cr = uv[(row / 2) * width + (x / 2) * 2 + 1] - 128;
			const int c = 298 * yy + 128;
			u8 *pixel = &expected[(row * width + x) * 4];
			pixel[0] = (u8)std::clamp((c + 409 * cr) >> 8, 0, 255);
			pixel[1] = (u8)std::clamp((c - 100 * cb - 208 * cr) >> 8, 0, 255);
			pixel[2] = (u8)std::clamp((c + 516 * cb) >> 8, 0, 255);
		}
	}
	EXPECT_EQ_MEM(output.data(), expected.data(), expected.size());
	std::vector<u8> original(width * height * 4);
	EXPECT_TRUE(context.draw->ReadVideoPlanes(context.converted, original.data(), width));
	for (int row = 0; row < height; ++row) for (int x = 0; x < width; ++x) {
		EXPECT_EQ_HEX(original[(row * width + x) * 4], y[row * width + x]);
		EXPECT_EQ_HEX(original[(row * width + x) * 4 + 1], uv[(row / 2) * width + (x / 2) * 2]);
		EXPECT_EQ_HEX(original[(row * width + x) * 4 + 2], uv[(row / 2) * width + (x / 2) * 2 + 1]);
	}
	auto counter = (PFN_vkGetSemaphoreCounterValueKHR)vkGetDeviceProcAddr(context.vulkan.GetDevice(), "vkGetSemaphoreCounterValueKHR");
	uint64_t done;
	EXPECT_TRUE(counter && counter(context.vulkan.GetDevice(), context.done, &done) == VK_SUCCESS && done == 1);

	Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE;
	PSP_CoreParameter().cpuCore = CPUCore::INTERPRETER;
	EXPECT_TRUE(Memory::Init(Memory::MemMapSetupFlags::Default));
	if (CanDeferVideoMemory()) {
		g_Config.iInternalResolution = 1;
		g_Config.bSkipBufferEffects = false;
		PSP_CoreParameter().renderWidth = 480;
		PSP_CoreParameter().renderHeight = 272;
		PSP_CoreParameter().renderScaleFactor = 1;
		EXPECT_TRUE(GPU_Init(GPUCORE_VULKAN, &context, context.draw));
		gpu->FinishInitOnMainThread();
		gpu->BeginHostFrame(g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation()));
		EXPECT_TRUE(gpu->GetFramebufferManagerCommon()->UseBufferedRendering());
		const u32 address = 0x08810080, size = stride * height * 4;
		memset(Memory::GetPointerWriteUnchecked(address), 0xA5, size);
		gpuStats.ResetFrame();
		EXPECT_TRUE(gpu->WriteVideoTexture(address, stride, width, height, context.converted));
		EXPECT_TRUE(HasVideoMemory(address, size));
		EXPECT_TRUE(gpu->WriteVideoTexture(address, stride, width, height, context.converted));
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 0);
		const u32 first = Memory::ReadUnchecked_U32(address | 0x40000000);
		EXPECT_EQ_MEM((const char *)&first, expected.data(), 4);
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 1);
		for (int row = 0; row < height; ++row) {
			EXPECT_EQ_MEM(Memory::GetPointerUnchecked(address + row * stride * 4), expected.data() + row * width * 4, width * 4);
			for (int x = width; x < stride; ++x)
				EXPECT_EQ_HEX(Memory::ReadUnchecked_U32(address + (row * stride + x) * 4), 0xA5A5A5A5);
		}
		EXPECT_TRUE(gpu->WriteVideoTexture(address, stride, width, height, context.converted));
		Memory::WriteUnchecked_U32(0x00123456, address | 0xC0000000);
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 2);
		auto *framebuffers = gpu->GetFramebufferManagerCommon();
		const VirtualFramebuffer *video = nullptr;
		for (auto *vfb : framebuffers->GetFramebufferList()) if (vfb->fb_address == address) video = vfb;
		EXPECT_TRUE(video);
		framebuffers->EnsureVideoCpuCurrent(const_cast<VirtualFramebuffer *>(video));
		std::vector<u32> changed(stride * height);
		EXPECT_TRUE(context.draw->CopyFramebufferToMemory(video->fbo, Draw::Aspect::COLOR_BIT, 0, 0, video->width, height,
			Draw::DataFormat::R8G8B8A8_UNORM, changed.data(), stride, Draw::ReadbackMode::BLOCK, "Video CPU write test"));
		EXPECT_EQ_HEX(changed[0], 0x00123456);
		EXPECT_TRUE(gpu->WriteVideoTexture(address, stride, width, height, context.converted));
		EXPECT_TRUE(ResolveAllVideoMemory());
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 3);
		const u32 vram = 0x04010080;
		memset(Memory::GetPointerWriteUnchecked(vram), 0xA5, size);
		EXPECT_TRUE(gpu->WriteVideoTexture(vram, stride, width, height, context.converted));
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 3);
		// A generated JIT load bypasses Memory helpers. Exercise the real fault path.
		const u32 raw = *(const u32 *)(Memory::base + vram + 0x40400000);
		EXPECT_TRUE(!HasVideoMemory(vram, size));
		EXPECT_EQ_MEM((const char *)&raw, expected.data(), 4);
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 4);
		EXPECT_TRUE(gpu->WriteVideoTexture(vram, stride, width, height, context.converted));
		gpu->EndHostFrame();
		context.draw->EndFrame();
		context.draw->Present();
		context.frame = false;
		// Savestate serialization and CPU access can happen between frames.
		EXPECT_TRUE(ResolveAllVideoMemory());
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 5);
		EXPECT_EQ_MEM(Memory::GetPointerUnchecked(vram), expected.data(), width * 4);
		context.draw->BeginFrame(Draw::DebugFlags::NONE);
		context.frame = true;
		gpu->BeginHostFrame(g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation()));
		gpuStats.ResetFrame();
		frame.width = 32; frame.height = 16; frame.done_value = 2;
		Draw::Texture *tiled = context.draw->ImportVideoFrame(frame);
		EXPECT_TRUE(tiled);
		u32 offsets[8], buffers[8];
		const u32 bytes = VideocodecFrameBufferLayout(32, 16, nullptr, offsets);
		for (int i = 0; i < 8; ++i) buffers[i] = 0x08900000 + offsets[i];
		memset(Memory::GetPointerWriteUnchecked(buffers[0]), 0xA5, bytes);
		EXPECT_TRUE(RememberTiledVideoSurface(tiled, buffers, 32, 16, false));
		EXPECT_TRUE(RememberTiledVideoSurface(tiled, buffers, 32, 16, false));
		EXPECT_TRUE(FindTiledVideoSurface(buffers, 32, 16) == tiled);
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 0);
		// One CPU component read must make all eight components coherent together.
		const u8 *luma, *cb, *cr;
		EXPECT_TRUE(ReadTiledYCbCr(buffers, 32, 16, &luma, &cb, &cr));
		for (int row = 0; row < 16; ++row) EXPECT_EQ_MEM(luma + row * 32, y.data() + row * width, 32);
		for (int row = 0; row < 8; ++row) for (int x = 0; x < 16; ++x) {
			EXPECT_EQ_HEX(cb[row * 16 + x], uv[row * width + x * 2]);
			EXPECT_EQ_HEX(cr[row * 16 + x], uv[row * width + x * 2 + 1]);
		}
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 1);
		EXPECT_TRUE(!FindTiledVideoSurface(buffers, 32, 16));
		EXPECT_TRUE(RememberTiledVideoSurface(tiled, buffers, 32, 16, false));
		Memory::WriteUnchecked_U8(0x69, buffers[7] | 0x40000000);
		EXPECT_EQ_INT(gpuStats.perFrame.numVideoReadbacks, 2);
		EXPECT_EQ_HEX(Memory::ReadUnchecked_U8(buffers[7]), 0x69);
		EXPECT_EQ_HEX(Memory::ReadUnchecked_U8(buffers[0]), y[0]);
		ClearTiledVideoSurfaces();
		tiled->Release();
		printf("Video surfaces: GPU replacements avoid readback; RGB/YUV RAM aliases, CPU writes, padding and between-frame materialization pass\n");
	}
	return true;
}
