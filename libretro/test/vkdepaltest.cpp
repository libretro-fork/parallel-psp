// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
// Offscreen Vulkan executor for Tools/TestVulkanDepal.py. Runs only in tests.
// Samples a deterministic 32x32 source and 512-entry palette, comparing the
// legacy and precompiled shader equations through compute wrappers. This
// tests conversion math, not the graphics pipeline or PSP game presentation.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#define VK(call)                                                                                                       \
	do {                                                                                                               \
		VkResult r = (call);                                                                                           \
		if (r != VK_SUCCESS)                                                                                           \
			throw std::runtime_error(std::string(#call) + ": " + std::to_string(r));                                   \
	} while (0)
struct Buffer {
	VkBuffer handle{};
	VkDeviceMemory memory{};
	void *mapped{};
	VkDeviceSize size{};
};
struct Image {
	VkImage handle{};
	VkDeviceMemory memory{};
	VkImageView view{};
};
static VkDevice device;
static VkPhysicalDevice gpu;
uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
	VkPhysicalDeviceMemoryProperties p;
	vkGetPhysicalDeviceMemoryProperties(gpu, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	throw std::runtime_error("memory type");
}
Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
	Buffer b{};
	b.size = size;
	VkBufferCreateInfo c{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	c.size = size;
	c.usage = usage;
	c.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK(vkCreateBuffer(device, &c, nullptr, &b.handle));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, b.handle, &req);
	VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	a.allocationSize = req.size;
	a.memoryTypeIndex =
		MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK(vkAllocateMemory(device, &a, nullptr, &b.memory));
	VK(vkBindBufferMemory(device, b.handle, b.memory, 0));
	VK(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
	return b;
}
Image MakeImage(uint32_t w, uint32_t h, bool array) {
	Image image{};
	VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	c.imageType = VK_IMAGE_TYPE_2D;
	c.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	c.extent = {w, h, 1};
	c.mipLevels = 1;
	c.arrayLayers = 1;
	c.samples = VK_SAMPLE_COUNT_1_BIT;
	c.tiling = VK_IMAGE_TILING_OPTIMAL;
	c.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	c.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK(vkCreateImage(device, &c, nullptr, &image.handle));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, image.handle, &req);
	VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	a.allocationSize = req.size;
	a.memoryTypeIndex = MemoryType(req.memoryTypeBits, 0);
	VK(vkAllocateMemory(device, &a, nullptr, &image.memory));
	VK(vkBindImageMemory(device, image.handle, image.memory, 0));
	VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	v.image = image.handle;
	v.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
	v.format = c.format;
	v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VK(vkCreateImageView(device, &v, nullptr, &image.view));
	return image;
}
std::vector<uint32_t> Read(const std::string &name) {
	std::ifstream f(name, std::ios::binary | std::ios::ate);
	if (!f)
		throw std::runtime_error(name);
	size_t size = f.tellg();
	if (size % 4 || size < 20)
		throw std::runtime_error("shader size");
	std::vector<uint32_t> result(size / 4);
	f.seekg(0);
	f.read((char *)result.data(), size);
	return result;
}
int main(int argc, char **argv) {
	try {
		if (argc != 4)
			return 2;
		std::string root = argv[1];
		int count = std::atoi(argv[2]);
		int linear = std::atoi(argv[3]);
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "palette shader differential test";
		app.apiVersion = VK_API_VERSION_1_0;
		VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ic.pApplicationInfo = &app;
		VkInstance instance;
		VK(vkCreateInstance(&ic, nullptr, &instance));
		uint32_t n = 1;
		VK(vkEnumeratePhysicalDevices(instance, &n, &gpu));
		uint32_t family = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
		std::vector<VkQueueFamilyProperties> properties(n);
		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, properties.data());
		while (family < n && !(properties[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
			++family;
		if (family == n)
			throw std::runtime_error("queue");
		float priority = 1;
		VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qc.queueFamilyIndex = family;
		qc.queueCount = 1;
		qc.pQueuePriorities = &priority;
		VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dc.queueCreateInfoCount = 1;
		dc.pQueueCreateInfos = &qc;
		VK(vkCreateDevice(gpu, &dc, nullptr, &device));
		VkQueue queue;
		vkGetDeviceQueue(device, family, 0, &queue);
		VkPhysicalDeviceProperties devProps;
		vkGetPhysicalDeviceProperties(gpu, &devProps);
		printf("Device: %s; filter: %s\n", devProps.deviceName, linear ? "linear" : "nearest");
		VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pc.queueFamilyIndex = family;
		pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkCommandPool pool;
		VK(vkCreateCommandPool(device, &pc, nullptr, &pool));
		VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ca.commandPool = pool;
		ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ca.commandBufferCount = 1;
		VkCommandBuffer cmd;
		VK(vkAllocateCommandBuffers(device, &ca, &cmd));
		auto begin = [&]() {
			VK(vkResetCommandBuffer(cmd, 0));
			VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			VK(vkBeginCommandBuffer(cmd, &b));
		};
		auto submit = [&]() {
			VK(vkEndCommandBuffer(cmd));
			VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			s.commandBufferCount = 1;
			s.pCommandBuffers = &cmd;
			VK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE));
			VK(vkQueueWaitIdle(queue));
		};
		Buffer ubo = MakeBuffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
			   output = MakeBuffer(32 * 32 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
			   staging = MakeBuffer((32 * 32 + 512) * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		memset(ubo.mapped, 0, 64);
		float uniforms[] = {32, 32, 1};
		memcpy(ubo.mapped, uniforms, sizeof(uniforms));
		auto *pixels = (float *)staging.mapped;
		uint32_t random = 42;
		for (int i = 0; i < 32 * 32 * 4; i++) {
			random = random * 1664525u + 1013904223u;
			pixels[i] = (random >> 16) / 65535.0f;
			if (i % 4 == 3 && (i / 4) % 5 == 0)
				pixels[i] = 1.0f;
		}
		for (int i = 0; i < 512; i++) {
			float *p = pixels + 32 * 32 * 4 + i * 4;
			p[0] = (i & 31) / 31.0f;
			p[1] = (i >> 5) / 15.0f;
			p[2] = ((i * 37) & 255) / 255.0f;
			p[3] = 1;
		}
		Image source = MakeImage(32, 32, true), palette = MakeImage(512, 1, false);
		begin();
		Image *images[] = {&source, &palette};
		for (auto *image : images) {
			VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = image->handle;
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
								 0, nullptr, 1, &b);
		}
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {32, 32, 1};
		vkCmdCopyBufferToImage(cmd, staging.handle, source.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		copy.bufferOffset = 32 * 32 * 16;
		copy.imageExtent = {512, 1, 1};
		vkCmdCopyBufferToImage(cmd, staging.handle, palette.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		for (auto *image : images) {
			VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = image->handle;
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
								 nullptr, 0, nullptr, 1, &b);
		}
		submit();
		VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sc.magFilter = sc.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sc.maxLod = 0;
		VkSampler sampler;
		VK(vkCreateSampler(device, &sc, nullptr, &sampler));
		VkDescriptorSetLayoutBinding bindings[4] = {
			{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
		VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lc.bindingCount = 4;
		lc.pBindings = bindings;
		VkDescriptorSetLayout layout;
		VK(vkCreateDescriptorSetLayout(device, &lc, nullptr, &layout));
		VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
										{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
										{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
		VkDescriptorPoolCreateInfo dpc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpc.maxSets = 1;
		dpc.poolSizeCount = 3;
		dpc.pPoolSizes = sizes;
		VkDescriptorPool descPool;
		VK(vkCreateDescriptorPool(device, &dpc, nullptr, &descPool));
		VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		da.descriptorPool = descPool;
		da.descriptorSetCount = 1;
		da.pSetLayouts = &layout;
		VkDescriptorSet set;
		VK(vkAllocateDescriptorSets(device, &da, &set));
		VkDescriptorBufferInfo bi0{ubo.handle, 0, 64}, bi3{output.handle, 0, output.size};
		VkDescriptorImageInfo ii1{sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			ii2{sampler, palette.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet writes[4]{};
		for (int i = 0; i < 4; i++) {
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = bindings[i].descriptorType;
		}
		writes[0].pBufferInfo = &bi0;
		writes[1].pImageInfo = &ii1;
		writes[2].pImageInfo = &ii2;
		writes[3].pBufferInfo = &bi3;
		vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
		VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plc.setLayoutCount = 1;
		plc.pSetLayouts = &layout;
		VkPipelineLayout pipelineLayout;
		VK(vkCreatePipelineLayout(device, &plc, nullptr, &pipelineLayout));
		auto run = [&](const std::string &file) {
			auto words = Read(file);
			VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			mc.codeSize = words.size() * 4;
			mc.pCode = words.data();
			VkShaderModule module;
			VK(vkCreateShaderModule(device, &mc, nullptr, &module));
			VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			pipelineInfo.stage.module = module;
			pipelineInfo.stage.pName = "main";
			pipelineInfo.layout = pipelineLayout;
			VkPipeline pipeline;
			VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
			begin();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
			vkCmdDispatch(cmd, 4, 4, 1);
			VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0,
								 nullptr, 0, nullptr);
			submit();
			std::vector<float> result(32 * 32 * 4);
			memcpy(result.data(), output.mapped, output.size);
			vkDestroyPipeline(device, pipeline, nullptr);
			vkDestroyShaderModule(device, module, nullptr);
			return result;
		};
		int bad = 0, exact = 0;
		float maxError = 0;
		for (int i = 0; i < count; i++) {
			auto ref = run(root + "/case_" + std::to_string(i) + "_ref.spv");
			auto test = run(root + "/case_" + std::to_string(i) + "_new.spv");
			float error = 0;
			for (size_t j = 0; j < ref.size(); j++) {
				if (!std::isfinite(ref[j]) || !std::isfinite(test[j])) {
					error = 1000;
					break;
				}
				error = std::max(error, std::fabs(ref[j] - test[j]));
			}
			if (error > 2e-6f) {
				printf("FAIL case %d max error %.9f\n", i, error);
				bad++;
			}
			if (memcmp(ref.data(), test.data(), output.size) == 0)
				exact++;
			maxError = std::max(error, maxError);
		}
		printf("%s: %d cases, %d bit-exact, %d failures, max error %.9f\n", bad ? "FAIL" : "PASS", count, exact, bad,
			   maxError);
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorPool(device, descPool, nullptr);
		vkDestroyDescriptorSetLayout(device, layout, nullptr);
		vkDestroySampler(device, sampler, nullptr);
		for (auto *i : images) {
			vkDestroyImageView(device, i->view, nullptr);
			vkDestroyImage(device, i->handle, nullptr);
			vkFreeMemory(device, i->memory, nullptr);
		}
		for (auto *b : {&ubo, &output, &staging}) {
			vkUnmapMemory(device, b->memory);
			vkDestroyBuffer(device, b->handle, nullptr);
			vkFreeMemory(device, b->memory, nullptr);
		}
		vkDestroyCommandPool(device, pool, nullptr);
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
		return bad ? 1 : 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "ERROR %s\n", e.what());
		return 2;
	}
}
