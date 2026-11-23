#pragma once

#include "MiniVulkan.h"

struct texture_object {
	VkImage image;
	VkImageLayout imageLayout;

	VkDeviceMemory mem;
	VkImageView view;
	int32_t tex_width, tex_height;
};

typedef struct {
	struct texture_object texture;
	VkDeviceSize deviceSize;

	uint8_t *conversionBuffer;
	int stride;

	int depth;               // > 1 or is_3d for 3D textures
	bool is_3d;
	VkFormat format;
	VkImageView uint_view;   // R32_UINT view of 32-bit images (GL-style r32ui image access), else VK_NULL_HANDLE
	bool storage;            // created with VK_IMAGE_USAGE_STORAGE_BIT
} Texture5Impl;
