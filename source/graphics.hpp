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

// Буфер в памяти видеокарты (VRAM). CPU в неё не пишет: данные сначала копируются
// в staging-буфер, а оттуда командой GPU vkCmdCopyBuffer в буфер в VRAM.
// У такого буфера memory = nullptr.
bool createDeviceBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& result);

// Текстура: изображение в VRAM и вид (image view), через который его читает шейдер
struct Texture {
	VkImage image;
	VmaAllocation allocation;
	VkImageView view;
};

// Загружает картинку из файла (stb_image) и копирует её в VRAM через staging-буфер
bool loadTexture(const char path[], Texture& result);
void destroyTexture(const Texture& texture);

// Изображение глубины для карты теней: в него рисуют (depth attachment)
// и из него потом читает шейдер (sampled)
bool createDepthTexture(uint32_t width, uint32_t height, VkFormat format, Texture& result);

VkShaderModule loadShaderModule(const char path[]);

} // namespace graphics
