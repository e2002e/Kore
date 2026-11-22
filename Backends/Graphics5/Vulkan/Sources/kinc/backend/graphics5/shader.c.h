#include <kinc/graphics5/shader.h>

void kinc_g5_shader_init(kinc_g5_shader_t *shader, const void *source, size_t length, kinc_g5_shader_type_t type) {
	shader->impl.length = (int)length;
	shader->impl.id = 0;
	shader->impl.source = (char *)malloc(length + 1);
	for (int i = 0; i < length; ++i) {
		shader->impl.source[i] = ((char *)source)[i];
	}
	shader->impl.source[length] = 0;

	size_t fixed_length = 0;
	uint32_t *fixed = kinc_vulkan_fix_uniform_layout((const uint32_t *)shader->impl.source, length, &fixed_length);
	if (fixed != NULL) {
		free(shader->impl.source);
		shader->impl.source = (char *)fixed;
		shader->impl.length = (int)fixed_length;
	}
}

void kinc_g5_shader_destroy(kinc_g5_shader_t *shader) {
	free(shader->impl.source);
	shader->impl.source = NULL;
}
