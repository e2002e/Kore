#pragma once

#include "MiniVulkan.h"

typedef struct {
	VkImage sourceImage;
	VkDeviceMemory sourceMemory;
	VkImageView sourceView;

	VkImage depthImage;
	VkDeviceMemory depthMemory;
	VkImageView depthView;
	int depthBufferBits;
	VkFormat depth_format;
	bool depth_borrowed; // depth image shared from another target (set_depth_stencil_from)

	VkFramebuffer framebuffer;

	VkFormat format;

	VkBuffer readbackBuffer;
	VkDeviceMemory readbackMemory;
	bool readbackBufferCreated;

	int stage;
	int stage_depth;

	// Cube render targets: sourceView/depthView are CUBE views for sampling, the face views are
	// the per-face 2D attachments selected by kinc_g5_command_list_set_render_target_face
	bool is_cube;
	// Depth-format target (KINC_G5_RENDER_TARGET_FORMAT_16BIT_DEPTH): no color image, the depth image
	// is the attachment and sourceView/face_views alias the depth views (like a GL depth texture)
	bool depth_only;
	int current_face;
	VkImageView face_views[6];
	VkImageView face_depth_views[6];
} RenderTarget5Impl;
