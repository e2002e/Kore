// Descriptor sets for graphics pipelines and compute shaders, shaped by what each shader declares.
//
// Binding 0 and 1 are the dynamic uniform storage buffers (vertex/fragment, or compute twice); bindings
// 2..17 are the 16 texture slots. A slot is a combined image sampler or, when the shader declares an
// image for load/store (OpTypeImage Sampled = 2), a storage image. Set layouts are cached by that
// signature, sets by their exact contents: identical sets are reused, otherwise a set of the same layout
// that is free this frame is rewritten, otherwise a new one is allocated.

#define KINC_VK_DESCRIPTOR_SET_COUNT 4096

typedef struct {
	uint8_t storage[16];
	VkShaderStageFlags buffer0_stages;
	VkShaderStageFlags buffer1_stages;
	VkShaderStageFlags image_stages;
	VkDescriptorSetLayout layout;
} kinc_vk_set_layout_entry;

static kinc_vk_set_layout_entry kinc_vk_set_layouts[64];
static int kinc_vk_set_layout_count = 0;
static VkDescriptorPool kinc_vk_descriptor_pool = VK_NULL_HANDLE;

static VkDescriptorSetLayout kinc_vk_get_set_layout(const uint8_t *storage, VkShaderStageFlags buffer0_stages, VkShaderStageFlags buffer1_stages,
                                                    VkShaderStageFlags image_stages) {
	for (int i = 0; i < kinc_vk_set_layout_count; ++i) {
		kinc_vk_set_layout_entry *e = &kinc_vk_set_layouts[i];
		if (memcmp(e->storage, storage, 16) == 0 && e->buffer0_stages == buffer0_stages && e->buffer1_stages == buffer1_stages &&
		    e->image_stages == image_stages) {
			return e->layout;
		}
	}

	VkDescriptorSetLayoutBinding bindings[18];
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = buffer0_stages;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = buffer1_stages;
	for (int i = 0; i < 16; ++i) {
		bindings[i + 2].binding = i + 2;
		bindings[i + 2].descriptorType = storage[i] ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i + 2].descriptorCount = 1;
		bindings[i + 2].stageFlags = image_stages;
	}

	VkDescriptorSetLayoutCreateInfo info = {0};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 18;
	info.pBindings = bindings;
	VkDescriptorSetLayout layout;
	VkResult err = vkCreateDescriptorSetLayout(vk_ctx.device, &info, NULL, &layout);
	assert(!err);

	if (kinc_vk_set_layout_count < 64) {
		kinc_vk_set_layout_entry *e = &kinc_vk_set_layouts[kinc_vk_set_layout_count++];
		memcpy(e->storage, storage, 16);
		e->buffer0_stages = buffer0_stages;
		e->buffer1_stages = buffer1_stages;
		e->image_stages = image_stages;
		e->layout = layout;
	}
	return layout;
}

static void kinc_vk_ensure_descriptor_pool(void) {
	if (kinc_vk_descriptor_pool != VK_NULL_HANDLE) {
		return;
	}
	VkDescriptorPoolSize sizes[3];
	sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
	sizes[0].descriptorCount = 2 * KINC_VK_DESCRIPTOR_SET_COUNT;
	sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[1].descriptorCount = 16 * KINC_VK_DESCRIPTOR_SET_COUNT;
	sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[2].descriptorCount = 4 * KINC_VK_DESCRIPTOR_SET_COUNT;
	VkDescriptorPoolCreateInfo info = {0};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.maxSets = KINC_VK_DESCRIPTOR_SET_COUNT;
	info.poolSizeCount = 3;
	info.pPoolSizes = sizes;
	VkResult err = vkCreateDescriptorPool(vk_ctx.device, &info, NULL, &kinc_vk_descriptor_pool);
	assert(!err);
}

// SPIR-V ImageFormat -> VkFormat, for storage images
static VkFormat kinc_vk_spirv_image_format(uint32_t format) {
	switch (format) {
	case 1:
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	case 2:
		return VK_FORMAT_R16G16B16A16_SFLOAT;
	case 3:
		return VK_FORMAT_R32_SFLOAT;
	case 4:
		return VK_FORMAT_R8G8B8A8_UNORM;
	case 9:
		return VK_FORMAT_R16_SFLOAT;
	case 15:
		return VK_FORMAT_R8_UNORM;
	case 24:
		return VK_FORMAT_R32_SINT;
	case 33:
		return VK_FORMAT_R32_UINT;
	default:
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
}

// Storage-capable 1x1(x1) dummies for unbound image slots, per dimension and format
typedef struct {
	int kind;
	VkFormat format;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
} kinc_vk_storage_dummy;
static kinc_vk_storage_dummy kinc_vk_storage_dummies[16];
static int kinc_vk_storage_dummy_count = 0;

static VkImageView kinc_vk_get_storage_dummy(int kind, VkFormat format) {
	bool volume = kind == KINC_VK_TEXTURE_KIND_3D;
	for (int i = 0; i < kinc_vk_storage_dummy_count; ++i) {
		if (kinc_vk_storage_dummies[i].kind == (volume ? 1 : 0) && kinc_vk_storage_dummies[i].format == format) {
			return kinc_vk_storage_dummies[i].view;
		}
	}
	if (kinc_vk_storage_dummy_count == 16) {
		return kinc_vk_storage_dummies[0].view;
	}
	kinc_vk_storage_dummy *d = &kinc_vk_storage_dummies[kinc_vk_storage_dummy_count++];
	d->kind = volume ? 1 : 0;
	d->format = format;

	VkImageCreateInfo image = {0};
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	image.format = format;
	image.extent.width = 1;
	image.extent.height = 1;
	image.extent.depth = 1;
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkResult err = vkCreateImage(vk_ctx.device, &image, NULL, &d->image);
	assert(!err);
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(vk_ctx.device, d->image, &reqs);
	VkMemoryAllocateInfo alloc = {0};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	memory_type_from_properties(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc.memoryTypeIndex);
	err = vkAllocateMemory(vk_ctx.device, &alloc, NULL, &d->memory);
	assert(!err);
	vkBindImageMemory(vk_ctx.device, d->image, d->memory, 0);

	setup_init_cmd();
	setImageLayout(vk_ctx.setup_cmd, d->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	flush_init_cmd();

	VkImageViewCreateInfo view = {0};
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = d->image;
	view.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	err = vkCreateImageView(vk_ctx.device, &view, NULL, &d->view);
	assert(!err);
	return d->view;
}

// The image infos of the 16 slots for the given shader bindings, from the currently bound textures
static void kinc_vk_fill_image_infos(const uint8_t *kinds, const uint8_t *storage, const uint32_t *formats, VkDescriptorImageInfo *infos, bool compute) {
	memset(infos, 0, sizeof(VkDescriptorImageInfo) * 16);
	for (int i = 0; i < 16; ++i) {
		infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (storage[i]) {
			if (vulkanTextures[i] != NULL && vulkanTextures[i]->impl.storage && vulkanTextures[i]->impl.is_3d == (kinds[i] == KINC_VK_TEXTURE_KIND_3D)) {
				bool uint_format = formats[i] == 33 || formats[i] == 24; // r32ui / r32i
				infos[i].imageView = uint_format && vulkanTextures[i]->impl.uint_view != VK_NULL_HANDLE ? vulkanTextures[i]->impl.uint_view
				                                                                                       : vulkanTextures[i]->impl.texture.view;
			}
			else if (vulkanRenderTargets[i] != NULL && !vulkanRenderTargets[i]->impl.depth_only) {
				infos[i].imageView = vulkanRenderTargets[i]->impl.sourceView;
			}
			if (infos[i].imageView == VK_NULL_HANDLE) {
				infos[i].imageView = kinc_vk_get_storage_dummy(kinds[i], kinc_vk_spirv_image_format(formats[i]));
			}
			continue;
		}

		// Only bind what matches the declared dimension (GL treats a mismatch as an incomplete texture)
		bool want_3d = kinds[i] == KINC_VK_TEXTURE_KIND_3D;
		bool want_cube = kinds[i] == KINC_VK_TEXTURE_KIND_CUBE || kinds[i] == KINC_VK_TEXTURE_KIND_CUBE_DEPTH;
		if (vulkanTextures[i] != NULL) {
			if (vulkanTextures[i]->impl.is_3d == want_3d && !want_cube) {
				infos[i].imageView = vulkanTextures[i]->impl.texture.view;
			}
		}
		else if (vulkanRenderTargets[i] != NULL) {
			if (vulkanRenderTargets[i]->impl.is_cube == want_cube && !want_3d) {
				infos[i].imageView = vulkanRenderTargetsDepth[i] ? vulkanRenderTargets[i]->impl.depthView : vulkanRenderTargets[i]->impl.sourceView;
			}
		}
		if (infos[i].imageView == VK_NULL_HANDLE) {
			// Unbound: a 1x1 dummy of the declared type ((0,0,0,1) color, depth 1.0 = unshadowed)
			kinc_vk_dummy *dummy = get_dummy_texture(kinds[i] != KINC_VK_TEXTURE_KIND_UNKNOWN ? kinds[i] : KINC_VK_TEXTURE_KIND_2D);
			infos[i].imageView = dummy->view;
			infos[i].sampler = dummy->sampler;
			continue;
		}
		// Shadow samplers need depth comparison, everything else must not have it (G4onG5 keeps compare
		// mode as per-slot state, so it would otherwise leak onto later textures)
		bool shadow = kinds[i] == KINC_VK_TEXTURE_KIND_2D_DEPTH || kinds[i] == KINC_VK_TEXTURE_KIND_CUBE_DEPTH;
		infos[i].sampler = shadow ? vulkanSamplersCompare[i] : vulkanSamplersPlain[i];
		// Volumes read by compute shaders filter linearly, like the GL defaults those shaders were written
		// against (2D inputs stay as set: gbuffers hold packed data that must not be filtered)
		if (compute && kinds[i] == KINC_VK_TEXTURE_KIND_3D && vulkanSamplersLinear[i] != VK_NULL_HANDLE) {
			infos[i].sampler = vulkanSamplersLinear[i];
		}
		if (infos[i].sampler == VK_NULL_HANDLE) {
			infos[i].sampler = get_dummy_texture(KINC_VK_TEXTURE_KIND_2D)->sampler;
		}
	}
}

typedef struct {
	VkDescriptorSetLayout layout;
	VkBuffer buffers[2];
	VkDescriptorImageInfo images[16];
	VkDescriptorSet set;
	bool in_use;
} kinc_vk_set_entry;

static kinc_vk_set_entry *kinc_vk_sets = NULL;
static int kinc_vk_set_count = 0;

static void kinc_vk_write_set(kinc_vk_set_entry *entry, const uint8_t *storage) {
	VkDescriptorBufferInfo buffer_infos[2];
	VkWriteDescriptorSet writes[18];
	memset(writes, 0, sizeof(writes));
	int count = 0;
	for (int b = 0; b < 2; ++b) {
		if (entry->buffers[b] == VK_NULL_HANDLE) {
			continue;
		}
		buffer_infos[b].buffer = entry->buffers[b];
		buffer_infos[b].offset = 0;
		buffer_infos[b].range = 256 * sizeof(float);
		writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[count].dstSet = entry->set;
		writes[count].dstBinding = b;
		writes[count].descriptorCount = 1;
		writes[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
		writes[count].pBufferInfo = &buffer_infos[b];
		count++;
	}
	for (int i = 0; i < 16; ++i) {
		writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[count].dstSet = entry->set;
		writes[count].dstBinding = i + 2;
		writes[count].descriptorCount = 1;
		writes[count].descriptorType = storage[i] ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[count].pImageInfo = &entry->images[i];
		count++;
	}
	vkUpdateDescriptorSets(vk_ctx.device, count, writes, 0, NULL);
}

static VkDescriptorSet kinc_vk_get_descriptor_set(VkDescriptorSetLayout layout, VkBuffer buffer0, VkBuffer buffer1, const uint8_t *kinds, const uint8_t *storage,
                                                  const uint32_t *formats, bool compute) {
	kinc_vk_ensure_descriptor_pool();

	kinc_vk_set_entry wanted;
	memset(&wanted, 0, sizeof(wanted));
	wanted.layout = layout;
	wanted.buffers[0] = buffer0;
	wanted.buffers[1] = buffer1;
	kinc_vk_fill_image_infos(kinds, storage, formats, wanted.images, compute);

	kinc_vk_set_entry *free_entry = NULL;
	for (int i = 0; i < kinc_vk_set_count; ++i) {
		kinc_vk_set_entry *e = &kinc_vk_sets[i];
		if (e->layout != layout) {
			continue;
		}
		// Reuse unchanged only within the frame that wrote it: handles of destroyed views/samplers can be
		// handed out again for new objects, so equal handles across frames do not mean equal descriptors
		if (e->in_use && memcmp(e->buffers, wanted.buffers, sizeof(wanted.buffers)) == 0 && memcmp(e->images, wanted.images, sizeof(wanted.images)) == 0) {
			return e->set;
		}
		if (!e->in_use && free_entry == NULL) {
			free_entry = e;
		}
	}

	if (free_entry == NULL) {
		if (kinc_vk_set_count == KINC_VK_DESCRIPTOR_SET_COUNT) {
			static bool warned = false;
			if (!warned) {
				kinc_log(KINC_LOG_LEVEL_WARNING, "Vulkan: descriptor set pool exhausted (%d sets in one frame)", KINC_VK_DESCRIPTOR_SET_COUNT);
				warned = true;
			}
			return kinc_vk_sets[kinc_vk_set_count - 1].set;
		}
		if (kinc_vk_sets == NULL) {
			kinc_vk_sets = (kinc_vk_set_entry *)calloc(KINC_VK_DESCRIPTOR_SET_COUNT, sizeof(kinc_vk_set_entry));
		}
		VkDescriptorSetAllocateInfo alloc = {0};
		alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc.descriptorPool = kinc_vk_descriptor_pool;
		alloc.descriptorSetCount = 1;
		alloc.pSetLayouts = &layout;
		VkDescriptorSet set;
		VkResult err = vkAllocateDescriptorSets(vk_ctx.device, &alloc, &set);
		if (err != VK_SUCCESS) {
			kinc_log(KINC_LOG_LEVEL_ERROR, "Vulkan: could not allocate a descriptor set (%d)", (int)err);
			return VK_NULL_HANDLE;
		}
		free_entry = &kinc_vk_sets[kinc_vk_set_count++];
		free_entry->set = set;
	}

	VkDescriptorSet set = free_entry->set;
	*free_entry = wanted;
	free_entry->set = set;
	free_entry->in_use = true;
	kinc_vk_write_set(free_entry, storage);
	return set;
}

// A view is being destroyed: no set referencing it may be matched as unchanged anymore (it is rewritten
// before its next use)
static void kinc_vk_forget_image_view(VkImageView view) {
	if (view == VK_NULL_HANDLE) {
		return;
	}
	for (int i = 0; i < kinc_vk_set_count; ++i) {
		for (int s = 0; s < 16; ++s) {
			if (kinc_vk_sets[i].images[s].imageView == view) {
				memset(kinc_vk_sets[i].images, 0, sizeof(kinc_vk_sets[i].images));
				break;
			}
		}
	}
}

// End of frame: every set may be rewritten next frame (the next frame waits for this one's fence first)
static void kinc_vk_reuse_descriptor_sets(void) {
	for (int i = 0; i < kinc_vk_set_count; ++i) {
		kinc_vk_sets[i].in_use = false;
	}
}
