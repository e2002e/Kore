#pragma once

#include "MiniVulkan.h"

#include "named_number.h"

#ifndef KINC_VK_TEXTURE_SLOTS
// Texture slots: 0..15 map to bindings 2..17, 16..31 hold elements [1..n-1] of sampler arrays
#define KINC_VK_TEXTURE_SLOTS 32
#endif

struct kinc_g5_shader;

typedef struct PipelineState5Impl_s {
	const char **textures;
	int *textureValues;
	int textureCount;

	// VkPipelines are created per render pass on first use (render-pass compatibility depends on
	// the bound targets' formats), see kinc_vulkan_internal_get_pipeline.
	struct {
		VkRenderPass pass;
		VkPipeline pipeline;
	} variants[16];
	int variant_count;

	// Kept alive for creating variants later
	VkShaderModule vert_shader_module;
	VkShaderModule frag_shader_module;

	// Snapshot of the vertex input state: the input layout structures may not outlive compile
	VkVertexInputBindingDescription vi_bindings[16];
	int vi_binding_count;
	VkVertexInputAttributeDescription vi_attrs[64];
	int vi_attr_count;

	kinc_internal_named_number vertexLocations[KINC_INTERNAL_NAMED_NUMBER_COUNT];
	kinc_internal_named_number fragmentLocations[KINC_INTERNAL_NAMED_NUMBER_COUNT];
	kinc_internal_named_number textureBindings[KINC_INTERNAL_NAMED_NUMBER_COUNT];
	kinc_internal_named_number vertexOffsets[KINC_INTERNAL_NAMED_NUMBER_COUNT];
	kinc_internal_named_number fragmentOffsets[KINC_INTERNAL_NAMED_NUMBER_COUNT];

	VkPipelineLayout pipeline_layout;

	// Image type per sampler slot (enum kinc_vk_texture_kind), for dummies in unbound slots
	uint8_t texture_kinds[KINC_VK_TEXTURE_SLOTS];
	uint8_t texture_storage[KINC_VK_TEXTURE_SLOTS];   // slot is an image load/store binding
	uint32_t texture_formats[KINC_VK_TEXTURE_SLOTS];  // SPIR-V ImageFormat of storage slots
	uint8_t texture_counts[16];      // array length per binding slot (1 for plain samplers)
	uint8_t texture_first_extra[16]; // slot of element [1] of an array binding
	VkDescriptorSetLayout set_layout; // built from the bindings above (descriptors.c.h)
	VkShaderModule geom_shader_module; // optional geometry stage
} PipelineState5Impl;

typedef struct ComputePipelineState5Impl_t {
	int a;
} ComputePipelineState5Impl;

typedef struct {
	int vertexOffset;
	int fragmentOffset;
	int computeOffset;
} ConstantLocation5Impl;

typedef struct {
	int nothing;
} AttributeLocation5Impl;
