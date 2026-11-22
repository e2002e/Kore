#include "vulkan.h"

#include "rendertarget.h"

#include <kinc/graphics5/rendertarget.h>
#include <kinc/graphics5/texture.h>
#include <kinc/log.h>

extern uint32_t swapchainImageCount;
extern kinc_g5_texture_t *vulkanTextures[16];
extern kinc_g5_render_target_t *vulkanRenderTargets[16];

bool memory_type_from_properties(uint32_t typeBits, VkFlags requirements_mask, uint32_t *typeIndex);
void setup_init_cmd();

/*static VkFormat convert_format(kinc_g5_render_target_format_t format) {
    switch (format) {
    case KINC_G5_RENDER_TARGET_FORMAT_128BIT_FLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case KINC_G5_RENDER_TARGET_FORMAT_64BIT_FLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case KINC_G5_RENDER_TARGET_FORMAT_32BIT_RED_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case KINC_G5_RENDER_TARGET_FORMAT_16BIT_RED_FLOAT:
        return VK_FORMAT_R16_SFLOAT;
    case KINC_G5_RENDER_TARGET_FORMAT_8BIT_RED:
        return VK_FORMAT_R8_UNORM;
    case KINC_G5_RENDER_TARGET_FORMAT_32BIT:
    default:
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
}*/

void setImageLayout(VkCommandBuffer _buffer, VkImage image, VkImageAspectFlags aspectMask, VkImageLayout oldImageLayout, VkImageLayout newImageLayout) {
	VkImageMemoryBarrier imageMemoryBarrier = {0};
	imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imageMemoryBarrier.pNext = NULL;
	imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageMemoryBarrier.oldLayout = oldImageLayout;
	imageMemoryBarrier.newLayout = newImageLayout;
	imageMemoryBarrier.image = image;
	imageMemoryBarrier.subresourceRange.aspectMask = aspectMask;
	imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
	imageMemoryBarrier.subresourceRange.levelCount = 1;
	imageMemoryBarrier.subresourceRange.layerCount = 1;

	if (oldImageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		imageMemoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	}
	if (oldImageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		imageMemoryBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	if (oldImageLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
		imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	}
	if (oldImageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		imageMemoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	}
	if (newImageLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	}
	if (newImageLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
		if (oldImageLayout != VK_IMAGE_LAYOUT_UNDEFINED)
			imageMemoryBarrier.srcAccessMask = imageMemoryBarrier.srcAccessMask | VK_ACCESS_TRANSFER_READ_BIT;
		imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	}
	if (newImageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		if (oldImageLayout != VK_IMAGE_LAYOUT_UNDEFINED)
			imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	}
	if (newImageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		imageMemoryBarrier.dstAccessMask = imageMemoryBarrier.dstAccessMask | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	if (newImageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		if (oldImageLayout != VK_IMAGE_LAYOUT_UNDEFINED)
			imageMemoryBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		imageMemoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	}

	VkPipelineStageFlags srcStageFlags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkPipelineStageFlags dstStageFlags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	vkCmdPipelineBarrier(_buffer, srcStageFlags, dstStageFlags, 0, 0, NULL, 0, NULL, 1, &imageMemoryBarrier);
}

static void render_target_init(kinc_g5_render_target_t *target, int width, int height, kinc_g5_render_target_format_t format, int depthBufferBits,
                               int stencilBufferBits, int samples_per_pixel, int framebuffer_index) {
	target->width = width;
	target->height = height;
	target->framebuffer_index = framebuffer_index;
	target->texWidth = width;
	target->texHeight = height;
	target->impl.format = convert_format(format);
	target->impl.depthBufferBits = depthBufferBits;
	target->impl.stage = 0;
	target->impl.stage_depth = -1;
	target->impl.readbackBufferCreated = false;
	target->impl.depth_borrowed = false;
	target->impl.is_cube = false;
	target->impl.current_face = 0;
	target->impl.depth_only = framebuffer_index < 0 && format == KINC_G5_RENDER_TARGET_FORMAT_16BIT_DEPTH;
	if (target->impl.depth_only) {
		target->impl.format = KINC_VK_RENDER_TARGET_DEPTH_FORMAT;
		if (depthBufferBits <= 0) {
			target->impl.depthBufferBits = depthBufferBits = 16;
		}
	}
	target->impl.depth_format = target->impl.depth_only ? KINC_VK_RENDER_TARGET_DEPTH_FORMAT : kinc_vulkan_depth_format(depthBufferBits);

	if (framebuffer_index < 0) {
		if (!target->impl.depth_only) {
			VkFormatProperties formatProperties;
			VkResult err;

			vkGetPhysicalDeviceFormatProperties(vk_ctx.gpu, target->impl.format, &formatProperties);
			assert(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT);

			VkImageCreateInfo image = {0};
			image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			image.pNext = NULL;
			image.imageType = VK_IMAGE_TYPE_2D;
			image.format = target->impl.format;
			image.extent.width = width;
			image.extent.height = height;
			image.extent.depth = 1;
			image.mipLevels = 1;
			image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT;
			image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			image.flags = 0;

			VkImageViewCreateInfo colorImageView = {0};
			colorImageView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			colorImageView.pNext = NULL;
			colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			colorImageView.format = target->impl.format;
			colorImageView.flags = 0;
			colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			colorImageView.subresourceRange.baseMipLevel = 0;
			colorImageView.subresourceRange.levelCount = 1;
			colorImageView.subresourceRange.baseArrayLayer = 0;
			colorImageView.subresourceRange.layerCount = 1;

			err = vkCreateImage(vk_ctx.device, &image, NULL, &target->impl.sourceImage);
			assert(!err);

			VkMemoryRequirements memoryRequirements;
			vkGetImageMemoryRequirements(vk_ctx.device, target->impl.sourceImage, &memoryRequirements);

			VkMemoryAllocateInfo allocationInfo = {0};
			allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocationInfo.pNext = NULL;
			allocationInfo.memoryTypeIndex = 0;
			allocationInfo.allocationSize = memoryRequirements.size;
			bool pass = memory_type_from_properties(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocationInfo.memoryTypeIndex);
			assert(pass);

			err = vkAllocateMemory(vk_ctx.device, &allocationInfo, NULL, &target->impl.sourceMemory);
			assert(!err);

			err = vkBindImageMemory(vk_ctx.device, target->impl.sourceImage, target->impl.sourceMemory, 0);
			assert(!err);

			setup_init_cmd();
			// Render targets stay in GENERAL for their whole life (attachment and sampling), see rtcache.c.h
			setImageLayout(vk_ctx.setup_cmd, target->impl.sourceImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			flush_init_cmd();

			colorImageView.image = target->impl.sourceImage;
			err = vkCreateImageView(vk_ctx.device, &colorImageView, NULL, &target->impl.sourceView);
			assert(!err);
		}

		if (depthBufferBits > 0) {
			const VkFormat depth_format = target->impl.depth_format;
			VkImageCreateInfo image = {0};
			image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			image.pNext = NULL;
			image.imageType = VK_IMAGE_TYPE_2D;
			image.format = depth_format;
			image.extent.width = width;
			image.extent.height = height;
			image.extent.depth = 1;
			image.mipLevels = 1;
			image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT;
			image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			image.flags = 0;

			VkResult err = vkCreateImage(vk_ctx.device, &image, NULL, &target->impl.depthImage);
			assert(!err);

			VkMemoryAllocateInfo mem_alloc = {0};
			mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			mem_alloc.pNext = NULL;
			mem_alloc.allocationSize = 0;
			mem_alloc.memoryTypeIndex = 0;

			VkImageViewCreateInfo view = {0};
			view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			view.pNext = NULL;
			view.image = target->impl.depthImage;
			view.format = depth_format;
			view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			view.subresourceRange.baseMipLevel = 0;
			view.subresourceRange.levelCount = 1;
			view.subresourceRange.baseArrayLayer = 0;
			view.subresourceRange.layerCount = 1;
			view.flags = 0;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;

			VkMemoryRequirements mem_reqs = {0};
			bool pass;

			/* get memory requirements for this object */
			vkGetImageMemoryRequirements(vk_ctx.device, target->impl.depthImage, &mem_reqs);

			/* select memory size and type */
			mem_alloc.allocationSize = mem_reqs.size;
			pass = memory_type_from_properties(mem_reqs.memoryTypeBits, 0, /* No requirements */ &mem_alloc.memoryTypeIndex);
			assert(pass);

			/* allocate memory */
			err = vkAllocateMemory(vk_ctx.device, &mem_alloc, NULL, &target->impl.depthMemory);
			assert(!err);

			/* bind memory */
			err = vkBindImageMemory(vk_ctx.device, target->impl.depthImage, target->impl.depthMemory, 0);
			assert(!err);

			setup_init_cmd();
			setImageLayout(vk_ctx.setup_cmd, target->impl.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			flush_init_cmd();

			/* create image view */
			err = vkCreateImageView(vk_ctx.device, &view, NULL, &target->impl.depthView);
			assert(!err);
		}

		if (target->impl.depth_only) {
			target->impl.sourceImage = VK_NULL_HANDLE;
			target->impl.sourceMemory = VK_NULL_HANDLE;
			target->impl.sourceView = target->impl.depthView;
		}
		// Framebuffers are created on demand per render pass (get_rt_framebuffer)
		target->impl.framebuffer = VK_NULL_HANDLE;
	}
}

void kinc_g5_render_target_init_with_multisampling(kinc_g5_render_target_t *target, int width, int height, kinc_g5_render_target_format_t format,
                                                   int depthBufferBits, int stencilBufferBits, int samples_per_pixel) {
	render_target_init(target, width, height, format, depthBufferBits, stencilBufferBits, samples_per_pixel, -1);
}

static int framebuffer_count = 0;

void kinc_g5_render_target_init_framebuffer_with_multisampling(kinc_g5_render_target_t *target, int width, int height, kinc_g5_render_target_format_t format,
                                                               int depthBufferBits, int stencilBufferBits, int samples_per_pixel) {
	render_target_init(target, width, height, format, depthBufferBits, stencilBufferBits, samples_per_pixel, framebuffer_count);
	framebuffer_count += 1;
}

static void create_cube_image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, int size, VkImage *image, VkDeviceMemory *memory,
                              VkImageView *cube_view, VkImageView *face_views) {
	VkImageCreateInfo info = {0};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = size;
	info.extent.height = size;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 6;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	VkResult err = vkCreateImage(vk_ctx.device, &info, NULL, image);
	assert(!err);

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(vk_ctx.device, *image, &reqs);
	VkMemoryAllocateInfo alloc = {0};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	bool pass = memory_type_from_properties(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc.memoryTypeIndex);
	assert(pass);
	err = vkAllocateMemory(vk_ctx.device, &alloc, NULL, memory);
	assert(!err);
	err = vkBindImageMemory(vk_ctx.device, *image, *memory, 0);
	assert(!err);

	// All 6 layers to GENERAL, like every other render target
	setup_init_cmd();
	VkImageMemoryBarrier barrier = {0};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = *image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 6;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(vk_ctx.setup_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	flush_init_cmd();

	VkImageViewCreateInfo view = {0};
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = *image;
	view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	view.format = format;
	view.subresourceRange.aspectMask = aspect;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 6;
	err = vkCreateImageView(vk_ctx.device, &view, NULL, cube_view);
	assert(!err);

	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.subresourceRange.layerCount = 1;
	for (int face = 0; face < 6; ++face) {
		view.subresourceRange.baseArrayLayer = face;
		err = vkCreateImageView(vk_ctx.device, &view, NULL, &face_views[face]);
		assert(!err);
	}
}

void kinc_g5_render_target_init_cube_with_multisampling(kinc_g5_render_target_t *target, int cubeMapSize, kinc_g5_render_target_format_t format,
                                                        int depthBufferBits, int stencilBufferBits, int samples_per_pixel) {
	memset(&target->impl, 0, sizeof(target->impl));
	target->width = cubeMapSize;
	target->height = cubeMapSize;
	target->texWidth = cubeMapSize;
	target->texHeight = cubeMapSize;
	target->framebuffer_index = -1;
	target->isCubeMap = true;
	target->impl.format = convert_format(format);
	target->impl.depthBufferBits = depthBufferBits;
	target->impl.stage_depth = -1;
	target->impl.is_cube = true;
	target->impl.current_face = 0;
	target->impl.depth_only = format == KINC_G5_RENDER_TARGET_FORMAT_16BIT_DEPTH;
	if (target->impl.depth_only) {
		target->impl.format = KINC_VK_RENDER_TARGET_DEPTH_FORMAT;
		if (depthBufferBits <= 0) {
			target->impl.depthBufferBits = depthBufferBits = 16;
		}
		target->impl.depth_format = KINC_VK_RENDER_TARGET_DEPTH_FORMAT;
		create_cube_image(KINC_VK_RENDER_TARGET_DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                  VK_IMAGE_ASPECT_DEPTH_BIT, cubeMapSize, &target->impl.depthImage, &target->impl.depthMemory, &target->impl.depthView,
		                  target->impl.face_depth_views);
		target->impl.sourceView = target->impl.depthView;
		memcpy(target->impl.face_views, target->impl.face_depth_views, sizeof(target->impl.face_views));
		target->impl.framebuffer = VK_NULL_HANDLE;
		return;
	}

	create_cube_image(target->impl.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                  VK_IMAGE_ASPECT_COLOR_BIT, cubeMapSize, &target->impl.sourceImage, &target->impl.sourceMemory, &target->impl.sourceView,
	                  target->impl.face_views);
	target->impl.depth_format = kinc_vulkan_depth_format(depthBufferBits);
	if (depthBufferBits > 0) {
		create_cube_image(target->impl.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                  VK_IMAGE_ASPECT_DEPTH_BIT, cubeMapSize, &target->impl.depthImage, &target->impl.depthMemory, &target->impl.depthView,
		                  target->impl.face_depth_views);
	}
	target->impl.framebuffer = VK_NULL_HANDLE;
}

void kinc_g5_render_target_destroy(kinc_g5_render_target_t *target) {
	if (target->framebuffer_index >= 0) {
		framebuffer_count -= 1;
	}
	else {
		// Rare; make sure nothing in flight still uses the images or their framebuffers
		vkQueueWaitIdle(vk_ctx.queue);
		release_rt_framebuffers_using(target->impl.sourceView);
		if (target->impl.is_cube) {
			for (int face = 0; face < 6; ++face) {
				if (!target->impl.depth_only) {
					release_rt_framebuffers_using(target->impl.face_views[face]);
					vkDestroyImageView(vk_ctx.device, target->impl.face_views[face], NULL);
				}
				if (target->impl.depthBufferBits > 0) {
					release_rt_framebuffers_using(target->impl.face_depth_views[face]);
					vkDestroyImageView(vk_ctx.device, target->impl.face_depth_views[face], NULL);
				}
			}
		}
		if (target->impl.depthBufferBits > 0 && !target->impl.depth_borrowed) {
			release_rt_framebuffers_using(target->impl.depthView);
			vkDestroyImageView(vk_ctx.device, target->impl.depthView, NULL);
			vkDestroyImage(vk_ctx.device, target->impl.depthImage, NULL);
			vkFreeMemory(vk_ctx.device, target->impl.depthMemory, NULL);
		}
		if (!target->impl.depth_only) {
			vkDestroyImageView(vk_ctx.device, target->impl.sourceView, NULL);
			vkDestroyImage(vk_ctx.device, target->impl.sourceImage, NULL);
			vkFreeMemory(vk_ctx.device, target->impl.sourceMemory, NULL);
		}
	}
}

void kinc_g5_render_target_set_depth_stencil_from(kinc_g5_render_target_t *target, kinc_g5_render_target_t *source) {
	target->impl.depthImage = source->impl.depthImage;
	target->impl.depthMemory = source->impl.depthMemory;
	target->impl.depthView = source->impl.depthView;
	target->impl.depthBufferBits = source->impl.depthBufferBits;
	target->impl.depth_format = source->impl.depth_format;
	// The source owns the depth image; framebuffers pick up the new view via the cache key
	target->impl.depth_borrowed = true;
}
