#pragma once

#include <vulkan/vulkan_core.h>

#include <vk_mem_alloc.h>

namespace graphics {

// Буфер в памяти, доступной и CPU, и GPU. Память сразу отображена (mapped),
// поэтому данные в буфер можно записывать обычным memcpy по указателю memory.
struct Buffer {
	VkBuffer buffer;
	VmaAllocation allocation;
	void* memory;
};

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& result);
void destroyBuffer(const Buffer& buffer);

VkShaderModule loadShaderModule(const char path[]);

} // namespace graphics
