#pragma once

#include <vulkan/vulkan_core.h>

typedef struct kinc_g5_sampler_impl {
	VkSampler sampler;
	// Same options with depth comparison forced off / on. Which one a slot uses follows the shader's
	// declared sampler type (shadow or not), not slot state left behind by earlier draws.
	VkSampler sampler_plain;
	VkSampler sampler_compare;
	// Linear min/mag variant: for 3D textures read by compute shaders that never set filters (GL keeps
	// linear defaults on such textures, e.g. Armory's voxel volumes)
	VkSampler sampler_linear;
} kinc_g5_sampler_impl_t;
