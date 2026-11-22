#include <kinc/backend/graphics4/rendertarget.h>

#include <kinc/graphics4/rendertarget.h>
#include <kinc/graphics5/commandlist.h>
#include <kinc/log.h>
#include <stdio.h>
#include <stdlib.h>

extern kinc_g5_command_list_t commandList;

// Debug aid: with KINC_DUMP_FRAME=<n> set, kinc_g4_end reads back every offscreen render target
// at the end of frame n and writes them as raw files (see kinc_g4_on_g5_internal_debug_dump).
static kinc_g4_render_target_t *debug_render_targets[256];
static int debug_render_target_count = 0;

static void kinc_g4_on_g5_internal_debug_register_render_target(kinc_g4_render_target_t *target) {
	if (debug_render_target_count < 256) {
		debug_render_targets[debug_render_target_count++] = target;
	}
}

static void kinc_g4_on_g5_internal_debug_unregister_render_target(kinc_g4_render_target_t *target) {
	for (int i = 0; i < debug_render_target_count; ++i) {
		if (debug_render_targets[i] == target) {
			debug_render_targets[i] = debug_render_targets[--debug_render_target_count];
			return;
		}
	}
}

static int debug_frame = 0;
static int debug_set_target_calls = 0;

static void debug_dump_now(int frame, int call) {
#ifdef KINC_VULKAN
	const char *dir = getenv("KINC_DUMP_DIR");
	for (int i = 0; i < debug_render_target_count; ++i) {
		kinc_g5_render_target_t *rt = &debug_render_targets[i]->impl._renderTarget;
		if (rt->impl.depth_only || rt->impl.is_cube) {
			continue;
		}
		int bytes_per_pixel = 4;
		switch (rt->impl.format) {
		case VK_FORMAT_R32G32B32A32_SFLOAT:
			bytes_per_pixel = 16;
			break;
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			bytes_per_pixel = 8;
			break;
		case VK_FORMAT_R16_SFLOAT:
			bytes_per_pixel = 2;
			break;
		case VK_FORMAT_R8_UNORM:
			bytes_per_pixel = 1;
			break;
		default:
			break;
		}
		uint8_t *data = (uint8_t *)malloc((size_t)rt->width * rt->height * bytes_per_pixel);
		kinc_g5_command_list_get_render_target_pixels(&commandList, rt, data);
		char path[512];
		snprintf(path, sizeof(path), "%s/rt%02d_%dx%d_vkfmt%d.raw", dir != NULL ? dir : ".", i, rt->width, rt->height, (int)rt->impl.format);
		FILE *file = fopen(path, "wb");
		if (file != NULL) {
			fwrite(data, 1, (size_t)rt->width * rt->height * bytes_per_pixel, file);
			fclose(file);
		}
		free(data);
	}
	kinc_log(KINC_LOG_LEVEL_INFO, "Dumped %d render targets of frame %d (call %d)", debug_render_target_count, frame, call);
#endif
}

static int debug_env_int(const char *name) {
	const char *env = getenv(name);
	return env != NULL ? atoi(env) : -1;
}

// Called at the end of each frame
void kinc_g4_on_g5_internal_debug_dump(void) {
	static int dump_frame = -2;
	if (dump_frame == -2) {
		dump_frame = debug_env_int("KINC_DUMP_FRAME");
	}
	if (debug_frame == dump_frame && debug_env_int("KINC_DUMP_CALL") < 0) {
		debug_dump_now(debug_frame, -1);
	}
	debug_frame++;
	debug_set_target_calls = 0;
}

// Called before every render target switch: KINC_DUMP_CALL=k dumps before the k-th switch of the
// KINC_DUMP_FRAME frame, to inspect a frame pass by pass
void kinc_g4_on_g5_internal_debug_set_target(void) {
	static int dump_frame = -2, dump_call = -2;
	if (dump_frame == -2) {
		dump_frame = debug_env_int("KINC_DUMP_FRAME");
		dump_call = debug_env_int("KINC_DUMP_CALL");
	}
	if (debug_frame == dump_frame && debug_set_target_calls == dump_call) {
		debug_dump_now(debug_frame, dump_call);
	}
	debug_set_target_calls++;
}


void kinc_g4_render_target_init_with_multisampling(kinc_g4_render_target_t *render_target, int width, int height, kinc_g4_render_target_format_t format,
                                                   int depthBufferBits, int stencilBufferBits, int samples_per_pixel) {
	kinc_g5_render_target_init_with_multisampling(&render_target->impl._renderTarget, width, height, (kinc_g5_render_target_format_t)format, depthBufferBits,
	                                              stencilBufferBits, samples_per_pixel);
	render_target->texWidth = render_target->width = width;
	render_target->texHeight = render_target->height = height;
	render_target->impl.state = KINC_INTERNAL_RENDER_TARGET_STATE_RENDER_TARGET;
	kinc_g4_on_g5_internal_debug_register_render_target(render_target);
}

void kinc_g4_render_target_init_cube_with_multisampling(kinc_g4_render_target_t *render_target, int cubeMapSize, kinc_g4_render_target_format_t format,
                                                        int depthBufferBits, int stencilBufferBits, int samples_per_pixel) {
	kinc_g5_render_target_init_cube_with_multisampling(&render_target->impl._renderTarget, cubeMapSize, (kinc_g5_render_target_format_t)format, depthBufferBits,
	                                                   stencilBufferBits, samples_per_pixel);
}

void kinc_g4_render_target_destroy(kinc_g4_render_target_t *render_target) {
	kinc_g4_on_g5_internal_debug_unregister_render_target(render_target);
	kinc_g5_render_target_destroy(&render_target->impl._renderTarget);
}

void kinc_g4_render_target_set_depth_stencil_from(kinc_g4_render_target_t *render_target, kinc_g4_render_target_t *source) {
	kinc_g5_render_target_set_depth_stencil_from(&render_target->impl._renderTarget, &source->impl._renderTarget);
}

void kinc_g4_render_target_get_pixels(kinc_g4_render_target_t *render_target, uint8_t *data) {
	kinc_g5_command_list_get_render_target_pixels(&commandList, &render_target->impl._renderTarget, data);
}

void kinc_g4_render_target_generate_mipmaps(kinc_g4_render_target_t *render_target, int levels) {}
