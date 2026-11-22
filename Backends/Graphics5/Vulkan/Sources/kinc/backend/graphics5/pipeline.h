#pragma once

#include "MiniVulkan.h"

#include "named_number.h"

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
