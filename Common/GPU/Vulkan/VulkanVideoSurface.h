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

#pragma once

#include "Common/GPU/Vulkan/VulkanContext.h"
#include <formats/video_hw_frame.h>

class VulkanTexture;
struct FrameData;

// Prepared on the GPU owner thread, recorded by VulkanRenderManager.
class VulkanVideoSurface {
public:
	explicit VulkanVideoSurface(VulkanContext *context) : context_(context) {}
	~VulkanVideoSurface();
	bool Init();
	struct Task;
	Task *Prepare(const video_hw_frame_t &frame, VulkanTexture *destination, VulkanTexture *original);
	static void Record(void *task, VkCommandBuffer commandBuffer, FrameData &frameData);
	static void Dispose(void *task);

private:
	bool tried_ = false;
	VulkanContext *context_;
	VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
};
