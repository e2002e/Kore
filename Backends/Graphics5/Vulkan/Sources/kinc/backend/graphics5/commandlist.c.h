#include "kinc/graphics5/sampler.h"
#include "vulkan.h"

#include <kinc/graphics5/commandlist.h>
#include <kinc/graphics5/compute.h>
#include <kinc/graphics5/indexbuffer.h>
#include <kinc/graphics5/pipeline.h>
#include <kinc/graphics5/vertexbuffer.h>
#include <kinc/log.h>
#include <kinc/graphics5/constantbuffer.h>
#include <kinc/window.h>
#include <vulkan/vulkan_core.h>

extern kinc_g5_texture_t *vulkanTextures[KINC_VK_TEXTURE_SLOTS];
extern kinc_g5_render_target_t *vulkanRenderTargets[KINC_VK_TEXTURE_SLOTS];
extern VkSampler vulkanSamplersPlain[KINC_VK_TEXTURE_SLOTS];
extern VkSampler vulkanSamplersCompare[KINC_VK_TEXTURE_SLOTS];
extern VkSampler vulkanSamplersLinear[KINC_VK_TEXTURE_SLOTS];
extern bool vulkanRenderTargetsDepth[KINC_VK_TEXTURE_SLOTS];
VkDescriptorSet getDescriptorSet(void);
static VkDescriptorSet get_compute_descriptor_set(void);
bool memory_type_from_properties(uint32_t typeBits, VkFlags requirements_mask, uint32_t *typeIndex);
void setImageLayout(VkCommandBuffer _buffer, VkImage image, VkImageAspectFlags aspectMask, VkImageLayout oldImageLayout, VkImageLayout newImageLayout);

VkRenderPassBeginInfo currentRenderPassBeginInfo;
VkPipeline currentVulkanPipeline;
kinc_g5_render_target_t *currentRenderTargets[8] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static bool onBackBuffer = false;
static uint32_t lastVertexConstantBufferOffset = 0;
static uint32_t lastFragmentConstantBufferOffset = 0;
static uint32_t lastComputeConstantBufferOffset = 0;
static kinc_g5_pipeline_t *currentPipeline = NULL;
static kinc_g5_compute_shader *current_compute_shader = NULL;

// Pipelines (graphics and compute) can't be freed on destroy: the command buffer being recorded (or
// one in flight) may still reference them. Queue them and free once the queue is idle, at the next
// list begin. Either handle may be VK_NULL_HANDLE.
typedef struct {
	VkPipeline pipeline;
	VkPipelineLayout layout;
} pending_pipeline_release;
static pending_pipeline_release *pending_pipeline_releases = NULL;
static int pending_pipeline_release_count = 0;
static int pending_pipeline_release_capacity = 0;

static void queue_pipeline_release(VkPipeline pipeline, VkPipelineLayout layout) {
	if (pending_pipeline_release_count == pending_pipeline_release_capacity) {
		int capacity = pending_pipeline_release_capacity == 0 ? 16 : pending_pipeline_release_capacity * 2;
		pending_pipeline_release *grown = (pending_pipeline_release *)realloc(pending_pipeline_releases, capacity * sizeof(pending_pipeline_release));
		if (grown == NULL) {
			// Out of memory: fall back to a blocking release
			vkDeviceWaitIdle(vk_ctx.device);
			vkDestroyPipeline(vk_ctx.device, pipeline, NULL);
			vkDestroyPipelineLayout(vk_ctx.device, layout, NULL);
			return;
		}
		pending_pipeline_releases = grown;
		pending_pipeline_release_capacity = capacity;
	}
	pending_pipeline_releases[pending_pipeline_release_count].pipeline = pipeline;
	pending_pipeline_releases[pending_pipeline_release_count].layout = layout;
	++pending_pipeline_release_count;
}

static void flush_pipeline_releases(void) {
	if (pending_pipeline_release_count == 0) {
		return;
	}
	// Rare (only after a pipeline or compute shader was destroyed), so a full idle wait is acceptable.
	// Assumes no other command list is mid-recording with a destroyed shader (G4onG5 uses one list).
	vkQueueWaitIdle(vk_ctx.queue);
	for (int i = 0; i < pending_pipeline_release_count; ++i) {
		vkDestroyPipeline(vk_ctx.device, pending_pipeline_releases[i].pipeline, NULL);
		vkDestroyPipelineLayout(vk_ctx.device, pending_pipeline_releases[i].layout, NULL);
	}
	pending_pipeline_release_count = 0;
}

VkPipeline kinc_vulkan_internal_get_pipeline(kinc_g5_pipeline_t *pipeline, VkRenderPass pass, int color_count);

// Render pass the current/next draws are recorded in, and its color attachment count
static VkRenderPass current_render_pass = VK_NULL_HANDLE;
static int current_color_count = 1;
static bool current_pass_has_depth = false;

static bool in_render_pass = false;
static kinc_g5_command_list_t *recording_list = NULL; // list between begin and end, for texture clears

// Asserts are compiled out in release builds; a lost device would otherwise go unnoticed and every
// later wait would "succeed" immediately.
static void check_fence_wait(VkResult err) {
	static bool reported = false;
	if (err != VK_SUCCESS && !reported) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "Vulkan fence wait failed (%d)%s", (int)err, err == VK_ERROR_DEVICE_LOST ? ": device lost" : "");
		reported = true;
	}
}

static void bind_current_pipeline(kinc_g5_command_list_t *list) {
	if (currentPipeline == NULL || current_render_pass == VK_NULL_HANDLE) {
		return;
	}
	currentVulkanPipeline = kinc_vulkan_internal_get_pipeline(currentPipeline, current_render_pass, current_color_count);
	if (currentVulkanPipeline != VK_NULL_HANDLE) {
		vkCmdBindPipeline(list->impl._buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, currentVulkanPipeline);
	}
}

static void endPass(kinc_g5_command_list_t *list) {
	if (in_render_pass) {
		vkCmdEndRenderPass(list->impl._buffer);
		in_render_pass = false;
	}

	for (int i = 0; i < KINC_VK_TEXTURE_SLOTS; ++i) {
		vulkanTextures[i] = NULL;
		vulkanRenderTargets[i] = NULL;
		vulkanRenderTargetsDepth[i] = false;
	}
}

static int formatSize(VkFormat format) {
	switch (format) {
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		return 16;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return 8;
	case VK_FORMAT_R16_SFLOAT:
		return 2;
	case VK_FORMAT_R8_UNORM:
		return 1;
	default:
		return 4;
	}
}

void set_image_layout(VkImage image, VkImageAspectFlags aspectMask, VkImageLayout old_image_layout, VkImageLayout new_image_layout) {
	VkResult err;

	if (vk_ctx.setup_cmd == VK_NULL_HANDLE) {
		VkCommandBufferAllocateInfo cmd = {0};
		cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmd.pNext = NULL;
		cmd.commandPool = vk_ctx.cmd_pool;
		cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmd.commandBufferCount = 1;

		err = vkAllocateCommandBuffers(vk_ctx.device, &cmd, &vk_ctx.setup_cmd);
		assert(!err);

		VkCommandBufferBeginInfo cmd_buf_info = {0};
		cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		cmd_buf_info.pNext = NULL;
		cmd_buf_info.flags = 0;
		cmd_buf_info.pInheritanceInfo = NULL;

		err = vkBeginCommandBuffer(vk_ctx.setup_cmd, &cmd_buf_info);
		assert(!err);
	}

	VkImageMemoryBarrier image_memory_barrier = {0};
	image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_memory_barrier.pNext = NULL;
	image_memory_barrier.srcAccessMask = 0;
	image_memory_barrier.dstAccessMask = 0;
	image_memory_barrier.oldLayout = old_image_layout;
	image_memory_barrier.newLayout = new_image_layout;
	image_memory_barrier.image = image;
	image_memory_barrier.subresourceRange.aspectMask = aspectMask;
	image_memory_barrier.subresourceRange.baseMipLevel = 0;
	image_memory_barrier.subresourceRange.levelCount = 1;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount = 1;

	if (old_image_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
		image_memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	}
	if (old_image_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
		// Host writes done before the submit are made visible by the submit itself
		image_memory_barrier.srcAccessMask = 0;
	}
	if (new_image_layout == VK_IMAGE_LAYOUT_GENERAL) {
		image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	}

	if (new_image_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
		image_memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	}

	if (new_image_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		// Make sure anything that was copying from this image has completed
		image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	}

	if (new_image_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		image_memory_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	}

	if (new_image_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		image_memory_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}

	if (new_image_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		// Make sure any Copy or CPU writes to image are flushed
		image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	}

	VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	if (new_image_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	}
	if (new_image_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}

	vkCmdPipelineBarrier(vk_ctx.setup_cmd, srcStageMask, dstStageMask, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier);
}

void setup_init_cmd() {
	if (vk_ctx.setup_cmd == VK_NULL_HANDLE) {
		VkCommandBufferAllocateInfo cmd = {0};
		cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmd.pNext = NULL;
		cmd.commandPool = vk_ctx.cmd_pool;
		cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmd.commandBufferCount = 1;

		VkResult err = vkAllocateCommandBuffers(vk_ctx.device, &cmd, &vk_ctx.setup_cmd);
		assert(!err);

		VkCommandBufferBeginInfo cmd_buf_info = {0};
		cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		cmd_buf_info.pNext = NULL;
		cmd_buf_info.flags = 0;
		cmd_buf_info.pInheritanceInfo = NULL;

		err = vkBeginCommandBuffer(vk_ctx.setup_cmd, &cmd_buf_info);
		assert(!err);
	}
}

void flush_init_cmd() {
	VkResult err;

	if (vk_ctx.setup_cmd == VK_NULL_HANDLE)
		return;

	err = vkEndCommandBuffer(vk_ctx.setup_cmd);
	assert(!err);

	const VkCommandBuffer cmd_bufs[] = {vk_ctx.setup_cmd};
	VkFence nullFence = {VK_NULL_HANDLE};
	VkSubmitInfo submit_info = {0};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext = NULL;
	submit_info.waitSemaphoreCount = 0;
	submit_info.pWaitSemaphores = NULL;
	submit_info.pWaitDstStageMask = NULL;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = cmd_bufs;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores = NULL;

	err = vkQueueSubmit(vk_ctx.queue, 1, &submit_info, nullFence);
	assert(!err);

	err = vkQueueWaitIdle(vk_ctx.queue);
	assert(!err);

	vkFreeCommandBuffers(vk_ctx.device, vk_ctx.cmd_pool, 1, cmd_bufs);
	vk_ctx.setup_cmd = VK_NULL_HANDLE;
}

void set_viewport_and_scissor(kinc_g5_command_list_t *list) {
	VkViewport viewport;
	memset(&viewport, 0, sizeof(viewport));
	VkRect2D scissor;
	memset(&scissor, 0, sizeof(scissor));

	if (currentRenderTargets[0] == NULL || currentRenderTargets[0]->framebuffer_index >= 0) {
		viewport.x = 0;
		viewport.y = (float)vk_ctx.windows[vk_ctx.current_window].height;
		viewport.width = (float)vk_ctx.windows[vk_ctx.current_window].width;
		viewport.height = -(float)vk_ctx.windows[vk_ctx.current_window].height;
		viewport.minDepth = (float)0.0f;
		viewport.maxDepth = (float)1.0f;
		scissor.extent.width = vk_ctx.windows[vk_ctx.current_window].width;
		scissor.extent.height = vk_ctx.windows[vk_ctx.current_window].height;
		scissor.offset.x = 0;
		scissor.offset.y = 0;
	}
	else {
		viewport.x = 0;
		viewport.y = 0; // offscreen targets are not y-flipped (see set_render_targets)
		viewport.width = (float)currentRenderTargets[0]->width;
		viewport.height = (float)currentRenderTargets[0]->height;
		viewport.minDepth = (float)0.0f;
		viewport.maxDepth = (float)1.0f;
		scissor.extent.width = currentRenderTargets[0]->width;
		scissor.extent.height = currentRenderTargets[0]->height;
		scissor.offset.x = 0;
		scissor.offset.y = 0;
	}

	vkCmdSetViewport(list->impl._buffer, 0, 1, &viewport);
	vkCmdSetScissor(list->impl._buffer, 0, 1, &scissor);
}

void kinc_g5_command_list_init(kinc_g5_command_list_t *list) {
	VkCommandBufferAllocateInfo cmd = {0};
	cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd.pNext = NULL;
	cmd.commandPool = vk_ctx.cmd_pool;
	cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd.commandBufferCount = 1;

	VkResult err = vkAllocateCommandBuffers(vk_ctx.device, &cmd, &list->impl._buffer);
	assert(!err);

	VkFenceCreateInfo fenceInfo = {0};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.pNext = NULL;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	err = vkCreateFence(vk_ctx.device, &fenceInfo, NULL, &list->impl.fence);
	assert(!err);

	list->impl._indexCount = 0;
}

void kinc_g5_command_list_destroy(kinc_g5_command_list_t *list) {
	vkFreeCommandBuffers(vk_ctx.device, vk_ctx.cmd_pool, 1, &list->impl._buffer);
	vkDestroyFence(vk_ctx.device, list->impl.fence, NULL);
}

void kinc_g5_command_list_begin(kinc_g5_command_list_t *list) {
	VkResult err = vkWaitForFences(vk_ctx.device, 1, &list->impl.fence, VK_TRUE, UINT64_MAX);
	check_fence_wait(err);

	flush_pipeline_releases();
	recording_list = list;

	vkResetCommandBuffer(list->impl._buffer, 0);
	VkCommandBufferBeginInfo cmd_buf_info = {0};
	cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cmd_buf_info.pNext = NULL;
	cmd_buf_info.flags = 0;
	cmd_buf_info.pInheritanceInfo = NULL;

	VkClearValue clear_values[2];
	memset(clear_values, 0, sizeof(VkClearValue) * 2);
	clear_values[0].color.float32[0] = 0.0f;
	clear_values[0].color.float32[1] = 0.0f;
	clear_values[0].color.float32[2] = 0.0f;
	clear_values[0].color.float32[3] = 1.0f;
	if (vk_ctx.windows[vk_ctx.current_window].depth_bits > 0) {
		clear_values[1].depthStencil.depth = 1.0;
		clear_values[1].depthStencil.stencil = 0;
	}

	VkRenderPassBeginInfo rp_begin = {0};
	rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp_begin.pNext = NULL;
	rp_begin.renderPass = vk_ctx.windows[vk_ctx.current_window].framebuffer_render_pass;
	rp_begin.framebuffer = vk_ctx.windows[vk_ctx.current_window].framebuffers[vk_ctx.windows[vk_ctx.current_window].current_image];
	rp_begin.renderArea.offset.x = 0;
	rp_begin.renderArea.offset.y = 0;
	rp_begin.renderArea.extent.width = vk_ctx.windows[vk_ctx.current_window].width;
	rp_begin.renderArea.extent.height = vk_ctx.windows[vk_ctx.current_window].height;
	rp_begin.clearValueCount = vk_ctx.windows[vk_ctx.current_window].depth_bits > 0 ? 2 : 1;
	rp_begin.pClearValues = clear_values;

	err = vkBeginCommandBuffer(list->impl._buffer, &cmd_buf_info);
	assert(!err);

	VkImageMemoryBarrier prePresentBarrier = {0};
	prePresentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	prePresentBarrier.pNext = NULL;
	prePresentBarrier.srcAccessMask = 0;
	prePresentBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	prePresentBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	prePresentBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	prePresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	prePresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	prePresentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	prePresentBarrier.subresourceRange.baseMipLevel = 0;
	prePresentBarrier.subresourceRange.levelCount = 1;
	prePresentBarrier.subresourceRange.baseArrayLayer = 0;
	prePresentBarrier.subresourceRange.layerCount = 1;

	prePresentBarrier.image = vk_ctx.windows[vk_ctx.current_window].images[vk_ctx.windows[vk_ctx.current_window].current_image];
	VkImageMemoryBarrier *pmemory_barrier = &prePresentBarrier;
	vkCmdPipelineBarrier(list->impl._buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
	                     pmemory_barrier);

	vkCmdBeginRenderPass(list->impl._buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
	currentRenderPassBeginInfo = rp_begin;
	in_render_pass = true;
	current_render_pass = rp_begin.renderPass;
	current_color_count = 1;

	set_viewport_and_scissor(list);

	onBackBuffer = true;
}

// Clears a whole color image (kept in GENERAL layout) in frame order: clears are not allowed inside a
// render pass, so the active pass is ended and resumed (its attachments use LOAD, except the swapchain
// pass, which should not be active while textures are cleared).
void kinc_vulkan_internal_clear_color_image(VkImage image, VkClearColorValue value) {
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (recording_list == NULL) {
		setup_init_cmd();
		vkCmdClearColorImage(vk_ctx.setup_cmd, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
		flush_init_cmd();
		return;
	}
	VkCommandBuffer buffer = recording_list->impl._buffer;
	bool resume = in_render_pass;
	if (in_render_pass) {
		vkCmdEndRenderPass(buffer);
		in_render_pass = false;
	}
	VkMemoryBarrier barrier = {0};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	vkCmdClearColorImage(buffer, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	if (resume) {
		vkCmdBeginRenderPass(buffer, &currentRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
		in_render_pass = true;
	}
}

// External command recording (e.g. NGX/DLSS): returns the frame's command buffer outside of any render
// pass, with earlier work made visible; NULL when no frame is being recorded. Pair with end.
static bool external_resume_pass = false;

VkCommandBuffer kinc_vulkan_begin_external_commands(void) {
	if (recording_list == NULL) {
		return VK_NULL_HANDLE;
	}
	VkCommandBuffer buffer = recording_list->impl._buffer;
	external_resume_pass = in_render_pass;
	if (in_render_pass) {
		vkCmdEndRenderPass(buffer);
		in_render_pass = false;
	}
	VkMemoryBarrier barrier = {0};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	return buffer;
}

void kinc_vulkan_end_external_commands(void) {
	if (recording_list == NULL) {
		return;
	}
	VkCommandBuffer buffer = recording_list->impl._buffer;
	VkMemoryBarrier barrier = {0};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	// External code may have bound its own pipelines and descriptor sets
	if (external_resume_pass) {
		vkCmdBeginRenderPass(buffer, &currentRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
		in_render_pass = true;
		external_resume_pass = false;
	}
	if (currentPipeline != NULL) {
		bind_current_pipeline(recording_list);
	}
}

void kinc_g5_command_list_end(kinc_g5_command_list_t *list) {
	// A compute dispatch may have ended the pass without one being resumed
	if (in_render_pass) {
		vkCmdEndRenderPass(list->impl._buffer);
		in_render_pass = false;
	}

	VkImageMemoryBarrier prePresentBarrier = {0};
	prePresentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	prePresentBarrier.pNext = NULL;
	prePresentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	prePresentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	prePresentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	prePresentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	prePresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	prePresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	prePresentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	prePresentBarrier.subresourceRange.baseMipLevel = 0;
	prePresentBarrier.subresourceRange.levelCount = 1;
	prePresentBarrier.subresourceRange.baseArrayLayer = 0;
	prePresentBarrier.subresourceRange.layerCount = 1;

	prePresentBarrier.image = vk_ctx.windows[vk_ctx.current_window].images[vk_ctx.windows[vk_ctx.current_window].current_image];
	VkImageMemoryBarrier *pmemory_barrier = &prePresentBarrier;
	vkCmdPipelineBarrier(list->impl._buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, pmemory_barrier);

	VkResult err = vkEndCommandBuffer(list->impl._buffer);
	assert(!err);
}

void kinc_g5_command_list_clear(kinc_g5_command_list_t *list, struct kinc_g5_render_target *renderTarget, unsigned flags, unsigned color, float depth,
                                int stencil) {
	if (!in_render_pass) {
		return;
	}
	VkClearRect clearRect = {0};
	clearRect.rect.offset.x = 0;
	clearRect.rect.offset.y = 0;
	// The bound pass decides what gets cleared; the passed target can be stale (e.g. after a cube face switch)
	clearRect.rect.extent = currentRenderPassBeginInfo.renderArea.extent;
	clearRect.baseArrayLayer = 0;
	clearRect.layerCount = 1;

	int count = 0;
	VkClearAttachment attachments[9];
	if (flags & KINC_G5_CLEAR_COLOR) {
		VkClearColorValue clearColor = {0};
		clearColor.float32[0] = ((color & 0x00ff0000) >> 16) / 255.0f;
		clearColor.float32[1] = ((color & 0x0000ff00) >> 8) / 255.0f;
		clearColor.float32[2] = (color & 0x000000ff) / 255.0f;
		clearColor.float32[3] = ((color & 0xff000000) >> 24) / 255.0f;
		// Like GL, a color clear clears every bound color target (MRT)
		int color_count = onBackBuffer ? 1 : current_color_count;
		for (int i = 0; i < color_count; ++i) {
			attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			attachments[count].colorAttachment = i;
			attachments[count].clearValue.color = clearColor;
			count++;
		}
	}
	bool pass_has_depth = onBackBuffer ? vk_ctx.windows[vk_ctx.current_window].depth_bits > 0 : current_pass_has_depth;
	if (((flags & KINC_G5_CLEAR_DEPTH) || (flags & KINC_G5_CLEAR_STENCIL)) && pass_has_depth) {
		attachments[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; // | VK_IMAGE_ASPECT_STENCIL_BIT;
		attachments[count].clearValue.depthStencil.depth = depth;
		attachments[count].clearValue.depthStencil.stencil = stencil;
		count++;
	}
	if (count > 0) {
		vkCmdClearAttachments(list->impl._buffer, count, attachments, 1, &clearRect);
	}
}

void kinc_g5_command_list_render_target_to_framebuffer_barrier(kinc_g5_command_list_t *list, struct kinc_g5_render_target *renderTarget) {}

void kinc_g5_command_list_framebuffer_to_render_target_barrier(kinc_g5_command_list_t *list, struct kinc_g5_render_target *renderTarget) {}

void kinc_g5_command_list_draw_indexed_vertices(kinc_g5_command_list_t *list) {
	kinc_g5_command_list_draw_indexed_vertices_from_to(list, 0, list->impl._indexCount);
}

void kinc_g5_command_list_draw_indexed_vertices_from_to(kinc_g5_command_list_t *list, int start, int count) {
	vkCmdDrawIndexed(list->impl._buffer, count, 1, start, 0, 0);
}

void kinc_g5_command_list_draw_indexed_vertices_from_to_from(kinc_g5_command_list_t *list, int start, int count, int vertex_offset) {
	vkCmdDrawIndexed(list->impl._buffer, count, 1, start, vertex_offset, 0);
}

void kinc_g5_command_list_draw_indexed_vertices_instanced(kinc_g5_command_list_t *list, int instanceCount) {
	kinc_g5_command_list_draw_indexed_vertices_instanced_from_to(list, instanceCount, 0, list->impl._indexCount);
}

void kinc_g5_command_list_draw_indexed_vertices_instanced_from_to(kinc_g5_command_list_t *list, int instanceCount, int start, int count) {
	vkCmdDrawIndexed(list->impl._buffer, count, instanceCount, start, 0, 0);
}

void kinc_g5_command_list_viewport(kinc_g5_command_list_t *list, int x, int y, int width, int height) {
	VkViewport viewport;
	memset(&viewport, 0, sizeof(viewport));
	viewport.x = (float)x;
	if (onBackBuffer) {
		viewport.y = y + (float)height;
		viewport.height = (float)-height;
	}
	else {
		viewport.y = (float)y;
		viewport.height = (float)height;
	}
	viewport.width = (float)width;
	viewport.minDepth = (float)0.0f;
	viewport.maxDepth = (float)1.0f;
	vkCmdSetViewport(list->impl._buffer, 0, 1, &viewport);
}

void kinc_g5_command_list_scissor(kinc_g5_command_list_t *list, int x, int y, int width, int height) {
	VkRect2D scissor;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = width;
	scissor.extent.height = height;
	scissor.offset.x = x;
	scissor.offset.y = y;
	vkCmdSetScissor(list->impl._buffer, 0, 1, &scissor);
}

void kinc_g5_command_list_disable_scissor(kinc_g5_command_list_t *list) {
	VkRect2D scissor;
	memset(&scissor, 0, sizeof(scissor));
	if (currentRenderTargets[0] == NULL || currentRenderTargets[0]->framebuffer_index >= 0) {
		scissor.extent.width = vk_ctx.windows[vk_ctx.current_window].width;
		scissor.extent.height = vk_ctx.windows[vk_ctx.current_window].height;
	}
	else {
		scissor.extent.width = currentRenderTargets[0]->width;
		scissor.extent.height = currentRenderTargets[0]->height;
	}
	vkCmdSetScissor(list->impl._buffer, 0, 1, &scissor);
}

void kinc_g5_command_list_set_pipeline(kinc_g5_command_list_t *list, struct kinc_g5_pipeline *pipeline) {
	currentPipeline = pipeline;
	lastVertexConstantBufferOffset = 0;
	lastFragmentConstantBufferOffset = 0;

	bind_current_pipeline(list);
}

void kinc_g5_command_list_set_blend_constant(kinc_g5_command_list_t *list, float r, float g, float b, float a) {
	const float blendConstants[4] = {r, g, b, a};
	vkCmdSetBlendConstants(list->impl._buffer, blendConstants);
}

void kinc_g5_command_list_set_vertex_buffers(kinc_g5_command_list_t *list, struct kinc_g5_vertex_buffer **vertexBuffers, int *offsets_, int count) {
// this seems to be a no-op function?
// kinc_g5_internal_vertex_buffer_set(vertexBuffers[0], 0);
#ifdef KINC_WINDOWS
	VkBuffer *buffers = (VkBuffer *)alloca(sizeof(VkBuffer) * count);
	VkDeviceSize *offsets = (VkDeviceSize *)alloca(sizeof(VkDeviceSize) * count);
#else
	VkBuffer buffers[count];
	VkDeviceSize offsets[count];
#endif
	for (int i = 0; i < count; ++i) {
		buffers[i] = vertexBuffers[i]->impl.vertices.buf;
		offsets[i] = (VkDeviceSize)(offsets_[i] * kinc_g5_vertex_buffer_stride(vertexBuffers[i]));
	}
	vkCmdBindVertexBuffers(list->impl._buffer, 0, count, buffers, offsets);
}

void kinc_g5_command_list_set_index_buffer(kinc_g5_command_list_t *list, struct kinc_g5_index_buffer *indexBuffer) {
	list->impl._indexCount = kinc_g5_index_buffer_count(indexBuffer);
	vkCmdBindIndexBuffer(list->impl._buffer, indexBuffer->impl.buf, 0,
	                     indexBuffer->impl.format == KINC_G5_INDEX_BUFFER_FORMAT_16BIT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void kinc_internal_restore_render_target(kinc_g5_command_list_t *list, struct kinc_g5_render_target *target) {
	VkViewport viewport;
	memset(&viewport, 0, sizeof(viewport));
	viewport.x = 0;
	viewport.y = (float)vk_ctx.windows[vk_ctx.current_window].height;
	viewport.width = (float)vk_ctx.windows[vk_ctx.current_window].width;
	viewport.height = -(float)vk_ctx.windows[vk_ctx.current_window].height;
	viewport.minDepth = (float)0.0f;
	viewport.maxDepth = (float)1.0f;
	vkCmdSetViewport(list->impl._buffer, 0, 1, &viewport);
	VkRect2D scissor;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = vk_ctx.windows[vk_ctx.current_window].width;
	scissor.extent.height = vk_ctx.windows[vk_ctx.current_window].height;
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	vkCmdSetScissor(list->impl._buffer, 0, 1, &scissor);

	if (onBackBuffer && in_render_pass) {
		return;
	}

	endPass(list);

	currentRenderTargets[0] = NULL;
	onBackBuffer = true;

	VkClearValue clear_values[2];
	memset(clear_values, 0, sizeof(VkClearValue) * 2);
	clear_values[0].color.float32[0] = 0.0f;
	clear_values[0].color.float32[1] = 0.0f;
	clear_values[0].color.float32[2] = 0.0f;
	clear_values[0].color.float32[3] = 1.0f;
	clear_values[1].depthStencil.depth = 1.0;
	clear_values[1].depthStencil.stencil = 0;
	VkRenderPassBeginInfo rp_begin = {0};
	rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp_begin.pNext = NULL;
	rp_begin.renderPass = vk_ctx.windows[vk_ctx.current_window].framebuffer_render_pass;
	rp_begin.framebuffer = vk_ctx.windows[vk_ctx.current_window].framebuffers[vk_ctx.windows[vk_ctx.current_window].current_image];
	rp_begin.renderArea.offset.x = 0;
	rp_begin.renderArea.offset.y = 0;
	rp_begin.renderArea.extent.width = vk_ctx.windows[vk_ctx.current_window].width;
	rp_begin.renderArea.extent.height = vk_ctx.windows[vk_ctx.current_window].height;
	rp_begin.clearValueCount = 2;
	rp_begin.pClearValues = clear_values;
	vkCmdBeginRenderPass(list->impl._buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
	currentRenderPassBeginInfo = rp_begin;
	in_render_pass = true;
	current_render_pass = rp_begin.renderPass;
	current_color_count = 1;

	bind_current_pipeline(list);
}

void kinc_g5_command_list_set_render_targets(kinc_g5_command_list_t *list, struct kinc_g5_render_target **targets, int count) {
	for (int i = 0; i < count; ++i) {
		currentRenderTargets[i] = targets[i];
	}
	for (int i = count; i < 8; ++i) {
		currentRenderTargets[i] = NULL;
	}

	if (targets[0]->framebuffer_index >= 0) {
		kinc_internal_restore_render_target(list, targets[0]);
		return;
	}

	endPass(list);

	onBackBuffer = false;

	VkClearValue clear_values[9];
	memset(clear_values, 0, sizeof(VkClearValue));
	for (int i = 0; i < count; ++i) {
		clear_values[i].color.float32[0] = 0.0f;
		clear_values[i].color.float32[1] = 0.0f;
		clear_values[i].color.float32[2] = 0.0f;
		clear_values[i].color.float32[3] = 1.0f;
	}
	clear_values[count].depthStencil.depth = 1.0;
	clear_values[count].depthStencil.stencil = 0;

	VkRenderPassBeginInfo rp_begin = {0};
	rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp_begin.pNext = NULL;
	rp_begin.renderArea.offset.x = 0;
	rp_begin.renderArea.offset.y = 0;
	rp_begin.renderArea.extent.width = targets[0]->width;
	rp_begin.renderArea.extent.height = targets[0]->height;
	rp_begin.clearValueCount = count + 1;
	rp_begin.pClearValues = clear_values;

	// Depth-only targets contribute no color attachment
	VkFormat formats[8];
	VkImageView views[9];
	int color_count = 0;
	for (int i = 0; i < count; ++i) {
		if (targets[i]->impl.depth_only) {
			continue;
		}
		formats[color_count] = targets[i]->impl.format;
		views[color_count] = targets[i]->impl.is_cube ? targets[i]->impl.face_views[targets[i]->impl.current_face] : targets[i]->impl.sourceView;
		color_count++;
	}
	bool depth = targets[0]->impl.depthBufferBits > 0;
	if (depth) {
		views[color_count] = targets[0]->impl.is_cube ? targets[0]->impl.face_depth_views[targets[0]->impl.current_face] : targets[0]->impl.depthView;
	}
	current_pass_has_depth = depth;
	rp_begin.renderPass = get_rt_render_pass(formats, color_count, depth ? targets[0]->impl.depth_format : VK_FORMAT_UNDEFINED);
	rp_begin.framebuffer = get_rt_framebuffer(rp_begin.renderPass, views, depth ? color_count + 1 : color_count, targets[0]->width, targets[0]->height);

	vkCmdBeginRenderPass(list->impl._buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
	currentRenderPassBeginInfo = rp_begin;
	in_render_pass = true;
	current_render_pass = rp_begin.renderPass;
	current_color_count = color_count;

	// Offscreen targets are not y-flipped: with a positive viewport their memory layout matches GL
	// (row 0 = NDC y -1 = texture v 0), so sampling and gl_FragCoord behave as on GL. Only the back
	// buffer is flipped; pipeline variants pick the matching front face (create_pipeline_variant).
	VkViewport viewport;
	memset(&viewport, 0, sizeof(viewport));
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = (float)targets[0]->width;
	viewport.height = (float)targets[0]->height;
	viewport.minDepth = (float)0.0f;
	viewport.maxDepth = (float)1.0f;
	vkCmdSetViewport(list->impl._buffer, 0, 1, &viewport);

	VkRect2D scissor;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = targets[0]->width;
	scissor.extent.height = targets[0]->height;
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	vkCmdSetScissor(list->impl._buffer, 0, 1, &scissor);

	bind_current_pipeline(list);
}

void kinc_g5_command_list_upload_index_buffer(kinc_g5_command_list_t *list, struct kinc_g5_index_buffer *buffer) {}

void kinc_g5_command_list_upload_vertex_buffer(kinc_g5_command_list_t *list, struct kinc_g5_vertex_buffer *buffer) {}

void kinc_g5_command_list_upload_texture(kinc_g5_command_list_t *list, struct kinc_g5_texture *texture) {}

void kinc_g5_command_list_get_render_target_pixels(kinc_g5_command_list_t *list, kinc_g5_render_target_t *render_target, uint8_t *data) {
	if (render_target->impl.depth_only || render_target->impl.is_cube) {
		kinc_log(KINC_LOG_LEVEL_WARNING, "Reading back depth-only or cube render targets is not supported on Vulkan.");
		return;
	}
	VkFormat format = render_target->impl.format;
	int formatByteSize = formatSize(format);

	// Create readback buffer
	if (!render_target->impl.readbackBufferCreated) {
		VkBufferCreateInfo buf_info = {0};
		buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buf_info.pNext = NULL;
		buf_info.size = render_target->width * render_target->height * formatByteSize;
		buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		buf_info.flags = 0;
		vkCreateBuffer(vk_ctx.device, &buf_info, NULL, &render_target->impl.readbackBuffer);

		VkMemoryRequirements mem_reqs = {0};
		vkGetBufferMemoryRequirements(vk_ctx.device, render_target->impl.readbackBuffer, &mem_reqs);

		VkMemoryAllocateInfo mem_alloc;
		memset(&mem_alloc, 0, sizeof(VkMemoryAllocateInfo));
		mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mem_alloc.pNext = NULL;
		mem_alloc.allocationSize = 0;
		mem_alloc.memoryTypeIndex = 0;
		mem_alloc.allocationSize = mem_reqs.size;
		memory_type_from_properties(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &mem_alloc.memoryTypeIndex);
		vkAllocateMemory(vk_ctx.device, &mem_alloc, NULL, &render_target->impl.readbackMemory);
		vkBindBufferMemory(vk_ctx.device, render_target->impl.readbackBuffer, render_target->impl.readbackMemory, 0);

		render_target->impl.readbackBufferCreated = true;
	}

	vkCmdEndRenderPass(list->impl._buffer);
	setImageLayout(list->impl._buffer, render_target->impl.sourceImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
	               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy region;
	region.bufferOffset = 0;
	region.bufferRowLength = render_target->width;
	region.bufferImageHeight = render_target->height;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = 0;
	region.imageOffset.x = 0;
	region.imageOffset.y = 0;
	region.imageOffset.z = 0;
	region.imageExtent.width = (uint32_t)render_target->width;
	region.imageExtent.height = (uint32_t)render_target->height;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(list->impl._buffer, render_target->impl.sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, render_target->impl.readbackBuffer, 1,
	                       &region);

	setImageLayout(list->impl._buffer, render_target->impl.sourceImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBeginRenderPass(list->impl._buffer, &currentRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
	in_render_pass = true;

	kinc_g5_command_list_end(list);
	kinc_g5_command_list_execute(list);
	kinc_g5_command_list_wait_for_execution_to_finish(list);
	kinc_g5_command_list_begin(list);

	// Read buffer
	void *p;
	vkMapMemory(vk_ctx.device, render_target->impl.readbackMemory, 0, VK_WHOLE_SIZE, 0, (void **)&p);
	memcpy(data, p, render_target->texWidth * render_target->texHeight * formatByteSize);
	vkUnmapMemory(vk_ctx.device, render_target->impl.readbackMemory);
}

// Debug aid (KINC_DUMP_FRAME, see G4onG5): read back every 3D texture as a raw file
static kinc_g5_texture_t *debug_textures_3d[64];
static int debug_texture_3d_count = 0;

static void kinc_vulkan_debug_register_3d(kinc_g5_texture_t *texture) {
	if (debug_texture_3d_count < 64) {
		debug_textures_3d[debug_texture_3d_count++] = texture;
	}
}

static void kinc_vulkan_debug_unregister_3d(kinc_g5_texture_t *texture) {
	for (int i = 0; i < debug_texture_3d_count; ++i) {
		if (debug_textures_3d[i] == texture) {
			debug_textures_3d[i] = debug_textures_3d[--debug_texture_3d_count];
			return;
		}
	}
}

void kinc_vulkan_debug_dump_3d(kinc_g5_command_list_t *list, const char *dir) {
	for (int i = 0; i < debug_texture_3d_count; ++i) {
		kinc_g5_texture_t *t = debug_textures_3d[i];
		VkDeviceSize size = (VkDeviceSize)t->texWidth * t->texHeight * t->impl.depth * formatSize(t->impl.format);

		VkBufferCreateInfo buf_info = {0};
		buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buf_info.size = size;
		buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VkBuffer buffer;
		vkCreateBuffer(vk_ctx.device, &buf_info, NULL, &buffer);
		VkMemoryRequirements reqs;
		vkGetBufferMemoryRequirements(vk_ctx.device, buffer, &reqs);
		VkMemoryAllocateInfo alloc = {0};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = reqs.size;
		memory_type_from_properties(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &alloc.memoryTypeIndex);
		VkDeviceMemory memory;
		vkAllocateMemory(vk_ctx.device, &alloc, NULL, &memory);
		vkBindBufferMemory(vk_ctx.device, buffer, memory, 0);

		if (in_render_pass) {
			vkCmdEndRenderPass(list->impl._buffer);
		}
		VkMemoryBarrier barrier = {0};
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(list->impl._buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
		VkBufferImageCopy region = {0};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = (uint32_t)t->texWidth;
		region.imageExtent.height = (uint32_t)t->texHeight;
		region.imageExtent.depth = (uint32_t)t->impl.depth;
		vkCmdCopyImageToBuffer(list->impl._buffer, t->impl.texture.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
		if (in_render_pass) {
			vkCmdBeginRenderPass(list->impl._buffer, &currentRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
		}
		kinc_g5_command_list_end(list);
		kinc_g5_command_list_execute(list);
		kinc_g5_command_list_wait_for_execution_to_finish(list);
		kinc_g5_command_list_begin(list);

		void *p;
		vkMapMemory(vk_ctx.device, memory, 0, VK_WHOLE_SIZE, 0, &p);
		char path[512];
		snprintf(path, sizeof(path), "%s/tex3d%02d_%dx%dx%d_vkfmt%d.raw", dir != NULL ? dir : ".", i, t->texWidth, t->texHeight, t->impl.depth, (int)t->impl.format);
		FILE *file = fopen(path, "wb");
		if (file != NULL) {
			fwrite(p, 1, (size_t)size, file);
			fclose(file);
		}
		vkUnmapMemory(vk_ctx.device, memory);
		vkDestroyBuffer(vk_ctx.device, buffer, NULL);
		vkFreeMemory(vk_ctx.device, memory, NULL);
	}
}

void kinc_g5_command_list_texture_to_render_target_barrier(kinc_g5_command_list_t *list, struct kinc_g5_render_target *renderTarget) {
	// render-passes are used to transition render-targets
}

void kinc_g5_command_list_render_target_to_texture_barrier(kinc_g5_command_list_t *list, struct kinc_g5_render_target *renderTarget) {
	// render-passes are used to transition render-targets
}

// The buffers passed in by G4onG5. vk_ctx.*_uniform_buffer is a global guess reset by any later
// kinc_g5_constant_buffer_init, which left bindings unwritten (GPU hang).
static VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
static VkBuffer current_fragment_buffer = VK_NULL_HANDLE;

void kinc_g5_command_list_set_vertex_constant_buffer(kinc_g5_command_list_t *list, struct kinc_g5_constant_buffer *buffer, int offset, size_t size) {
	lastVertexConstantBufferOffset = offset;
	current_vertex_buffer = buffer->impl.buf;
}

void kinc_g5_command_list_set_fragment_constant_buffer(kinc_g5_command_list_t *list, struct kinc_g5_constant_buffer *buffer, int offset, size_t size) {
	lastFragmentConstantBufferOffset = offset;
	current_fragment_buffer = buffer->impl.buf;

	VkDescriptorSet descriptor_set = kinc_vk_get_descriptor_set(
	    currentPipeline->impl.set_layout, current_vertex_buffer, current_fragment_buffer, currentPipeline->impl.texture_kinds,
	    currentPipeline->impl.texture_storage, currentPipeline->impl.texture_formats, currentPipeline->impl.texture_counts,
	    currentPipeline->impl.texture_first_extra, false);
	uint32_t offsets[2] = {lastVertexConstantBufferOffset, lastFragmentConstantBufferOffset};
	vkCmdBindDescriptorSets(list->impl._buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, currentPipeline->impl.pipeline_layout, 0, 1, &descriptor_set, 2, offsets);
}

void kinc_g5_command_list_set_compute_constant_buffer(kinc_g5_command_list_t *list, struct kinc_g5_constant_buffer *buffer, int offset, size_t size) {
	lastComputeConstantBufferOffset = offset;

	VkBuffer compute_buffer = buffer->impl.buf;
	VkDescriptorSet descriptor_set =
	    kinc_vk_get_descriptor_set(current_compute_shader->impl.set_layout, compute_buffer, compute_buffer, current_compute_shader->impl.texture_kinds,
	                               current_compute_shader->impl.texture_storage, current_compute_shader->impl.texture_formats,
	                               current_compute_shader->impl.texture_counts, current_compute_shader->impl.texture_first_extra, true);
	uint32_t offsets[2] = {lastComputeConstantBufferOffset, lastComputeConstantBufferOffset};
	vkCmdBindDescriptorSets(list->impl._buffer, VK_PIPELINE_BIND_POINT_COMPUTE, current_compute_shader->impl.pipeline_layout, 0, 1, &descriptor_set, 2,
	                        offsets);
}

static bool wait_for_framebuffer = false;

static void command_list_should_wait_for_framebuffer(void) {
	wait_for_framebuffer = true;
}

void kinc_g5_command_list_execute(kinc_g5_command_list_t *list) {
	// Make sure the previous execution is done, so we can reuse the fence
	// Not optimal of course
	VkResult err = vkWaitForFences(vk_ctx.device, 1, &list->impl.fence, VK_TRUE, UINT64_MAX);
	check_fence_wait(err);
	vkResetFences(vk_ctx.device, 1, &list->impl.fence);

	VkPipelineStageFlags pipe_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	VkSubmitInfo submit_info = {0};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext = NULL;

	VkSemaphore semaphores[2] = {framebuffer_available, relay_semaphore};
	VkPipelineStageFlags dst_stage_flags[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
	if (wait_for_framebuffer) {
		submit_info.pWaitSemaphores = semaphores;
		submit_info.pWaitDstStageMask = dst_stage_flags;
		submit_info.waitSemaphoreCount = wait_for_relay ? 2 : 1;
		wait_for_framebuffer = false;
	}
	else if (wait_for_relay) {
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &semaphores[1];
		submit_info.pWaitDstStageMask = &dst_stage_flags[1];
	}

	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &list->impl._buffer;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &relay_semaphore;
	wait_for_relay = true;

	err = vkQueueSubmit(vk_ctx.queue, 1, &submit_info, list->impl.fence);
	assert(!err);
}

void kinc_g5_command_list_wait_for_execution_to_finish(kinc_g5_command_list_t *list) {
	VkResult err = vkWaitForFences(vk_ctx.device, 1, &list->impl.fence, VK_TRUE, UINT64_MAX);
	check_fence_wait(err);
}

// Texture slot of a unit: names map to one binding for all stages, so take whichever stage uses it
// (compute units and vertex/geometry-only ones have no fragment index)
static int unit_slot(kinc_g5_texture_unit_t unit) {
	static const int order[] = {KINC_G5_SHADER_TYPE_FRAGMENT, KINC_G5_SHADER_TYPE_COMPUTE, KINC_G5_SHADER_TYPE_VERTEX, KINC_G5_SHADER_TYPE_GEOMETRY};
	for (int i = 0; i < 4; ++i) {
		int slot = unit.stages[order[i]];
		if (slot >= 0 && slot < KINC_VK_TEXTURE_SLOTS) {
			return slot;
		}
	}
	return -1;
}

void kinc_g5_command_list_set_texture(kinc_g5_command_list_t *list, kinc_g5_texture_unit_t unit, kinc_g5_texture_t *texture) {
	int slot = unit_slot(unit);
	if (slot < 0) {
		return;
	}
	vulkanTextures[slot] = texture;
	vulkanRenderTargets[slot] = NULL;
	vulkanRenderTargetsDepth[slot] = false;
}

void kinc_g5_command_list_set_sampler(kinc_g5_command_list_t *list, kinc_g5_texture_unit_t unit, kinc_g5_sampler_t *sampler) {
	int slot = unit_slot(unit);
	if (slot >= 0) {
		vulkanSamplers[slot] = sampler->impl.sampler;
		vulkanSamplersPlain[slot] = sampler->impl.sampler_plain;
		vulkanSamplersCompare[slot] = sampler->impl.sampler_compare;
		vulkanSamplersLinear[slot] = sampler->impl.sampler_linear;
	}
}

void kinc_g5_command_list_set_image_texture(kinc_g5_command_list_t *list, kinc_g5_texture_unit_t unit, kinc_g5_texture_t *texture) {
	int slot = unit_slot(unit);
	if (slot < 0) {
		return;
	}
	vulkanTextures[slot] = texture;
	vulkanRenderTargets[slot] = NULL;
	vulkanRenderTargetsDepth[slot] = false;
}

void kinc_g5_command_list_set_render_target_face(kinc_g5_command_list_t *list, kinc_g5_render_target_t *texture, int face) {
	if (!texture->impl.is_cube || face < 0 || face >= 6) {
		return;
	}
	texture->impl.current_face = face;
	kinc_g5_render_target_t *targets[1] = {texture};
	kinc_g5_command_list_set_render_targets(list, targets, 1);
}

bool kinc_g5_command_list_init_occlusion_query(kinc_g5_command_list_t *list, unsigned *occlusionQuery) {
	return false;
}

void kinc_g5_command_list_delete_occlusion_query(kinc_g5_command_list_t *list, unsigned occlusionQuery) {}

void kinc_g5_command_list_render_occlusion_query(kinc_g5_command_list_t *list, unsigned occlusionQuery, int triangles) {}

bool kinc_g5_command_list_are_query_results_available(kinc_g5_command_list_t *list, unsigned occlusionQuery) {
	return false;
}

void kinc_g5_command_list_get_query_result(kinc_g5_command_list_t *list, unsigned occlusionQuery, unsigned *pixelCount) {}

void kinc_g5_command_list_set_texture_from_render_target(kinc_g5_command_list_t *list, kinc_g5_texture_unit_t unit, kinc_g5_render_target_t *target) {
	int slot = unit_slot(unit);
	if (slot < 0) {
		return;
	}
	target->impl.stage = slot;
	vulkanRenderTargets[slot] = target;
	vulkanRenderTargetsDepth[slot] = false;
	vulkanTextures[slot] = NULL;
}

void kinc_g5_command_list_set_texture_from_render_target_depth(kinc_g5_command_list_t *list, kinc_g5_texture_unit_t unit, kinc_g5_render_target_t *target) {
	int slot = unit_slot(unit);
	if (slot < 0) {
		return;
	}
	target->impl.stage_depth = slot;
	vulkanRenderTargets[slot] = target;
	vulkanRenderTargetsDepth[slot] = true;
	vulkanTextures[slot] = NULL;
}

void kinc_g5_command_list_set_compute_shader(kinc_g5_command_list_t *list, kinc_g5_compute_shader *shader) {
	current_compute_shader = shader;
	vkCmdBindPipeline(list->impl._buffer, VK_PIPELINE_BIND_POINT_COMPUTE, shader->impl.pipeline);
	//**vkCmdBindDescriptorSets(list->impl._buffer, VK_PIPELINE_BIND_POINT_COMPUTE, shader->impl.pipeline_layout, 0, 1, &shader->impl.descriptor_set, 0, 0);
}

void kinc_g5_command_list_compute(kinc_g5_command_list_t *list, int x, int y, int z) {
	if (in_render_pass) {
		vkCmdEndRenderPass(list->impl._buffer);
		in_render_pass = false;
	}

	// Everything recorded before (draws writing storage images, e.g. voxelization into the back buffer
	// pass whose dependencies don't cover shader writes, attachments, clears) must be visible here
	{
		VkMemoryBarrier before = {0};
		before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
		                       VK_ACCESS_TRANSFER_WRITE_BIT;
		before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(list->impl._buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
	}
	vkCmdDispatch(list->impl._buffer, x, y, z);

	// Image writes of this dispatch must be visible to later dispatches, draws and copies
	VkMemoryBarrier barrier = {0};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(list->impl._buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);

	int render_target_count = 0;
	for (int i = 0; i < 8; ++i) {
		if (currentRenderTargets[i] == NULL) {
			break;
		}
		++render_target_count;
	}
	if (render_target_count > 0) {
		kinc_g5_command_list_set_render_targets(list, currentRenderTargets, render_target_count);
	}
}
