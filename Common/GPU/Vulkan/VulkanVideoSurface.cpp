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

#include "Common/GPU/Vulkan/VulkanVideoSurface.h"
#include "Common/GPU/Vulkan/VulkanVideoSurfaceSpv.h"
#include "Common/GPU/Vulkan/VulkanFrameData.h"
#include "Common/GPU/Vulkan/VulkanImage.h"

#include <cstring>
#include <formats/image.h>

struct VulkanVideoSurface::Task {
	VulkanVideoSurface *converter;
	video_hw_frame_t frame;
	VkImage destination;
	VkImage original;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	bool recorded = false;
};

VulkanVideoSurface::~VulkanVideoSurface() {
	auto &deletes = context_->Delete();
	if (pipeline_) deletes.QueueDeletePipeline(pipeline_);
	if (pipelineLayout_) deletes.QueueDeletePipelineLayout(pipelineLayout_);
	if (descriptorLayout_) deletes.QueueDeleteDescriptorSetLayout(descriptorLayout_);
	if (sampler_) deletes.QueueDeleteSampler(sampler_);
}

bool VulkanVideoSurface::Init() {
	if (tried_) return pipeline_ && sampler_;
	tried_ = true;
	if (!(context_->GetQueueFamilyProperties(context_->GetGraphicsQueueFamilyIndex()).queueFlags & VK_QUEUE_COMPUTE_BIT))
		return false;
	const VkDevice device = context_->GetDevice();
	VkDescriptorSetLayoutBinding bindings[4]{};
	for (int i = 0; i < 4; ++i) {
		bindings[i].binding = i;
		bindings[i].descriptorType = i >= 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	descriptorInfo.bindingCount = 4;
	descriptorInfo.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(device, &descriptorInfo, nullptr, &descriptorLayout_) != VK_SUCCESS)
		return false;
	VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(int32_t)};
	VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &descriptorLayout_;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &range;
	if (vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout_) != VK_SUCCESS)
		return false;
	VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	moduleInfo.codeSize = sizeof(videoSurfaceSpv);
	moduleInfo.pCode = videoSurfaceSpv;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
		return false;
	VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pipeline.layout = pipelineLayout_;
	pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline.stage.module = module;
	pipeline.stage.pName = "main";
	VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
	vkDestroyShaderModule(device, module, nullptr);
	if (result != VK_SUCCESS)
		return false;
	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	return vkCreateSampler(device, &sampler, nullptr, &sampler_) == VK_SUCCESS;
}

VulkanVideoSurface::Task *VulkanVideoSurface::Prepare(const video_hw_frame_t &frame, VulkanTexture *destination, VulkanTexture *original) {
	if (frame.api != VIDEO_HW_API_VULKAN || frame.pixfmt != IMAGE_PIXFMT_NV12 || !destination || !original || !pipeline_ || !sampler_
		|| !frame.image || !frame.plane_views[0] || !frame.plane_views[1]
		|| !frame.wait || !frame.done || !frame.abandon || frame.array_slice
		|| frame.crop_x || frame.crop_y || !frame.width || !frame.height
		|| frame.width != (unsigned)destination->GetWidth() || frame.height != (unsigned)destination->GetHeight())
		return nullptr;
	auto *task = new Task{this, frame, destination->GetImage(), original->GetImage()};
	VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
	VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pool.maxSets = 1;
	pool.poolSizeCount = 2;
	pool.pPoolSizes = sizes;
	const VkDevice device = context_->GetDevice();
	if (vkCreateDescriptorPool(device, &pool, nullptr, &task->pool) != VK_SUCCESS) {
		delete task;
		return nullptr;
	}
	VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	allocate.descriptorPool = task->pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &descriptorLayout_;
	if (vkAllocateDescriptorSets(device, &allocate, &task->set) != VK_SUCCESS) {
		vkDestroyDescriptorPool(device, task->pool, nullptr);
		delete task;
		return nullptr;
	}
	VkDescriptorImageInfo images[4]{};
	VkWriteDescriptorSet writes[4]{};
	for (int i = 0; i < 4; ++i) {
		images[i].imageLayout = i >= 2 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (i < 2) {
			images[i].sampler = sampler_;
			memcpy(&images[i].imageView, &frame.plane_views[i], sizeof(VkImageView));
		} else {
			images[i].imageView = (i == 2 ? destination : original)->GetImageView();
		}
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = task->set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i >= 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
	return task;
}

void VulkanVideoSurface::Record(void *context, VkCommandBuffer command, FrameData &frameData) {
	auto *task = (Task *)context;
	if (task->recorded) return;
	auto *converter = task->converter;
	VkImageMemoryBarrier barriers[3]{};
	for (auto &barrier : barriers) {
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	}
	memcpy(&barriers[0].image, &task->frame.image, sizeof(VkImage));
	barriers[0].oldLayout = (VkImageLayout)task->frame.layout;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[1].image = task->destination;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[2] = barriers[1];
	barriers[2].image = task->original;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, converter->pipeline_);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, converter->pipelineLayout_, 0, 1, &task->set, 0, nullptr);
	const int32_t size[2]{(int32_t)task->frame.width, (int32_t)task->frame.height};
	vkCmdPushConstants(command, converter->pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
	vkCmdDispatch(command, (task->frame.width + 7) / 8, (task->frame.height + 7) / 8, 1);
	barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[0].dstAccessMask = 0;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[0].newLayout = (VkImageLayout)task->frame.layout;
	barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[2] = barriers[1];
	barriers[2].image = task->original;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);
	frameData.AddVideoLease(task->frame);
	task->recorded = true;
}

void VulkanVideoSurface::Dispose(void *context) {
	auto *task = (Task *)context;
	if (!task->recorded)
		video_hw_frame_abandon(&task->frame);
	task->converter->context_->Delete().QueueDeleteDescriptorPool(task->pool);
	delete task;
}
