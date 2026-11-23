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
	size_t interface_length = 0;
	uint32_t *interfaced = kinc_vulkan_fix_entry_interface((const uint32_t *)shader->impl.source, (size_t)shader->impl.length, &interface_length);
	if (interfaced != NULL) {
		free(shader->impl.source);
		shader->impl.source = (char *)interfaced;
		shader->impl.length = (int)interface_length;
	}
	// Varying locations: outputs of vertex/geometry stages, inputs of fragment shaders (geometry inputs
	// are per-vertex arrays, their outer dimension takes no extra locations)
	uint32_t storage_class = type == KINC_G5_SHADER_TYPE_FRAGMENT ? 1 : (type == KINC_G5_SHADER_TYPE_VERTEX || type == KINC_G5_SHADER_TYPE_GEOMETRY ? 3 : 0);
	if (storage_class != 0) {
		uint32_t *relocated = kinc_vulkan_fix_interface_locations((const uint32_t *)shader->impl.source, (size_t)shader->impl.length, storage_class);
		if (relocated != NULL) {
			free(shader->impl.source);
			shader->impl.source = (char *)relocated;
		}
	}
}

void kinc_g5_shader_destroy(kinc_g5_shader_t *shader) {
	free(shader->impl.source);
	shader->impl.source = NULL;
}
