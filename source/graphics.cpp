#include "graphics.hpp"

#include <fstream>
#include <iostream>
#include <vector>

#include "graphics_internal.hpp"

namespace graphics {

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
