// Repairs the global uniform block layout in SPIR-V produced by krafix.
//
// krafix sometimes emits arrays in "_k_global_uniform_buffer_type" without an ArrayStride and sizes
// them as a single element (seen for vec4 arrays that are also passed to functions by value, e.g.
// Armory's shirr[7]). Following members then overlap the array, so uploading one uniform corrupts
// the other. This recomputes the block's member offsets and adds the missing ArrayStride decorations.
// Kore binds these blocks as storage buffers, so the std430 rules apply (arrays of scalars and vec2
// are tightly packed, which is also how kinc_g5_constant_buffer_set_floats writes them). Kore reads the offsets back from the (fixed) SPIR-V, so the CPU side
// follows automatically.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SPV_OP_NAME 5
#define SPV_OP_TYPE_BOOL 20
#define SPV_OP_TYPE_INT 21
#define SPV_OP_TYPE_FLOAT 22
#define SPV_OP_TYPE_VECTOR 23
#define SPV_OP_TYPE_MATRIX 24
#define SPV_OP_TYPE_ARRAY 28
#define SPV_OP_TYPE_STRUCT 30
#define SPV_OP_CONSTANT 43
#define SPV_OP_DECORATE 71
#define SPV_OP_MEMBER_DECORATE 72
#define SPV_DECORATION_ARRAY_STRIDE 6
#define SPV_DECORATION_OFFSET 35

typedef struct {
	uint16_t op;
	uint32_t a, b;           // vector/matrix: component type, count; array: element type, length id
	uint32_t first_word;     // struct: index of its first member type word
	uint32_t member_count;   // struct
	bool has_array_stride;
} spvfix_id;

static bool spvfix_layout(spvfix_id *ids, uint32_t bound, const uint32_t *constants, uint32_t type, uint32_t *size, uint32_t *align, int depth) {
	if (type >= bound || depth > 8) {
		return false;
	}
	spvfix_id *t = &ids[type];
	switch (t->op) {
	case SPV_OP_TYPE_BOOL:
	case SPV_OP_TYPE_INT:
	case SPV_OP_TYPE_FLOAT:
		*size = 4;
		*align = 4;
		return true;
	case SPV_OP_TYPE_VECTOR:
		*size = 4 * t->b;
		*align = t->b == 2 ? 8 : 16;
		return true;
	case SPV_OP_TYPE_MATRIX: // column-major float vec3/vec4 columns: stride 16 in std140 and std430
		*size = 16 * t->b;
		*align = 16;
		return true;
	case SPV_OP_TYPE_ARRAY: { // std430: element size rounded up to its alignment
		uint32_t element_size, element_align;
		if (!spvfix_layout(ids, bound, constants, t->a, &element_size, &element_align, depth + 1) || t->b >= bound) {
			return false;
		}
		uint32_t stride = (element_size + element_align - 1) & ~(element_align - 1);
		*size = stride * constants[t->b];
		*align = element_align;
		return true;
	}
	default:
		return false;
	}
}

// Returns a malloc'd fixed copy (and its size in bytes) or NULL when nothing needed fixing.
static uint32_t *kinc_vulkan_fix_uniform_layout(const uint32_t *code, size_t length, size_t *fixed_length) {
	size_t word_count = length / 4;
	if (word_count < 5 || code[0] != 0x07230203) {
		return NULL;
	}
	uint32_t bound = code[3];
	spvfix_id *ids = (spvfix_id *)calloc(bound, sizeof(spvfix_id));
	uint32_t *constants = (uint32_t *)calloc(bound, sizeof(uint32_t));
	if (ids == NULL || constants == NULL) {
		free(ids);
		free(constants);
		return NULL;
	}

	uint32_t block = 0;
	size_t first_annotation = 0;
	for (size_t i = 5; i < word_count;) {
		uint32_t count = code[i] >> 16;
		uint32_t op = code[i] & 0xffff;
		if (count == 0 || i + count > word_count) {
			break;
		}
		const uint32_t *o = &code[i + 1];
		switch (op) {
		case SPV_OP_NAME:
			if (o[0] < bound && strcmp((const char *)&o[1], "_k_global_uniform_buffer_type") == 0) {
				block = o[0];
			}
			break;
		case SPV_OP_DECORATE:
			if (first_annotation == 0) {
				first_annotation = i;
			}
			if (o[1] == SPV_DECORATION_ARRAY_STRIDE && o[0] < bound) {
				ids[o[0]].has_array_stride = true;
			}
			break;
		case SPV_OP_MEMBER_DECORATE:
			if (first_annotation == 0) {
				first_annotation = i;
			}
			break;
		case SPV_OP_TYPE_BOOL:
		case SPV_OP_TYPE_INT:
		case SPV_OP_TYPE_FLOAT:
			if (o[0] < bound) {
				ids[o[0]].op = (uint16_t)op;
			}
			break;
		case SPV_OP_TYPE_VECTOR:
		case SPV_OP_TYPE_MATRIX:
		case SPV_OP_TYPE_ARRAY:
			if (o[0] < bound) {
				ids[o[0]].op = (uint16_t)op;
				ids[o[0]].a = o[1];
				ids[o[0]].b = o[2];
			}
			break;
		case SPV_OP_TYPE_STRUCT:
			if (o[0] < bound) {
				ids[o[0]].op = (uint16_t)op;
				ids[o[0]].first_word = (uint32_t)(i + 2);
				ids[o[0]].member_count = count - 2;
			}
			break;
		case SPV_OP_CONSTANT:
			if (count >= 4 && o[1] < bound) {
				constants[o[1]] = o[2];
			}
			break;
		}
		i += count;
	}

	uint32_t *result = NULL;
	if (block != 0 && ids[block].op == SPV_OP_TYPE_STRUCT && first_annotation != 0) {
		spvfix_id *s = &ids[block];
		uint32_t member_count = s->member_count;
		uint32_t *offsets = (uint32_t *)malloc(sizeof(uint32_t) * (member_count + 1));
		uint32_t *missing_strides = (uint32_t *)malloc(sizeof(uint32_t) * 2 * (member_count + 1));
		int missing_count = 0;
		bool ok = offsets != NULL && missing_strides != NULL;
		uint32_t offset = 0;
		for (uint32_t m = 0; ok && m < member_count; ++m) {
			uint32_t type = code[s->first_word + m];
			uint32_t size, align;
			if (!spvfix_layout(ids, bound, constants, type, &size, &align, 0)) {
				ok = false;
				break;
			}
			offset = (offset + align - 1) & ~(align - 1);
			offsets[m] = offset;
			offset += size;
			if (ids[type].op == SPV_OP_TYPE_ARRAY && !ids[type].has_array_stride) {
				uint32_t element_size, element_align;
				spvfix_layout(ids, bound, constants, ids[type].a, &element_size, &element_align, 0);
				missing_strides[missing_count * 2] = type;
				missing_strides[missing_count * 2 + 1] = (element_size + element_align - 1) & ~(element_align - 1);
				ids[type].has_array_stride = true;
				missing_count++;
			}
		}

		// Only rewrite when krafix's layout is actually broken: a missing stride or overlapping members
		bool broken = missing_count > 0;
		if (ok && !broken) {
			uint32_t previous_end = 0;
			for (uint32_t m = 0; m < member_count && !broken; ++m) {
				for (size_t i = 5; i < word_count;) {
					uint32_t count = code[i] >> 16;
					if (count == 0) {
						break;
					}
					if ((code[i] & 0xffff) == SPV_OP_MEMBER_DECORATE && code[i + 1] == block && code[i + 2] == m && code[i + 3] == SPV_DECORATION_OFFSET) {
						uint32_t size, align;
						spvfix_layout(ids, bound, constants, code[s->first_word + m], &size, &align, 0);
						if (code[i + 4] < previous_end || (code[i + 4] % align) != 0) {
							broken = true;
						}
						previous_end = code[i + 4] + size;
						break;
					}
					i += count;
				}
			}
		}

		if (ok && broken) {
			size_t new_word_count = word_count + 4 * missing_count;
			result = (uint32_t *)malloc(new_word_count * 4);
			if (result != NULL) {
				memcpy(result, code, first_annotation * 4);
				for (int k = 0; k < missing_count; ++k) {
					uint32_t *d = &result[first_annotation + 4 * k];
					d[0] = (4u << 16) | SPV_OP_DECORATE;
					d[1] = missing_strides[k * 2];
					d[2] = SPV_DECORATION_ARRAY_STRIDE;
					d[3] = missing_strides[k * 2 + 1];
				}
				memcpy(&result[first_annotation + 4 * missing_count], &code[first_annotation], (word_count - first_annotation) * 4);
				for (size_t i = 5; i < new_word_count;) {
					uint32_t count = result[i] >> 16;
					if (count == 0) {
						break;
					}
					if ((result[i] & 0xffff) == SPV_OP_MEMBER_DECORATE && result[i + 1] == block && result[i + 2] < member_count &&
					    result[i + 3] == SPV_DECORATION_OFFSET) {
						result[i + 4] = offsets[result[i + 2]];
					}
					i += count;
				}
				*fixed_length = new_word_count * 4;
			}
		}
		free(offsets);
		free(missing_strides);
	}

	free(ids);
	free(constants);
	return result;
}

// Returns a malloc'd copy of the code whose "_k_global_uniform_buffer_type" member offsets are replaced
// by the offsets of the same-named members in offsets_by_name (another stage's block sharing the same
// buffer), or NULL when nothing changed. Members without a match keep their offset.
static uint32_t *kinc_vulkan_remap_block_offsets(const uint32_t *code, size_t length, kinc_internal_named_number *offsets_by_name, size_t *out_length) {
	size_t word_count = length / 4;
	if (word_count < 5 || code[0] != 0x07230203) {
		return NULL;
	}
	uint32_t *result = (uint32_t *)malloc(length);
	if (result == NULL) {
		return NULL;
	}
	memcpy(result, code, length);

	uint32_t block = 0;
	const char *member_names[64] = {NULL};
	bool changed = false;
	for (size_t i = 5; i < word_count;) {
		uint32_t count = result[i] >> 16;
		uint32_t op = result[i] & 0xffff;
		if (count == 0 || i + count > word_count) {
			break;
		}
		uint32_t *o = &result[i + 1];
		if (op == SPV_OP_NAME && strcmp((const char *)&o[1], "_k_global_uniform_buffer_type") == 0) {
			block = o[0];
		}
		else if (op == 6 /* OpMemberName */ && block != 0 && o[0] == block && o[1] < 64) {
			member_names[o[1]] = (const char *)&o[2];
		}
		else if (op == SPV_OP_MEMBER_DECORATE && block != 0 && o[0] == block && o[2] == SPV_DECORATION_OFFSET && o[1] < 64 && member_names[o[1]] != NULL) {
			bool found = false;
			for (int n = 0; n < KINC_INTERNAL_NAMED_NUMBER_COUNT; ++n) {
				if (offsets_by_name[n].name[0] != 0 && strcmp(offsets_by_name[n].name, member_names[o[1]]) == 0) {
					if (o[3] != offsets_by_name[n].number) {
						o[3] = offsets_by_name[n].number;
						changed = true;
					}
					found = true;
					break;
				}
			}
			if (!found) {
				kinc_log(KINC_LOG_LEVEL_WARNING, "Geometry shader uniform %s is not a fragment shader uniform, it can not be set", member_names[o[1]]);
			}
		}
		i += count;
	}
	if (!changed) {
		free(result);
		return NULL;
	}
	*out_length = length;
	return result;
}

// krafix numbers stage interface variables 0, 1, 2... (by name), one location each, even for arrays
// and matrices that span several locations, so e.g. a vec4[3] at 5 overlaps the variables at 6 and 7.
// GL links varyings by name and never noticed; Vulkan links by location. Each location is shifted by
// the extra locations of the variables before it. Both sides of an interface get the same shift as long
// as they declare the same variables, which keeps them matching.
// storage_class: 1 Input, 3 Output. Returns a malloc'd copy or NULL when nothing changed.
static uint32_t spvfix_location_slots(const uint32_t *code, size_t word_count, uint32_t type, int depth) {
	if (depth > 8) {
		return 1;
	}
	for (size_t i = 5; i < word_count;) {
		uint32_t count = code[i] >> 16;
		uint32_t op = code[i] & 0xffff;
		if (count == 0) {
			break;
		}
		if ((op == SPV_OP_TYPE_MATRIX || op == SPV_OP_TYPE_ARRAY || op == 32 /* OpTypePointer */) && code[i + 1] == type) {
			if (op == 32) {
				return spvfix_location_slots(code, word_count, code[i + 3], depth + 1);
			}
			if (op == SPV_OP_TYPE_MATRIX) {
				return code[i + 3];
			}
			// Array: length is a constant id
			uint32_t length_id = code[i + 3];
			uint32_t length = 1;
			for (size_t j = 5; j < word_count;) {
				uint32_t c = code[j] >> 16;
				if (c == 0) {
					break;
				}
				if ((code[j] & 0xffff) == SPV_OP_CONSTANT && code[j + 2] == length_id) {
					length = code[j + 3];
					break;
				}
				j += c;
			}
			return length * spvfix_location_slots(code, word_count, code[i + 2], depth + 1);
		}
		i += count;
	}
	return 1;
}

static uint32_t *kinc_vulkan_fix_interface_locations(const uint32_t *code, size_t length, uint32_t storage_class) {
	size_t word_count = length / 4;
	if (word_count < 5 || code[0] != 0x07230203) {
		return NULL;
	}
	// Interface variables of the storage class with their pointer type
	uint32_t vars[64], var_types[64];
	int var_count = 0;
	for (size_t i = 5; i < word_count && var_count < 64;) {
		uint32_t count = code[i] >> 16;
		if (count == 0) {
			break;
		}
		if ((code[i] & 0xffff) == 59 /* OpVariable */ && code[i + 3] == storage_class) {
			vars[var_count] = code[i + 2];
			var_types[var_count] = code[i + 1];
			var_count++;
		}
		i += count;
	}
	// Their Location decorations and slot counts
	int locations[64];
	uint32_t slots[64];
	bool any_multi = false;
	for (int v = 0; v < var_count; ++v) {
		locations[v] = -1;
		for (size_t i = 5; i < word_count;) {
			uint32_t count = code[i] >> 16;
			if (count == 0) {
				break;
			}
			if ((code[i] & 0xffff) == SPV_OP_DECORATE && code[i + 1] == vars[v] && code[i + 2] == 30 /* Location */) {
				locations[v] = (int)code[i + 3];
				break;
			}
			i += count;
		}
		slots[v] = spvfix_location_slots(code, word_count, var_types[v], 0);
		if (slots[v] > 1 && locations[v] >= 0) {
			any_multi = true;
		}
	}
	if (!any_multi) {
		return NULL;
	}
	uint32_t *result = (uint32_t *)malloc(length);
	if (result == NULL) {
		return NULL;
	}
	memcpy(result, code, length);
	for (size_t i = 5; i < word_count;) {
		uint32_t count = result[i] >> 16;
		if (count == 0) {
			break;
		}
		if ((result[i] & 0xffff) == SPV_OP_DECORATE && result[i + 2] == 30) {
			for (int v = 0; v < var_count; ++v) {
				if (vars[v] == result[i + 1] && locations[v] >= 0) {
					uint32_t extra = 0;
					for (int w = 0; w < var_count; ++w) {
						if (locations[w] >= 0 && locations[w] < locations[v]) {
							extra += slots[w] - 1;
						}
					}
					result[i + 3] = (uint32_t)locations[v] + extra;
					break;
				}
			}
		}
		i += count;
	}
	return result;
}
