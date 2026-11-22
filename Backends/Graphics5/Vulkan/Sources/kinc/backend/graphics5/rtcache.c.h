// Render-pass / framebuffer caches for offscreen render targets, plus the dummy texture used for
// unbound sampler slots.
//
// Every offscreen render-target image lives permanently in VK_IMAGE_LAYOUT_GENERAL (the layout the
// descriptor sets already declare), so passes simply LOAD/STORE and never transition layouts. The
// external subpass dependencies order earlier writes/reads against the pass and the pass's writes
// against later sampling, compute and transfers.
//
// Render passes are keyed by attachment formats (render-pass compatibility), framebuffers by the
// exact image views. Pipelines are created per render pass on demand (see pipeline.c.h).

#define KINC_VK_RENDER_TARGET_DEPTH_FORMAT VK_FORMAT_D16_UNORM

void setup_init_cmd();
void flush_init_cmd();
void setImageLayout(VkCommandBuffer _buffer, VkImage image, VkImageAspectFlags aspectMask, VkImageLayout oldImageLayout, VkImageLayout newImageLayout);

typedef struct {
	int color_count;
	VkFormat formats[8];
	bool depth;
	VkRenderPass pass;
} rt_render_pass_entry;

static rt_render_pass_entry *rt_render_passes = NULL;
static int rt_render_pass_count = 0;
static int rt_render_pass_capacity = 0;

static VkRenderPass get_rt_render_pass(const VkFormat *formats, int color_count, bool depth) {
	for (int i = 0; i < rt_render_pass_count; ++i) {
		rt_render_pass_entry *entry = &rt_render_passes[i];
		if (entry->color_count == color_count && entry->depth == depth && memcmp(entry->formats, formats, sizeof(VkFormat) * color_count) == 0) {
			return entry->pass;
		}
	}

	VkAttachmentDescription attachments[9];
	memset(attachments, 0, sizeof(attachments));
	VkAttachmentReference color_references[8];
	for (int i = 0; i < color_count; ++i) {
		attachments[i].format = formats[i];
		attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[i].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
		attachments[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
		color_references[i].attachment = i;
		color_references[i].layout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkAttachmentReference depth_reference = {0};
	if (depth) {
		attachments[color_count].format = KINC_VK_RENDER_TARGET_DEPTH_FORMAT;
		attachments[color_count].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[color_count].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[color_count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[color_count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[color_count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[color_count].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
		attachments[color_count].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
		depth_reference.attachment = color_count;
		depth_reference.layout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkSubpassDescription subpass = {0};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = color_count;
	subpass.pColorAttachments = color_references;
	subpass.pDepthStencilAttachment = depth ? &depth_reference : NULL;

	const VkPipelineStageFlags graphics_stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
	                                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
	                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	const VkAccessFlags attachment_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkSubpassDependency dependencies[2];
	memset(dependencies, 0, sizeof(dependencies));

	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = graphics_stages | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].dstStageMask = graphics_stages;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
	                                VK_ACCESS_TRANSFER_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | attachment_access;

	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = graphics_stages;
	dependencies[1].dstStageMask = graphics_stages | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | attachment_access | VK_ACCESS_TRANSFER_READ_BIT;

	VkRenderPassCreateInfo rp_info = {0};
	rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp_info.attachmentCount = depth ? color_count + 1 : color_count;
	rp_info.pAttachments = attachments;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;
	rp_info.dependencyCount = 2;
	rp_info.pDependencies = dependencies;

	VkRenderPass pass;
	VkResult err = vkCreateRenderPass(vk_ctx.device, &rp_info, NULL, &pass);
	assert(!err);

	if (rt_render_pass_count == rt_render_pass_capacity) {
		rt_render_pass_capacity = rt_render_pass_capacity == 0 ? 16 : rt_render_pass_capacity * 2;
		rt_render_passes = (rt_render_pass_entry *)realloc(rt_render_passes, rt_render_pass_capacity * sizeof(rt_render_pass_entry));
		assert(rt_render_passes != NULL);
	}
	rt_render_pass_entry *entry = &rt_render_passes[rt_render_pass_count++];
	memset(entry, 0, sizeof(*entry));
	entry->color_count = color_count;
	memcpy(entry->formats, formats, sizeof(VkFormat) * color_count);
	entry->depth = depth;
	entry->pass = pass;
	return pass;
}

typedef struct {
	VkRenderPass pass;
	int view_count;
	VkImageView views[9];
	uint32_t width;
	uint32_t height;
	VkFramebuffer framebuffer;
} rt_framebuffer_entry;

static rt_framebuffer_entry *rt_framebuffers = NULL;
static int rt_framebuffer_count = 0;
static int rt_framebuffer_capacity = 0;

static VkFramebuffer get_rt_framebuffer(VkRenderPass pass, const VkImageView *views, int view_count, uint32_t width, uint32_t height) {
	for (int i = 0; i < rt_framebuffer_count; ++i) {
		rt_framebuffer_entry *entry = &rt_framebuffers[i];
		if (entry->pass == pass && entry->view_count == view_count && entry->width == width && entry->height == height &&
		    memcmp(entry->views, views, sizeof(VkImageView) * view_count) == 0) {
			return entry->framebuffer;
		}
	}

	VkFramebufferCreateInfo info = {0};
	info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	info.renderPass = pass;
	info.attachmentCount = view_count;
	info.pAttachments = views;
	info.width = width;
	info.height = height;
	info.layers = 1;

	VkFramebuffer framebuffer;
	VkResult err = vkCreateFramebuffer(vk_ctx.device, &info, NULL, &framebuffer);
	assert(!err);

	if (rt_framebuffer_count == rt_framebuffer_capacity) {
		rt_framebuffer_capacity = rt_framebuffer_capacity == 0 ? 32 : rt_framebuffer_capacity * 2;
		rt_framebuffers = (rt_framebuffer_entry *)realloc(rt_framebuffers, rt_framebuffer_capacity * sizeof(rt_framebuffer_entry));
		assert(rt_framebuffers != NULL);
	}
	rt_framebuffer_entry *entry = &rt_framebuffers[rt_framebuffer_count++];
	memset(entry, 0, sizeof(*entry));
	entry->pass = pass;
	entry->view_count = view_count;
	memcpy(entry->views, views, sizeof(VkImageView) * view_count);
	entry->width = width;
	entry->height = height;
	entry->framebuffer = framebuffer;
	return framebuffer;
}

// Destroys every cached framebuffer that references the view (call before destroying the view).
// The caller must make sure the GPU no longer uses them.
static void release_rt_framebuffers_using(VkImageView view) {
	if (view == VK_NULL_HANDLE) {
		return;
	}
	int kept = 0;
	for (int i = 0; i < rt_framebuffer_count; ++i) {
		bool uses = false;
		for (int v = 0; v < rt_framebuffers[i].view_count; ++v) {
			if (rt_framebuffers[i].views[v] == view) {
				uses = true;
				break;
			}
		}
		if (uses) {
			vkDestroyFramebuffer(vk_ctx.device, rt_framebuffers[i].framebuffer, NULL);
		}
		else {
			rt_framebuffers[kept++] = rt_framebuffers[i];
		}
	}
	rt_framebuffer_count = kept;
}

// 1x1 (0,0,0,1) texture + sampler bound to sampler slots nothing was assigned to, like an unbound
// GL texture unit. Vulkan requires every statically used descriptor to be valid.
static VkImage dummy_texture_image = VK_NULL_HANDLE;
static VkDeviceMemory dummy_texture_memory = VK_NULL_HANDLE;
static VkImageView dummy_texture_view = VK_NULL_HANDLE;
static VkSampler dummy_sampler = VK_NULL_HANDLE;

static void ensure_dummy_texture(void) {
	if (dummy_texture_view != VK_NULL_HANDLE) {
		return;
	}

	VkImageCreateInfo image = {0};
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_R8G8B8A8_UNORM;
	image.extent.width = 1;
	image.extent.height = 1;
	image.extent.depth = 1;
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkResult err = vkCreateImage(vk_ctx.device, &image, NULL, &dummy_texture_image);
	assert(!err);

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(vk_ctx.device, dummy_texture_image, &reqs);
	VkMemoryAllocateInfo alloc = {0};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	bool pass = memory_type_from_properties(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc.memoryTypeIndex);
	assert(pass);
	err = vkAllocateMemory(vk_ctx.device, &alloc, NULL, &dummy_texture_memory);
	assert(!err);
	err = vkBindImageMemory(vk_ctx.device, dummy_texture_image, dummy_texture_memory, 0);
	assert(!err);

	setup_init_cmd();
	setImageLayout(vk_ctx.setup_cmd, dummy_texture_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(vk_ctx.setup_cmd, dummy_texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	setImageLayout(vk_ctx.setup_cmd, dummy_texture_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	flush_init_cmd();

	VkImageViewCreateInfo view = {0};
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = dummy_texture_image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8G8B8A8_UNORM;
	view.subresourceRange = range;
	err = vkCreateImageView(vk_ctx.device, &view, NULL, &dummy_texture_view);
	assert(!err);

	VkSamplerCreateInfo sampler = {0};
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	err = vkCreateSampler(vk_ctx.device, &sampler, NULL, &dummy_sampler);
	assert(!err);
}
