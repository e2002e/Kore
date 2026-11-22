#pragma once

#include <vulkan/vulkan_core.h>

typedef struct kinc_g5_sampler_impl {
	VkSampler sampler;
	// Same options with depth comparison forced off / on. Which one a slot uses follows the shader's
	// declared sampler type (shadow or not), not slot state left behind by earlier draws.
	VkSampler sampler_plain;
	VkSampler sampler_compare;
} kinc_g5_sampler_impl_t;
