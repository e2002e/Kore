#pragma once

#include <kinc/backend/graphics5/ShaderHash.h>

#include "MiniVulkan.h"

#include "named_number.h"

#ifndef KINC_VK_TEXTURE_SLOTS
// Texture slots: 0..15 map to bindings 2..17, 16..31 hold elements [1..n-1] of sampler arrays
#define KINC_VK_TEXTURE_SLOTS 32
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kinc_g5_compute_shader_impl {
	kinc_internal_named_number locations[KINC_INTERNAL_NAMED_NUMBER_COUNT];
	kinc_internal_named_number texture_bindings[KINC_INTERNAL_NAMED_NUMBER_COUNT];
	kinc_internal_named_number offsets[KINC_INTERNAL_NAMED_NUMBER_COUNT];

	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkShaderModule shader_module;

	uint8_t texture_kinds[KINC_VK_TEXTURE_SLOTS];
	uint8_t texture_storage[KINC_VK_TEXTURE_SLOTS];
	uint32_t texture_formats[KINC_VK_TEXTURE_SLOTS];
	uint8_t texture_counts[16];
	uint8_t texture_first_extra[16];
	VkDescriptorSetLayout set_layout;
} kinc_g5_compute_shader_impl;

#ifdef __cplusplus
}
#endif
