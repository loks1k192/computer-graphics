#include "graphics.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "graphics_internal.hpp"

// Реализация stb_image подключается ровно в одном .cpp файле
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace graphics {

namespace {

VkCommandPool one_time_command_pool;

// Командный буфер для разовых команд копирования при загрузке ресурсов
VkCommandBuffer beginOneTimeCommands() {
	auto& context = internal::context;

	const VkCommandPoolCreateInfo command_pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = context.graphics_queue_index,
	};
	vkCreateCommandPool(context.device, &command_pool, nullptr, &one_time_command_pool);

	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = one_time_command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};

	VkCommandBuffer command_buffer;
	vkAllocateCommandBuffers(context.device, &allocate_info, &command_buffer);

	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(command_buffer, &begin_info);

	return command_buffer;
}

// Отправляет команды и ждёт, пока GPU их выполнит: после этого staging-буфер можно удалять
void endOneTimeCommands(VkCommandBuffer command_buffer) {
	auto& context = internal::context;

	vkEndCommandBuffer(command_buffer);

	const VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &command_buffer,
	};
	vkQueueSubmit(context.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
	vkQueueWaitIdle(context.graphics_queue);

	vkDestroyCommandPool(context.device, one_time_command_pool, nullptr);
}

} // namespace

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& result) {
	auto& context = internal::context;

	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	// MAPPED_BIT сразу отображает память буфера в адресное пространство CPU,
	// указатель на неё VMA возвращает в pMappedData (vmaMapMemory не нужен)
	const VmaAllocationCreateInfo allocation_create_info = {
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
		         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	VmaAllocationInfo allocation_info;

	if (vmaCreateBuffer(context.allocator, &buffer_info, &allocation_create_info,
	                    &result.buffer, &result.allocation, &allocation_info) != VK_SUCCESS) {
		std::cerr << "Failed to create and allocate buffer\n";
		return false;
	}

	result.memory = allocation_info.pMappedData;

	return true;
}

void destroyBuffer(const Buffer& buffer) {
	vmaDestroyBuffer(internal::context.allocator, buffer.buffer, buffer.allocation);
}

bool createDeviceBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& result) {
	auto& context = internal::context;

	// 1. staging-буфер в памяти, доступной CPU: туда данные кладутся обычным memcpy
	Buffer staging;
	if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
		return false;
	}
	memcpy(staging.memory, data, size);

	// 2. буфер в VRAM: TRANSFER_DST — в него можно копировать командой GPU
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	const VmaAllocationCreateInfo allocation_create_info = {
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
	};

	if (vmaCreateBuffer(context.allocator, &buffer_info, &allocation_create_info,
	                    &result.buffer, &result.allocation, nullptr) != VK_SUCCESS) {
		std::cerr << "Failed to create device buffer\n";
		destroyBuffer(staging);
		return false;
	}
	result.memory = nullptr;

	// 3. команда GPU на копирование staging -> VRAM
	VkCommandBuffer command_buffer = beginOneTimeCommands();
	const VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = size };
	vkCmdCopyBuffer(command_buffer, staging.buffer, result.buffer, 1, &region);
	endOneTimeCommands(command_buffer);

	destroyBuffer(staging);
	return true;
}

bool loadTexture(const char path[], Texture& result) {
	auto& context = internal::context;

	// stb_image декодирует PNG/JPEG/... в массив пикселей, STBI_rgb_alpha — всегда 4 канала
	int width, height, channels;
	stbi_uc* pixels = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
	if (pixels == nullptr) {
		std::cerr << "Failed to load texture " << path << '\n';
		return false;
	}

	const VkDeviceSize size = VkDeviceSize(width) * height * 4;

	Buffer staging;
	if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
		stbi_image_free(pixels);
		return false;
	}
	memcpy(staging.memory, pixels, size);
	stbi_image_free(pixels);

	// Формат SRGB: цвета в файле записаны в sRGB, при чтении в шейдере они переводятся
	// в линейное пространство, где и считается освещение
	const VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_SRGB,
		.extent = { uint32_t(width), uint32_t(height), 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	const VmaAllocationCreateInfo allocation_create_info = {
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
	};

	if (vmaCreateImage(context.allocator, &image_info, &allocation_create_info,
	                   &result.image, &result.allocation, nullptr) != VK_SUCCESS) {
		std::cerr << "Failed to create texture image\n";
		destroyBuffer(staging);
		return false;
	}

	const VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};

	VkCommandBuffer command_buffer = beginOneTimeCommands();

	// Барьер 1: раскладка UNDEFINED -> TRANSFER_DST_OPTIMAL, чтобы в изображение можно было копировать
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = result.image,
		.subresourceRange = subresource_range,
	};
	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &barrier);

	const VkBufferImageCopy region = {
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { uint32_t(width), uint32_t(height), 1 },
	};
	vkCmdCopyBufferToImage(command_buffer, staging.buffer, result.image,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// Барьер 2: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL, теперь изображение читает
	// фрагментный шейдер, и чтение начнётся только после окончания копирования
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &barrier);

	endOneTimeCommands(command_buffer);
	destroyBuffer(staging);

	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = result.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_SRGB,
		.subresourceRange = subresource_range,
	};

	if (vkCreateImageView(context.device, &view_info, nullptr, &result.view) != VK_SUCCESS) {
		std::cerr << "Failed to create texture image view\n";
		return false;
	}

	return true;
}

bool createDepthTexture(uint32_t width, uint32_t height, VkFormat format, Texture& result) {
	auto& context = internal::context;

	const VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	const VmaAllocationCreateInfo allocation_create_info = {
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
	};

	if (vmaCreateImage(context.allocator, &image_info, &allocation_create_info,
	                   &result.image, &result.allocation, nullptr) != VK_SUCCESS) {
		std::cerr << "Failed to create depth image\n";
		return false;
	}

	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = result.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	if (vkCreateImageView(context.device, &view_info, nullptr, &result.view) != VK_SUCCESS) {
		std::cerr << "Failed to create depth image view\n";
		return false;
	}

	return true;
}

void destroyTexture(const Texture& texture) {
	vkDestroyImageView(internal::context.device, texture.view, nullptr);
	vmaDestroyImage(internal::context.allocator, texture.image, texture.allocation);
}

VkShaderModule loadShaderModule(const char path[]) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		std::cerr << "Failed to open shader file " << path << '\n';
		return VK_NULL_HANDLE;
	}

	const size_t size = file.tellg();
	std::vector<uint32_t> buffer(size / sizeof(uint32_t));

	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	file.close();

	const VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = buffer.data(),
	};

	VkShaderModule result;
	if (vkCreateShaderModule(internal::context.device, &info, nullptr, &result) != VK_SUCCESS) {
		std::cerr << "Failed to create shader module from " << path << '\n';
		return VK_NULL_HANDLE;
	}

	return result;
}

} // namespace graphics
